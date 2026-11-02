#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "shell_internal.h"
#include "parser.h"
#include "script.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { OUTPUT_CHUNK = 4096 };

extern char **environ;

static void
emit_output(Shell *shell, const char *text, size_t length)
{
    if (length == 0) return;
    if (shell->capturing_output) {
        if (shell->capture_failed) return;
        if (shell->capture_length + length + 1 > shell->capture_capacity) {
            size_t cap = shell->capture_capacity == 0 ? 1024 : shell->capture_capacity;
            while (cap < shell->capture_length + length + 1) {
                if (cap > SIZE_MAX / 2) { shell->capture_failed = 1; return; }
                cap *= 2;
            }
            char *grown = realloc(shell->capture_buffer, cap);
            if (grown == NULL) { shell->capture_failed = 1; return; }
            shell->capture_buffer = grown; shell->capture_capacity = cap;
        }
        memcpy(shell->capture_buffer + shell->capture_length, text, length);
        shell->capture_length += length;
        shell->capture_buffer[shell->capture_length] = '\0';
    } else {
        (void)shell_emit(shell, SHELL_EVENT_OUTPUT, text, length, 0);
    }
}

static void
child_write_errno(int fd, int value)
{
    const char *bytes = (const char *)&value;
    size_t remaining = sizeof(value);
    while (remaining != 0) {
        ssize_t amount = write(fd, bytes, remaining);
        if (amount > 0) {
            bytes += amount;
            remaining -= (size_t)amount;
        } else if (amount < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static ssize_t
read_errno_value(int fd, int *value)
{
    char *bytes = (char *)value;
    size_t total = 0;
    while (total < sizeof(*value)) {
        ssize_t amount = read(fd, bytes + total, sizeof(*value) - total);
        if (amount > 0) {
            total += (size_t)amount;
        } else if (amount == 0) {
            break;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return (ssize_t)total;
}

static int
make_pipe_above_stdio(int pipe_fds[2])
{
    int i;

    if (pipe2(pipe_fds, O_CLOEXEC) < 0)
        return -1;
    for (i = 0; i < 2; ++i) {
        if (pipe_fds[i] <= STDERR_FILENO) {
            int moved = fcntl(pipe_fds[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (moved < 0) {
                int saved_errno = errno;
                (void)close(pipe_fds[0]);
                (void)close(pipe_fds[1]);
                errno = saved_errno;
                return -1;
            }
            (void)close(pipe_fds[i]);
            pipe_fds[i] = moved;
        }
    }
    return 0;
}

static char *
absolute_from_cwd(const Shell *shell, const char *path, size_t path_length)
{
    size_t cwd_length;
    char *result;

    if (path_length > 0 && path[0] == '/')
        return strndup(path, path_length);
    cwd_length = strlen(shell->cwd_path);
    if (path_length > SIZE_MAX - 2 || cwd_length > SIZE_MAX - path_length - 2) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    result = malloc(cwd_length + path_length + 2);
    if (result == NULL)
        return NULL;
    memcpy(result, shell->cwd_path, cwd_length);
    result[cwd_length] = '/';
    memcpy(result + cwd_length + 1, path, path_length);
    result[cwd_length + path_length + 1] = '\0';
    return result;
}

static char *
join_directory_command(const char *directory, const char *command)
{
    size_t directory_length = strlen(directory);
    size_t command_length = strlen(command);
    int needs_slash = directory_length == 0 || directory[directory_length - 1] != '/';
    char *result;

    if (directory_length > SIZE_MAX - command_length - (size_t)needs_slash - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    result = malloc(directory_length + (size_t)needs_slash + command_length + 1);
    if (result == NULL)
        return NULL;
    memcpy(result, directory, directory_length);
    if (needs_slash)
        result[directory_length++] = '/';
    memcpy(result + directory_length, command, command_length + 1);
    return result;
}

/* Resolve PATH in the parent, where allocation and access checks are safe. */
static char *
resolve_executable(const Shell *shell, const char *command)
{
    const char *path_value;
    const char *component;
    int saw_access_denied = 0;

    if (strchr(command, '/') != NULL)
        return absolute_from_cwd(shell, command, strlen(command));

    path_value = getenv("PATH");
    if (path_value == NULL)
        path_value = "/bin:/usr/bin";
    component = path_value;
    for (;;) {
        const char *end = strchr(component, ':');
        size_t length = end == NULL ? strlen(component) : (size_t)(end - component);
        char *directory = length == 0
            ? strdup(shell->cwd_path)
            : absolute_from_cwd(shell, component, length);
        char *candidate;
        struct stat candidate_stat;

        if (directory == NULL)
            return NULL;
        candidate = join_directory_command(directory, command);
        free(directory);
        if (candidate == NULL)
            return NULL;
        if (access(candidate, X_OK) == 0 &&
            stat(candidate, &candidate_stat) == 0 &&
            !S_ISDIR(candidate_stat.st_mode))
            return candidate;
        if (errno == EACCES)
            saw_access_denied = 1;
        free(candidate);
        if (end == NULL)
            break;
        component = end + 1;
    }
    errno = saw_access_denied ? EACCES : ENOENT;
    return NULL;
}

static int
emit_error(Shell *shell, const char *prefix, const char *detail)
{
    char message[1024];
    int count = snprintf(message, sizeof(message), "terminator: %s: %s\n",
                         prefix, detail);
    if (count > 0) {
        size_t length = (size_t)count < sizeof(message) ? (size_t)count : sizeof(message) - 1;
        if (shell->capturing_output)
            (void)shell_emit(shell, SHELL_EVENT_OUTPUT, message, length, 0);
        else
            emit_output(shell, message, length);
    }
    return 1;
}

static int
set_cwd(Shell *shell, const char *path)
{
    int new_fd;
    char proc_path[64];
    char resolved[4096];
    ssize_t length;
    char *new_path;

    new_fd = openat(shell->cwd_fd, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (new_fd < 0)
        return emit_error(shell, path, strerror(errno));
    if (new_fd <= STDERR_FILENO) {
        int moved = fcntl(new_fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (moved < 0) {
            int saved_errno = errno;
            (void)close(new_fd);
            return emit_error(shell, path, strerror(saved_errno));
        }
        (void)close(new_fd);
        new_fd = moved;
    }
    (void)snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", new_fd);
    length = readlink(proc_path, resolved, sizeof(resolved) - 1);
    if (length < 0) {
        int saved_errno = errno;
        (void)close(new_fd);
        return emit_error(shell, path, strerror(saved_errno));
    }
    resolved[length] = '\0';
    if (length >= 10 && strcmp(resolved + length - 10, " (deleted)") == 0) {
        (void)close(new_fd);
        return emit_error(shell, path, "directory is no longer available");
    }
    new_path = strdup(resolved);
    if (new_path == NULL) {
        (void)close(new_fd);
        return emit_error(shell, "cd", "out of memory");
    }
    (void)pthread_mutex_lock(&shell->mutex);
    (void)close(shell->cwd_fd);
    shell->cwd_fd = new_fd;
    free(shell->cwd_path);
    shell->cwd_path = new_path;
    (void)pthread_mutex_unlock(&shell->mutex);
    return 0;
}

typedef struct {
    char *executable;
    int input_fd;
    int output_fd;
} PreparedCommand;

static void
job_free(ShellJob *job)
{
    size_t i;
    if (job == NULL)
        return;
    if (job->capture_fd >= 0)
        (void)close(job->capture_fd);
    if (job->stderr_fd >= 0)
        (void)close(job->stderr_fd);
    for (i = 0; i < job->count; ++i) {
        if (job->status_fds != NULL && job->status_fds[i] >= 0)
            (void)close(job->status_fds[i]);
    }
    free(job->status_fds);
    free(job->states);
    free(job->pids);
    free(job->command);
    free(job);
}

static int
job_is_stopped(const ShellJob *job)
{
    size_t i;
    int any_stopped = 0;
    if (job->remaining == 0)
        return 0;
    for (i = 0; i < job->count; ++i) {
        if (job->states[i] == 0)
            return 0;
        if (job->states[i] == 1)
            any_stopped = 1;
    }
    return any_stopped;
}

static int wait_for_job(Shell *shell, ShellJob *job);

static void
job_drain_output(Shell *shell, ShellJob *job)
{
    char buffer[OUTPUT_CHUNK];
    unsigned chunks = 0;
    if (job->capture_fd < 0)
        return;
    for (;;) {
        ssize_t amount = read(job->capture_fd, buffer, sizeof(buffer));
        if (amount > 0) {
            if (job->multiwatch) {
                char header[1200];
                time_t now = time(NULL);
                int n = snprintf(header, sizeof(header), "\"%s\", %lld:\n--------------------\n",
                                 job->command, (long long)now);
                if (n > 0)
                    emit_output(shell, header, (size_t)n < sizeof(header) ? (size_t)n : sizeof(header) - 1);
                emit_output(shell, buffer, (size_t)amount);
                emit_output(shell, "\n--------------------\n", sizeof("\n--------------------\n") - 1);
                job->multiwatch_output_bytes += (size_t)amount;
            } else {
                emit_output(shell, buffer, (size_t)amount);
            }
            if (++chunks >= 16 && job->remaining != 0)
                break;
            continue;
        }
        if (amount == 0) {
            (void)close(job->capture_fd);
            job->capture_fd = -1;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            (void)close(job->capture_fd);
            job->capture_fd = -1;
        }
        if (amount < 0 && errno == EINTR)
            continue;
        break;
    }
}

static void
job_drain_stderr(Shell *shell, ShellJob *job)
{
    char buffer[OUTPUT_CHUNK];
    unsigned chunks = 0;
    if (job->stderr_fd < 0) return;
    for (;;) {
        ssize_t amount = read(job->stderr_fd, buffer, sizeof(buffer));
        if (amount > 0) {
            (void)shell_emit(shell, SHELL_EVENT_OUTPUT, buffer, (size_t)amount, 0);
            if (++chunks >= 16 && job->remaining != 0) break;
            continue;
        }
        if (amount == 0 || (amount < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            (void)close(job->stderr_fd);
            job->stderr_fd = -1;
        }
        if (amount < 0 && errno == EINTR) continue;
        break;
    }
}

static void
job_check_children(Shell *shell, ShellJob *job)
{
    size_t i;
    for (i = 0; i < job->count; ++i) {
        int status;
        pid_t result;
        if (job->states[i] == 2)
            continue;
        do {
            result = waitpid(job->pids[i], &status,
                             WNOHANG | WUNTRACED | WCONTINUED);
        } while (result < 0 && errno == EINTR);
        if (result == 0)
            continue;
        if (result < 0) {
            if (errno == ECHILD) {
                job->states[i] = 2;
                --job->remaining;
            }
            continue;
        }
        if (WIFSTOPPED(status)) {
            job->states[i] = 1;
        } else if (WIFCONTINUED(status)) {
            job->states[i] = 0;
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            job->states[i] = 2;
            --job->remaining;
            if (i + 1 == job->count)
                job->last_status = WIFEXITED(status) ? WEXITSTATUS(status) :
                                   128 + WTERMSIG(status);
            if (job->status_fds[i] >= 0) {
                int exec_errno = 0;
                ssize_t amount = read_errno_value(job->status_fds[i], &exec_errno);
                (void)close(job->status_fds[i]);
                job->status_fds[i] = -1;
                if (amount == (ssize_t)sizeof(exec_errno)) {
                    char message[1024];
                    int length;
                    if (job->multiwatch)
                        length = snprintf(message, sizeof(message),
                                          "\"%s\", %lld:\n--------------------\nterminator: stage %zu: %s\n--------------------\n",
                                          job->command, (long long)time(NULL), i + 1, strerror(exec_errno));
                    else
                        length = snprintf(message, sizeof(message), "terminator: job %u stage %zu: %s\n",
                                          job->id, i + 1, strerror(exec_errno));
                    if (length > 0) {
                        if (job->split_stderr)
                            (void)shell_emit(shell, SHELL_EVENT_OUTPUT, message,
                                             (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1, 0);
                        else
                            emit_output(shell, message,
                                        (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1);
                    }
                    if (job->multiwatch)
                        job->multiwatch_output_bytes += 1;
                }
            }
        }
    }
    if (job->remaining == 0) {
        job_drain_output(shell, job);
        job_drain_stderr(shell, job);
        if (job->multiwatch && job->multiwatch_output_bytes == 0) {
            char message[1200];
            int n = snprintf(message, sizeof(message), "\"%s\", %lld:\n--------------------\n(no output; exit status %d)\n--------------------\n",
                             job->command, (long long)time(NULL), job->last_status);
            if (n > 0)
                emit_output(shell, message, (size_t)n < sizeof(message) ? (size_t)n : sizeof(message) - 1);
        } else if (job->multiwatch && job->last_status != 0) {
            char message[96];
            int n = snprintf(message, sizeof(message), "[exit status %d]\n", job->last_status);
            if (n > 0) emit_output(shell, message, (size_t)n);
        }
        if (job->capture_fd >= 0) {
            (void)close(job->capture_fd);
            job->capture_fd = -1;
        }
        if (job->stderr_fd >= 0) {
            (void)close(job->stderr_fd);
            job->stderr_fd = -1;
        }
        job->completed = 1;
    }
}

void
execute_reap_jobs(Shell *shell)
{
    ShellJob *job;
    for (job = shell->jobs; job != NULL; job = job->next) {
        job_drain_output(shell, job);
        job_drain_stderr(shell, job);
        job_check_children(shell, job);
        if (job->completed && !job->foreground && !job->completion_notified && !job->multiwatch) {
            char message[1024];
            int length = snprintf(message, sizeof(message), "[%u] Done %s\n",
                                  job->id, job->command);
            if (length > 0)
                emit_output(shell, message,
                            (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1);
            job->completion_notified = 1;
        }
        if (!job->completed && !job->foreground && job_is_stopped(job) &&
            !job->stopped_notified) {
            char message[1024];
            int length = snprintf(message, sizeof(message), "[%u] Stopped %s\n",
                                  job->id, job->command);
            if (length > 0)
                emit_output(shell, message,
                            (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1);
            job->stopped_notified = 1;
        }
    }
}

static void
close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static void
close_pipe_array(int (*pipes)[2], size_t count)
{
    size_t i;
    if (pipes == NULL)
        return;
    for (i = 0; i < count; ++i) {
        close_fd(&pipes[i][0]);
        close_fd(&pipes[i][1]);
    }
}

static int
move_above_stdio(int *fd)
{
    if (*fd > STDERR_FILENO)
        return 0;
    int moved = fcntl(*fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0)
        return -1;
    (void)close(*fd);
    *fd = moved;
    return 0;
}

static int
open_redirection(Shell *shell, const Redirection *redirection)
{
    int fd;
    int flags;

    if (redirection->type == REDIRECTION_INPUT)
        flags = O_RDONLY | O_CLOEXEC;
    else if (redirection->type == REDIRECTION_OUTPUT_APPEND)
        flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
    else
        flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
    fd = openat(shell->cwd_fd, redirection->target, flags, 0666);
    if (fd < 0)
        return -1;
    if (move_above_stdio(&fd) < 0) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int
run_pipeline(Shell *shell, Pipeline *pipeline, const char *line,
             int multiwatch, size_t multiwatch_slot)
{
    size_t count = pipeline->count;
    size_t pipe_count = count > 1 ? count - 1 : 0;
    int (*pipes)[2] = NULL;
    int (*statuses)[2] = NULL;
    int capture[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
    int split_stderr = shell->capturing_output;
    PreparedCommand *prepared = NULL;
    pid_t *children = NULL;
    ShellJob *job = NULL;
    pid_t pgid = 0;
    char **child_environment = environ;
    size_t started = 0;
    size_t i;
    int result = 1;
    int job_registered = 0;

    if (pipe_count != 0) {
        if (pipe_count > SIZE_MAX / sizeof(*pipes)) {
            errno = ENOMEM;
            return emit_error(shell, "pipeline", strerror(errno));
        }
        pipes = malloc(pipe_count * sizeof(*pipes));
        if (pipes == NULL)
            return emit_error(shell, "pipeline", "out of memory");
        for (i = 0; i < pipe_count; ++i)
            pipes[i][0] = pipes[i][1] = -1;
    }
    if (count > SIZE_MAX / sizeof(*prepared) || count > SIZE_MAX / sizeof(*children)) {
        errno = ENOMEM;
        goto setup_failure;
    }
    prepared = calloc(count, sizeof(*prepared));
    children = calloc(count, sizeof(*children));
    statuses = malloc(count * sizeof(*statuses));
    job = calloc(1, sizeof(*job));
    if (job != NULL) {
        job->capture_fd = -1;
        job->stderr_fd = -1;
        job->count = count;
        job->remaining = count;
        job->states = calloc(count, sizeof(*job->states));
        job->status_fds = malloc(count * sizeof(*job->status_fds));
        job->command = strdup(line);
        if (job->status_fds != NULL)
            for (i = 0; i < count; ++i)
                job->status_fds[i] = -1;
    }
    if (prepared != NULL) {
        for (i = 0; i < count; ++i) {
            prepared[i].input_fd = -1;
            prepared[i].output_fd = -1;
        }
    }
    if (statuses != NULL) {
        for (i = 0; i < count; ++i)
            statuses[i][0] = statuses[i][1] = -1;
    }
    if (prepared == NULL || children == NULL || statuses == NULL || job == NULL ||
        job->states == NULL || job->status_fds == NULL || job->command == NULL)
        goto setup_failure;
    for (i = 0; i < count; ++i) {
        prepared[i].executable = resolve_executable(shell, pipeline->commands[i].argv[0]);
        if (prepared[i].executable == NULL) {
            int saved_errno = errno;
            (void)emit_error(shell, pipeline->commands[i].argv[0], strerror(saved_errno));
            result = saved_errno == EACCES ? 126 : 127;
            goto cleanup;
        }
        for (size_t r = 0; r < pipeline->commands[i].redirection_count; ++r) {
            const Redirection *redirection = &pipeline->commands[i].redirections[r];
            int fd = open_redirection(shell, redirection);
            if (fd < 0) {
                int saved_errno = errno;
                (void)emit_error(shell, redirection->target, strerror(saved_errno));
                result = 1;
                goto cleanup;
            }
            if (redirection->type == REDIRECTION_INPUT)
                prepared[i].input_fd = fd;
            else
                prepared[i].output_fd = fd;
        }
    }
    if (make_pipe_above_stdio(capture) < 0) {
        (void)emit_error(shell, "pipeline", strerror(errno));
        goto cleanup;
    }
    if (split_stderr && make_pipe_above_stdio(error_pipe) < 0) {
        (void)emit_error(shell, "command substitution stderr", strerror(errno));
        goto cleanup;
    }
    for (i = 0; i < count; ++i) {
        if (make_pipe_above_stdio(statuses[i]) < 0) {
            (void)emit_error(shell, "pipeline", strerror(errno));
            goto cleanup;
        }
    }
    for (i = 0; i < pipe_count; ++i) {
        if (make_pipe_above_stdio(pipes[i]) < 0) {
            (void)emit_error(shell, "pipeline", strerror(errno));
            goto cleanup;
        }
    }
    {
        int flags = fcntl(capture[0], F_GETFL);
        if (flags < 0 || fcntl(capture[0], F_SETFL, flags | O_NONBLOCK) < 0) {
            (void)emit_error(shell, "pipeline capture", strerror(errno));
            goto cleanup;
        }
    }
    if (split_stderr) {
        int flags = fcntl(error_pipe[0], F_GETFL);
        if (flags < 0 || fcntl(error_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0) {
            (void)emit_error(shell, "command substitution stderr", strerror(errno));
            goto cleanup;
        }
    }

    (void)pthread_mutex_lock(&shell->mutex);
    if (shell->stopping) {
        (void)pthread_mutex_unlock(&shell->mutex);
        result = 128;
        goto cleanup;
    }
    for (i = 0; i < count; ++i) {
        pid_t child = fork();
        if (child == 0) {
            int saved_errno;
            pid_t wanted_pgid = pgid == 0 ? 0 : pgid;
            if (setpgid(0, wanted_pgid) < 0)
                goto child_failure;
            for (size_t j = 0; j < count; ++j) {
                (void)close(statuses[j][0]);
                if (j != i)
                    (void)close(statuses[j][1]);
            }
            (void)close(capture[0]);
            if (split_stderr) {
                if (dup2(error_pipe[1], STDERR_FILENO) < 0)
                    goto child_failure;
            } else if (i == 0 && dup2(capture[1], STDERR_FILENO) < 0) {
                goto child_failure;
            }
            if (i != 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0)
                    goto child_failure;
            }
            if (i == 0 && count == 1 && prepared[i].output_fd < 0 &&
                dup2(capture[1], STDOUT_FILENO) < 0)
                goto child_failure;
            if (i + 1 < count) {
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0)
                    goto child_failure;
            } else if (count > 1 && prepared[i].output_fd < 0 &&
                       dup2(capture[1], STDOUT_FILENO) < 0) {
                goto child_failure;
            }
            if (prepared[i].input_fd >= 0 &&
                dup2(prepared[i].input_fd, STDIN_FILENO) < 0)
                goto child_failure;
            if (prepared[i].output_fd >= 0 &&
                dup2(prepared[i].output_fd, STDOUT_FILENO) < 0)
                goto child_failure;
            if (fchdir(shell->cwd_fd) < 0)
                goto child_failure;
            (void)close(shell->cwd_fd);
            close_pipe_array(pipes, pipe_count);
            (void)close(capture[1]);
            close_fd(&error_pipe[0]);
            close_fd(&error_pipe[1]);
            for (size_t j = 0; j < count; ++j) {
                close_fd(&prepared[j].input_fd);
                close_fd(&prepared[j].output_fd);
            }
            execve(prepared[i].executable, pipeline->commands[i].argv,
                   child_environment);
child_failure:
            saved_errno = errno;
            child_write_errno(statuses[i][1], saved_errno);
            _exit(126);
        }
        if (child < 0) {
            int saved_errno = errno;
            (void)pthread_mutex_unlock(&shell->mutex);
            errno = saved_errno;
            (void)emit_error(shell, "fork", strerror(saved_errno));
            goto stop_children;
        }
        if (pgid == 0)
            pgid = child;
        (void)close(statuses[i][1]);
        statuses[i][1] = -1;
        children[started++] = child;
        shell->active_pgid = pipeline->background ? 0 : pgid;
        if (setpgid(child, pgid) < 0 && errno != EACCES && errno != ESRCH) {
            int saved_errno = errno;
            (void)pthread_mutex_unlock(&shell->mutex);
            (void)emit_error(shell, "setpgid", strerror(saved_errno));
            goto stop_children;
        }
    }
    (void)pthread_mutex_unlock(&shell->mutex);

    (void)close(capture[1]);
    capture[1] = -1;
    if (split_stderr) {
        close_fd(&error_pipe[1]);
        job->stderr_fd = error_pipe[0];
        error_pipe[0] = -1;
    }
    close_pipe_array(pipes, pipe_count);
    for (i = 0; i < count; ++i) {
        close_fd(&prepared[i].input_fd);
        close_fd(&prepared[i].output_fd);
        job->status_fds[i] = statuses[i][0];
        statuses[i][0] = -1;
    }
    job->pids = children;
    children = NULL;
    job->pgid = pgid;
    job->capture_fd = capture[0];
    capture[0] = -1;
    job->split_stderr = split_stderr;
    job->foreground = !pipeline->background;
    job->multiwatch = multiwatch;
    job->multiwatch_slot = multiwatch_slot;
    (void)pthread_mutex_lock(&shell->mutex);
    job->id = shell->next_job_id++;
    if (job->id == 0)
        job->id = shell->next_job_id++;
    job->next = shell->jobs;
    shell->jobs = job;
    job_registered = 1;
    shell->active_pgid = job->foreground ? pgid : 0;
    (void)pthread_mutex_unlock(&shell->mutex);
    for (i = 0; i < count; ++i)
        close_fd(&statuses[i][1]);
    free(statuses);
    statuses = NULL;
    free(prepared);
    prepared = NULL;
    free(pipes);
    pipes = NULL;
    if (pipeline->background && !multiwatch) {
        char message[128];
        int length = snprintf(message, sizeof(message), "[%u] %ld\n",
                              job->id, (long)job->pgid);
        if (length > 0)
            emit_output(shell, message,
                        (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1);
        result = 0;
    } else if (!pipeline->background) {
        result = wait_for_job(shell, job);
    } else {
        result = 0;
    }
    goto cleanup;

stop_children:
    if (pgid > 0) {
        (void)kill(-pgid, SIGTERM);
        (void)kill(-pgid, SIGKILL);
    }
    for (i = 0; i < started; ++i) {
        (void)kill(children[i], SIGTERM);
        (void)kill(children[i], SIGKILL);
    }
    (void)close(capture[0]);
    (void)close(capture[1]);
    close_fd(&error_pipe[0]);
    close_fd(&error_pipe[1]);
    capture[0] = capture[1] = -1;
    close_pipe_array(pipes, pipe_count);
    for (i = 0; i < started; ++i) {
        while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR)
            ;
        close_fd(&statuses[i][0]);
    }
    (void)pthread_mutex_lock(&shell->mutex);
    shell->active_pgid = 0;
    (void)pthread_mutex_unlock(&shell->mutex);
    result = 1;
    goto cleanup;

setup_failure:
    (void)emit_error(shell, "pipeline", strerror(errno == 0 ? ENOMEM : errno));
    result = 1;
cleanup:
    if (prepared != NULL) {
        for (i = 0; i < count; ++i) {
            free(prepared[i].executable);
            close_fd(&prepared[i].input_fd);
            close_fd(&prepared[i].output_fd);
        }
    }
    free(prepared);
    free(children);
    if (statuses != NULL) {
        for (i = 0; i < count; ++i) {
            close_fd(&statuses[i][0]);
            close_fd(&statuses[i][1]);
        }
    }
    free(statuses);
    close_fd(&capture[0]);
    close_fd(&capture[1]);
    close_fd(&error_pipe[0]);
    close_fd(&error_pipe[1]);
    close_pipe_array(pipes, pipe_count);
    free(pipes);
    if (!job_registered)
        job_free(job);
    return result;
}

static int
wait_for_job(Shell *shell, ShellJob *job)
{
    job->foreground = 1;
    (void)pthread_mutex_lock(&shell->mutex);
    shell->active_pgid = job->pgid;
    (void)pthread_mutex_unlock(&shell->mutex);
    for (;;) {
        struct pollfd pfds[2];
        nfds_t count = 0;
        execute_reap_jobs(shell);
        if (job->completed) {
            job->foreground = 0;
            job->completion_notified = 1;
            (void)pthread_mutex_lock(&shell->mutex);
            if (shell->active_pgid == job->pgid)
                shell->active_pgid = 0;
            (void)pthread_mutex_unlock(&shell->mutex);
            return job->last_status;
        }
        if (job_is_stopped(job)) {
            char message[1024];
            int length;
            job->foreground = 0;
            job->stopped_notified = 1;
            (void)pthread_mutex_lock(&shell->mutex);
            if (shell->active_pgid == job->pgid)
                shell->active_pgid = 0;
            (void)pthread_mutex_unlock(&shell->mutex);
            length = snprintf(message, sizeof(message), "[%u] Stopped %s\n",
                              job->id, job->command);
            if (length > 0)
                emit_output(shell, message,
                            (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1);
            return 128 + SIGTSTP;
        }
        if (job->capture_fd >= 0)
            pfds[count++] = (struct pollfd){.fd=job->capture_fd,.events=POLLIN|POLLHUP};
        if (job->stderr_fd >= 0)
            pfds[count++] = (struct pollfd){.fd=job->stderr_fd,.events=POLLIN|POLLHUP};
        (void)poll(pfds, count, 100);
    }
}

static void
job_print_list(Shell *shell)
{
    ShellJob *job;
    for (job = shell->jobs; job != NULL; job = job->next) {
        const char *state = job->completed ? "Done" :
                            job_is_stopped(job) ? "Stopped" : "Running";
        char message[1024];
        int length = snprintf(message, sizeof(message), "[%u] %-7s %s\n",
                              job->id, state, job->command);
        if (length > 0)
            emit_output(shell, message,
                        (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1);
    }
}

static unsigned
parse_job_id(const char *argument)
{
    char *end;
    unsigned long value;
    if (argument == NULL)
        return 0;
    if (*argument == '%')
        ++argument;
    errno = 0;
    value = strtoul(argument, &end, 10);
    if (errno != 0 || *argument == '\0' || *end != '\0' || value == 0 ||
        value > UINT32_MAX)
        return UINT32_MAX;
    return (unsigned)value;
}

static ShellJob *
find_job(Shell *shell, unsigned id)
{
    ShellJob *job;
    ShellJob *latest = NULL;
    for (job = shell->jobs; job != NULL; job = job->next) {
        if (id != 0 && job->id == id)
            return job;
        if (!job->completed)
            latest = job;
    }
    return id == 0 ? latest : NULL;
}

static int
execute_job_builtin(Shell *shell, Command *command, int *recognized)
{
    const char *name = command->argv[0];
    ShellJob *job;
    unsigned id;
    *recognized = 1;

    if (strcmp(name, "jobs") == 0) {
        if (command->argc != 1)
            return emit_error(shell, "jobs", "usage: jobs");
        execute_reap_jobs(shell);
        job_print_list(shell);
        return 0;
    }
    if (strcmp(name, "fg") != 0 && strcmp(name, "bg") != 0) {
        *recognized = 0;
        return 0;
    }
    if (command->argc > 2)
        return emit_error(shell, name, "usage: fg [job] / bg [job]");
    id = command->argc == 2 ? parse_job_id(command->argv[1]) : 0;
    if (id == UINT32_MAX)
        return emit_error(shell, name, "invalid job ID");
    job = find_job(shell, id);
    if (job == NULL || job->completed)
        return emit_error(shell, name, "no such running or stopped job");
    if (strcmp(name, "bg") == 0) {
        size_t i;
        if (kill(-job->pgid, SIGCONT) < 0 && errno != ESRCH)
            return emit_error(shell, "bg", strerror(errno));
        for (i = 0; i < job->count; ++i)
            if (job->states[i] == 1)
                job->states[i] = 0;
        job->foreground = 0;
        job->stopped_notified = 0;
        {
            char message[1024];
            int length = snprintf(message, sizeof(message), "[%u] %s\n",
                                  job->id, job->command);
            if (length > 0)
                emit_output(shell, message,
                            (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1);
        }
        return 0;
    }
    if (kill(-job->pgid, SIGCONT) < 0 && errno != ESRCH)
        return emit_error(shell, "fg", strerror(errno));
    for (size_t i = 0; i < job->count; ++i)
        if (job->states[i] == 1)
            job->states[i] = 0;
    {
        char message[1024];
        int length = snprintf(message, sizeof(message), "%s\n", job->command);
        if (length > 0)
            emit_output(shell, message,
                        (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1);
    }
    return wait_for_job(shell, job);
}

void
execute_cleanup_jobs(Shell *shell)
{
    ShellJob *jobs;
    ShellJob *job;
    size_t i;
    (void)pthread_mutex_lock(&shell->mutex);
    jobs = shell->jobs;
    shell->jobs = NULL;
    (void)pthread_mutex_unlock(&shell->mutex);
    for (job = jobs; job != NULL; job = job->next) {
        (void)kill(-job->pgid, SIGTERM);
        (void)kill(-job->pgid, SIGKILL);
        for (i = 0; i < job->count; ++i) {
            if (job->states[i] != 2) {
                (void)kill(job->pids[i], SIGKILL);
                while (waitpid(job->pids[i], NULL, 0) < 0 && errno == EINTR)
                    ;
            }
        }
    }
    while (jobs != NULL) {
        ShellJob *next = jobs->next;
        job_free(jobs);
        jobs = next;
    }
}

typedef struct WatchItem {
    char *command;
    struct timespec due;
    int running;
    int disabled;
} WatchItem;

static int
time_compare(struct timespec a, struct timespec b)
{
    return a.tv_sec != b.tv_sec ? (a.tv_sec < b.tv_sec ? -1 : 1) :
           a.tv_nsec == b.tv_nsec ? 0 : (a.tv_nsec < b.tv_nsec ? -1 : 1);
}

static struct timespec
time_add_seconds(struct timespec value, time_t seconds)
{
    value.tv_sec += seconds;
    return value;
}

static void
watch_failure(Shell *shell, const char *command, const char *detail)
{
    char message[2048];
    int n = snprintf(message, sizeof(message), "\"%s\", %lld:\n--------------------\n%s\n--------------------\n",
                     command, (long long)time(NULL), detail);
    if (n > 0)
        emit_output(shell, message, (size_t)n < sizeof(message) ? (size_t)n : sizeof(message) - 1);
}

static int
parse_watch_list(const char *line, char ***commands_out, size_t *count_out,
                 const char **error_out)
{
    const char *p = line;
    char **commands = NULL;
    size_t count = 0, capacity = 0;
    while (*p == ' ' || *p == '\t') ++p;
    if (strncmp(p, "multiWatch", 10) != 0 ||
        (p[10] != '\0' && p[10] != ' ' && p[10] != '\t' && p[10] != '['))
        return 0;
    p += 10;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p++ != '[') { *error_out = "expected '[' after multiWatch"; goto fail; }
    for (;;) {
        char *word;
        size_t used = 0, cap = 32;
        while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
        if (*p == ']') {
            ++p;
            while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
            if (*p != '\0') { *error_out = "unexpected text after ']'"; goto fail; }
            if (count == 0) { *error_out = "at least one command is required"; goto fail; }
            *commands_out = commands; *count_out = count;
            return 1;
        }
        if (*p++ != '"') { *error_out = "expected a double-quoted command"; goto fail; }
        word = malloc(cap);
        if (word == NULL) { *error_out = "out of memory"; goto fail; }
        while (*p != '\0' && *p != '"') {
            char ch = *p++;
            if (ch == '\\' && (*p == '"' || *p == '\\')) ch = *p++;
            if (used + 1 >= cap) {
                size_t next_cap = cap * 2;
                char *next = realloc(word, next_cap);
                if (next == NULL) { free(word); *error_out = "out of memory"; goto fail; }
                word = next; cap = next_cap;
            }
            word[used++] = ch;
        }
        if (*p != '"') { free(word); *error_out = "unclosed command quote"; goto fail; }
        ++p; word[used] = '\0';
        if (used == 0) { free(word); *error_out = "empty command is not allowed"; goto fail; }
        if (count == capacity) {
            size_t next_capacity = capacity == 0 ? 4 : capacity * 2;
            char **next = realloc(commands, next_capacity * sizeof(*commands));
            if (next == NULL) { free(word); *error_out = "out of memory"; goto fail; }
            commands = next; capacity = next_capacity;
        }
        commands[count++] = word;
        while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
        if (*p == ',') {
            ++p;
            while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
            if (*p == ']' || *p == '\0') { *error_out = "expected a command after ','"; goto fail; }
            continue;
        }
        if (*p != ']') { *error_out = "expected ',' or ']'"; goto fail; }
    }
fail:
    for (size_t i = 0; i < count; ++i) free(commands[i]);
    free(commands);
    return -1;
}

static void
watch_remove_completed(Shell *shell, WatchItem *items, size_t count)
{
    ShellJob **link;
    ShellJob *removed = NULL;
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    (void)pthread_mutex_lock(&shell->mutex);
    link = &shell->jobs;
    while (*link != NULL) {
        ShellJob *job = *link;
        if (job->multiwatch && job->completed) {
            *link = job->next;
            job->next = removed;
            removed = job;
            if (job->multiwatch_slot < count) {
                items[job->multiwatch_slot].running = 0;
                items[job->multiwatch_slot].due = time_add_seconds(now, 2);
            }
        } else {
            link = &job->next;
        }
    }
    (void)pthread_mutex_unlock(&shell->mutex);
    while (removed != NULL) {
        ShellJob *next = removed->next;
        (void)kill(-removed->pgid, SIGTERM);
        job_free(removed);
        removed = next;
    }
}

static void
watch_stop_jobs(Shell *shell)
{
    ShellJob **link;
    ShellJob *removed = NULL;
    (void)pthread_mutex_lock(&shell->mutex);
    link = &shell->jobs;
    while (*link != NULL) {
        ShellJob *job = *link;
        if (!job->multiwatch) { link = &job->next; continue; }
        (void)kill(-job->pgid, SIGTERM);
        (void)kill(-job->pgid, SIGKILL);
        for (size_t i = 0; i < job->count; ++i) {
            if (job->states[i] != 2) {
                (void)kill(job->pids[i], SIGKILL);
                while (waitpid(job->pids[i], NULL, 0) < 0 && errno == EINTR) { }
            }
        }
        *link = job->next;
        job->next = removed;
        removed = job;
    }
    shell->multiwatch_active = 0;
    shell->multiwatch_stop = 0;
    (void)pthread_mutex_unlock(&shell->mutex);
    while (removed != NULL) {
        ShellJob *next = removed->next;
        job_free(removed);
        removed = next;
    }
}

static int
execute_multiwatch(Shell *shell, char **commands, size_t count)
{
    WatchItem *items = calloc(count, sizeof(*items));
    if (items == NULL) return emit_error(shell, "multiWatch", "out of memory");
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    for (size_t i = 0; i < count; ++i) {
        items[i].command = commands[i];
        items[i].due = now;
    }
    (void)pthread_mutex_lock(&shell->mutex);
    shell->multiwatch_active = 1;
    shell->multiwatch_stop = 0;
    (void)pthread_mutex_unlock(&shell->mutex);

    for (;;) {
        struct pollfd *fds = NULL;
        nfds_t fd_count = 0;
        int timeout = 100;
        int stop;
        (void)pthread_mutex_lock(&shell->mutex);
        stop = shell->multiwatch_stop || shell->stopping;
        (void)pthread_mutex_unlock(&shell->mutex);
        if (stop) break;
        execute_reap_jobs(shell);
        watch_remove_completed(shell, items, count);
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        for (size_t i = 0; i < count; ++i) {
            if (items[i].disabled || items[i].running || time_compare(items[i].due, now) > 0) continue;
            Pipeline pipeline;
            ParseError error;
            if (parser_parse(items[i].command, &pipeline, &error) < 0 || pipeline.count == 0) {
                watch_failure(shell, items[i].command, "syntax error");
                items[i].disabled = 1;
                continue;
            }
            if (pipeline.background) {
                parser_pipeline_free(&pipeline);
                watch_failure(shell, items[i].command, "background operator is not allowed inside multiWatch");
                items[i].disabled = 1;
                continue;
            }
            pipeline.background = 1;
            int launch_status = run_pipeline(shell, &pipeline, items[i].command, 1, i);
            if (launch_status == 0) items[i].running = 1;
            else {
                watch_failure(shell, items[i].command, "command could not be started");
                items[i].due = time_add_seconds(now, 2);
            }
            parser_pipeline_free(&pipeline);
        }
        for (ShellJob *job = shell->jobs; job != NULL; job = job->next)
            if (job->multiwatch && !job->completed && job->capture_fd >= 0) ++fd_count;
        if (fd_count != 0) fds = calloc(fd_count, sizeof(*fds));
        if (fds != NULL) {
            nfds_t n = 0;
            for (ShellJob *job = shell->jobs; job != NULL; job = job->next) {
                if (job->multiwatch && !job->completed && job->capture_fd >= 0) {
                    fds[n].fd = job->capture_fd;
                    fds[n].events = POLLIN | POLLHUP;
                    ++n;
                }
            }
        }
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        for (size_t i = 0; i < count; ++i) {
            if (!items[i].disabled && !items[i].running) {
                long ms = (long)(items[i].due.tv_sec - now.tv_sec) * 1000L +
                          (items[i].due.tv_nsec - now.tv_nsec) / 1000000L;
                if (ms < timeout && ms >= 0) timeout = (int)ms;
            }
        }
        (void)poll(fds, fds == NULL ? 0 : fd_count, timeout);
        free(fds);
    }
    watch_stop_jobs(shell);
    free(items);
    return 0;
}

static int
has_script_suffix(const char *path)
{
    size_t n = strlen(path);
    return n >= 3 && strcmp(path + n - 3, ".sh") == 0;
}

static int
has_shebang(Shell *shell, const char *path)
{
    unsigned char prefix[2];
    int fd = openat(shell->cwd_fd, path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n;
    do { n = read(fd, prefix, sizeof(prefix)); } while (n < 0 && errno == EINTR);
    (void)close(fd);
    return n == 2 && prefix[0] == '#' && prefix[1] == '!';
}

int
execute_line(Shell *shell, const char *line, int *should_exit)
{
    Pipeline pipeline;
    ParseError error;
    Command *command;
    int status = 0;

    *should_exit = 0;
    {
        char **watch_commands = NULL;
        size_t watch_count = 0;
        const char *watch_error = NULL;
        int watch_parse = parse_watch_list(line, &watch_commands, &watch_count, &watch_error);
        if (watch_parse < 0) {
            char message[512];
            int n = snprintf(message, sizeof(message), "terminator: multiWatch: %s\n",
                             watch_error == NULL ? "invalid syntax" : watch_error);
            if (n > 0) emit_output(shell, message, (size_t)n < sizeof(message) ? (size_t)n : sizeof(message) - 1);
            return 2;
        }
        if (watch_parse > 0) {
            status = execute_multiwatch(shell, watch_commands, watch_count);
            for (size_t i = 0; i < watch_count; ++i) free(watch_commands[i]);
            free(watch_commands);
            return status;
        }
    }
    if (parser_parse(line, &pipeline, &error) < 0) {
        char message[256];
        int count = snprintf(message, sizeof(message),
                             "terminator: syntax error at byte %zu: %s\n",
                             error.offset, error.message);
        if (count > 0)
            emit_output(shell, message,
                        (size_t)count < sizeof(message) ? (size_t)count : sizeof(message) - 1);
        return 2;
    }
    if (pipeline.count == 0) {
        parser_pipeline_free(&pipeline);
        return 0;
    }
    command = &pipeline.commands[0];

    if (pipeline.background || pipeline.count > 1 || command->redirection_count != 0) {
        status = run_pipeline(shell, &pipeline, line, 0, 0);
        parser_pipeline_free(&pipeline);
        return status;
    }

    if (strcmp(command->argv[0], "source") == 0) {
        if (command->argc < 2) status = emit_error(shell, "source", "usage: source script [arguments ...]");
        else if (command->argc == 2)
            status = script_invoke(shell, command->argv[1], (int)shell->script_argc,
                                   shell->script_args, 1, &status);
        else status = script_invoke(shell, command->argv[1], (int)(command->argc - 2),
                                    &command->argv[2], 1, &status);
        parser_pipeline_free(&pipeline);
        return status;
    }
    if (strcmp(command->argv[0], "bash") == 0) {
        if (command->argc < 2 || command->argv[1][0] == '-')
            status = emit_error(shell, "bash", "usage: bash script [arguments ...]");
        else status = script_invoke(shell, command->argv[1], (int)(command->argc - 2),
                                    &command->argv[2], 0, &status);
        parser_pipeline_free(&pipeline);
        return status;
    }
    if (strchr(command->argv[0], '/') != NULL &&
        (has_script_suffix(command->argv[0]) || has_shebang(shell, command->argv[0]))) {
        status = script_invoke(shell, command->argv[0], (int)(command->argc - 1),
                               &command->argv[1], 0, &status);
        parser_pipeline_free(&pipeline);
        return status;
    }

    if (strcmp(command->argv[0], "jobs") == 0 ||
        strcmp(command->argv[0], "fg") == 0 ||
        strcmp(command->argv[0], "bg") == 0) {
        int recognized;
        status = execute_job_builtin(shell, command, &recognized);
        parser_pipeline_free(&pipeline);
        return status;
    }

    if (strcmp(command->argv[0], "pwd") == 0) {
        emit_output(shell, shell->cwd_path, strlen(shell->cwd_path));
        emit_output(shell, "\n", 1);
    } else if (strcmp(command->argv[0], "cd") == 0) {
        const char *target;
        if (command->argc > 2) {
            status = emit_error(shell, "cd", "usage: cd [directory]");
        } else {
            target = command->argc == 1 ? script_variable_get(shell, "HOME") : command->argv[1];
            if (target == NULL || target[0] == '\0')
                status = emit_error(shell, "cd", "HOME is not set");
            else
                status = set_cwd(shell, target);
        }
    } else if (strcmp(command->argv[0], "exit") == 0) {
        *should_exit = 1;
    } else {
        status = run_pipeline(shell, &pipeline, line, 0, 0);
    }

    parser_pipeline_free(&pipeline);
    return status;
}
