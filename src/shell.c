#define _POSIX_C_SOURCE 200809L

#include "shell_internal.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { SHELL_OUTPUT_QUEUE_LIMIT = 1024 * 1024 };

static volatile sig_atomic_t child_signal_pending;

static void
child_signal_handler(int signal_number)
{
    (void)signal_number;
    child_signal_pending = 1;
}

static int
install_child_signal_handler(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = child_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    return sigaction(SIGCHLD, &action, NULL);
}

static void
free_commands(ShellCommand *command)
{
    while (command != NULL) {
        ShellCommand *next = command->next;
        free(command->line);
        free(command);
        command = next;
    }
}

static void
free_events(ShellEventNode *event)
{
    while (event != NULL) {
        ShellEventNode *next = event->next;
        free(event->event.text);
        free(event);
        event = next;
    }
}

int
shell_emit(Shell *shell, ShellEventType type,
           const char *text, size_t length, int status)
{
    ShellEventNode *node = calloc(1, sizeof(*node));
    unsigned char notification = 1;

    if (node == NULL)
        return -1;
    if (length != 0) {
        node->event.text = malloc(length + 1);
        if (node->event.text == NULL) {
            free(node);
            return -1;
        }
        memcpy(node->event.text, text, length);
        node->event.text[length] = '\0';
    }
    node->event.type = type;
    node->event.length = length;
    node->event.status = status;

    if (pthread_mutex_lock(&shell->mutex) != 0) {
        free(node->event.text);
        free(node);
        return -1;
    }
    while (type == SHELL_EVENT_OUTPUT && length != 0 &&
           shell->queued_output_bytes >= SHELL_OUTPUT_QUEUE_LIMIT &&
           !shell->stopping) {
        (void)pthread_cond_wait(&shell->events_space, &shell->mutex);
    }
    if (shell->event_tail == NULL)
        shell->event_head = node;
    else
        shell->event_tail->next = node;
    shell->event_tail = node;
    if (type == SHELL_EVENT_OUTPUT)
        shell->queued_output_bytes += length;
    (void)pthread_mutex_unlock(&shell->mutex);

    /* A full nonblocking wake pipe already guarantees that the GUI will wake. */
    for (;;) {
        ssize_t written = write(shell->wake_write_fd, &notification, 1);
        if (written == 1 || (written < 0 && errno == EAGAIN))
            break;
        if (written < 0 && errno == EINTR)
            continue;
        break;
    }
    return 0;
}

static void *
shell_worker(void *argument)
{
    Shell *shell = argument;

    for (;;) {
        ShellCommand *command;
        int should_exit = 0;
        int status;

        (void)pthread_mutex_lock(&shell->mutex);
        while (shell->command_head == NULL && !shell->stopping) {
            struct timespec deadline;
            (void)clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 100000000;
            if (deadline.tv_nsec >= 1000000000) {
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000;
            }
            int wait_result = pthread_cond_timedwait(&shell->commands_ready,
                                                      &shell->mutex, &deadline);
            if (wait_result == ETIMEDOUT || child_signal_pending) {
                child_signal_pending = 0;
                (void)pthread_mutex_unlock(&shell->mutex);
                execute_reap_jobs(shell);
                (void)pthread_mutex_lock(&shell->mutex);
            }
        }
        if (shell->stopping) {
            (void)pthread_mutex_unlock(&shell->mutex);
            break;
        }
        command = shell->command_head;
        shell->command_head = command->next;
        if (shell->command_head == NULL)
            shell->command_tail = NULL;
        (void)pthread_mutex_unlock(&shell->mutex);

        status = execute_line(shell, command->line, &should_exit);
        free(command->line);
        free(command);
        (void)shell_emit(shell, SHELL_EVENT_COMMAND_DONE, NULL, 0, status);

        if (should_exit) {
            (void)shell_emit(shell, SHELL_EVENT_EXIT, NULL, 0, status);
            break;
        }
    }

    execute_cleanup_jobs(shell);

    (void)pthread_mutex_lock(&shell->mutex);
    shell->worker_exited = 1;
    shell->active_pgid = 0;
    (void)pthread_mutex_unlock(&shell->mutex);
    return NULL;
}

Shell *
shell_create(int wake_write_fd)
{
    Shell *shell = calloc(1, sizeof(*shell));
    if (shell == NULL)
        return NULL;
    if (install_child_signal_handler() < 0) {
        free(shell);
        return NULL;
    }
    shell->cwd_fd = -1;
    shell->wake_write_fd = wake_write_fd;
    shell->cwd_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (shell->cwd_fd >= 0 && shell->cwd_fd <= STDERR_FILENO) {
        int moved = fcntl(shell->cwd_fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (moved < 0) {
            (void)close(shell->cwd_fd);
            shell->cwd_fd = -1;
        } else {
            (void)close(shell->cwd_fd);
            shell->cwd_fd = moved;
        }
    }
    shell->cwd_path = getcwd(NULL, 0);
    if (shell->cwd_fd < 0 || shell->cwd_path == NULL)
        goto fail;
    if (pthread_mutex_init(&shell->mutex, NULL) != 0)
        goto fail;
    if (pthread_cond_init(&shell->commands_ready, NULL) != 0) {
        (void)pthread_mutex_destroy(&shell->mutex);
        goto fail;
    }
    if (pthread_cond_init(&shell->events_space, NULL) != 0) {
        (void)pthread_cond_destroy(&shell->commands_ready);
        (void)pthread_mutex_destroy(&shell->mutex);
        goto fail;
    }
    if (pthread_create(&shell->thread, NULL, shell_worker, shell) != 0) {
        (void)pthread_cond_destroy(&shell->events_space);
        (void)pthread_cond_destroy(&shell->commands_ready);
        (void)pthread_mutex_destroy(&shell->mutex);
        goto fail;
    }
    shell->initialized = 1;
    return shell;

fail:
    if (shell->cwd_fd >= 0)
        (void)close(shell->cwd_fd);
    free(shell->cwd_path);
    while (shell->variables != NULL) {
        ShellVariable *next = shell->variables->next;
        free(shell->variables->name);
        free(shell->variables->value);
        free(shell->variables);
        shell->variables = next;
    }
    free(shell->capture_buffer);
    free(shell);
    return NULL;
}

void
shell_set_history(Shell *shell, History *history)
{
    if (shell == NULL)
        return;
    (void)pthread_mutex_lock(&shell->mutex);
    shell->history = history;
    (void)pthread_mutex_unlock(&shell->mutex);
}

int
shell_submit(Shell *shell, const char *line)
{
    ShellCommand *command = calloc(1, sizeof(*command));
    if (command == NULL)
        return -1;
    command->line = strdup(line);
    if (command->line == NULL) {
        free(command);
        return -1;
    }

    (void)pthread_mutex_lock(&shell->mutex);
    if (shell->stopping || shell->worker_exited) {
        (void)pthread_mutex_unlock(&shell->mutex);
        free(command->line);
        free(command);
        return -1;
    }
    if (shell->command_tail == NULL)
        shell->command_head = command;
    else
        shell->command_tail->next = command;
    shell->command_tail = command;
    (void)pthread_cond_signal(&shell->commands_ready);
    (void)pthread_mutex_unlock(&shell->mutex);
    return 0;
}

int
shell_pop_event(Shell *shell, ShellEvent *event)
{
    ShellEventNode *node;

    (void)pthread_mutex_lock(&shell->mutex);
    node = shell->event_head;
    if (node != NULL) {
        shell->event_head = node->next;
        if (shell->event_head == NULL)
            shell->event_tail = NULL;
        if (node->event.type == SHELL_EVENT_OUTPUT) {
            if (shell->queued_output_bytes >= node->event.length)
                shell->queued_output_bytes -= node->event.length;
            else
                shell->queued_output_bytes = 0;
            (void)pthread_cond_signal(&shell->events_space);
        }
    }
    (void)pthread_mutex_unlock(&shell->mutex);
    if (node == NULL)
        return 0;
    *event = node->event;
    free(node);
    return 1;
}

int
shell_signal_foreground(Shell *shell, int suspend)
{
    pid_t pgid;
    int script_active;
    int signal_number = suspend ? SIGTSTP : SIGINT;
    ShellJob *job;
    if (shell == NULL) {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&shell->mutex);
    if (shell->script_active && !suspend)
        shell->script_cancelled = 1;
    if (shell->multiwatch_active) {
        if (!suspend) {
            shell->multiwatch_stop = 1;
            for (job = shell->jobs; job != NULL; job = job->next) {
                if (job->multiwatch) {
                    (void)kill(-job->pgid, SIGTERM);
                }
            }
        }
        (void)pthread_mutex_unlock(&shell->mutex);
        return 0;
    }
    pgid = shell->active_pgid;
    script_active = shell->script_active;
    (void)pthread_mutex_unlock(&shell->mutex);
    if (pgid <= 0) {
        if (script_active) return 0;
        errno = ESRCH;
        return -1;
    }
    return kill(-pgid, signal_number);
}

void
shell_completion_free(char **matches, size_t match_count)
{
    size_t i;
    for (i = 0; i < match_count; ++i)
        free(matches[i]);
    free(matches);
}

int
shell_complete_path(Shell *shell, const char *prefix,
                    char ***matches, size_t *match_count)
{
    const char *slash;
    size_t head_length;
    const char *base;
    char *directory;
    int directory_fd;
    DIR *stream;
    char **items = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *entry;

    if (shell == NULL || prefix == NULL || matches == NULL || match_count == NULL) {
        errno = EINVAL;
        return -1;
    }
    *matches = NULL;
    *match_count = 0;
    slash = strrchr(prefix, '/');
    head_length = slash == NULL ? 0 : (size_t)(slash - prefix) + 1;
    base = slash == NULL ? prefix : slash + 1;
    if (slash == NULL)
        directory = strdup(".");
    else if (slash == prefix)
        directory = strdup("/");
    else
        directory = strndup(prefix, (size_t)(slash - prefix));
    if (directory == NULL)
        return -1;

    (void)pthread_mutex_lock(&shell->mutex);
    directory_fd = openat(shell->cwd_fd, directory,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        int saved = errno;
        (void)pthread_mutex_unlock(&shell->mutex);
        free(directory);
        errno = saved;
        return -1;
    }
    stream = fdopendir(directory_fd);
    if (stream == NULL) {
        int saved = errno;
        (void)close(directory_fd);
        (void)pthread_mutex_unlock(&shell->mutex);
        free(directory);
        errno = saved;
        return -1;
    }
    while ((entry = readdir(stream)) != NULL) {
        struct stat st;
        int is_directory;
        size_t name_length;
        char *candidate;
        char **grown;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        name_length = strlen(entry->d_name);
        if (strncmp(entry->d_name, base, strlen(base)) != 0)
            continue;
        is_directory = fstatat(dirfd(stream), entry->d_name, &st, 0) == 0 &&
                       S_ISDIR(st.st_mode);
        if (head_length > SIZE_MAX - name_length - (size_t)is_directory - 1)
            continue;
        candidate = malloc(head_length + name_length + (size_t)is_directory + 1);
        if (candidate == NULL)
            goto fail_locked;
        memcpy(candidate, prefix, head_length);
        memcpy(candidate + head_length, entry->d_name, name_length);
        if (is_directory)
            candidate[head_length + name_length] = '/';
        candidate[head_length + name_length + (size_t)is_directory] = '\0';
        if (count == capacity) {
            size_t next = capacity == 0 ? 8 : capacity * 2;
            grown = realloc(items, next * sizeof(*grown));
            if (grown == NULL) {
                free(candidate);
                goto fail_locked;
            }
            items = grown;
            capacity = next;
        }
        items[count++] = candidate;
    }
    if (closedir(stream) < 0) {
        stream = NULL;
        goto fail_locked;
    }
    stream = NULL;
    (void)pthread_mutex_unlock(&shell->mutex);
    free(directory);
    *matches = items;
    *match_count = count;
    return 0;

fail_locked:
    {
        int saved = errno == 0 ? ENOMEM : errno;
        if (stream != NULL)
            (void)closedir(stream);
        (void)pthread_mutex_unlock(&shell->mutex);
        free(directory);
        shell_completion_free(items, count);
        errno = saved;
        return -1;
    }
}

void
shell_event_dispose(ShellEvent *event)
{
    free(event->text);
    memset(event, 0, sizeof(*event));
}

void
shell_request_stop(Shell *shell)
{
    pid_t pgid;

    if (shell == NULL || !shell->initialized)
        return;
    (void)pthread_mutex_lock(&shell->mutex);
    shell->stopping = 1;
    shell->script_cancelled = 1;
    pgid = shell->active_pgid;
    (void)pthread_cond_broadcast(&shell->commands_ready);
    (void)pthread_cond_broadcast(&shell->events_space);
    if (pgid > 0) {
        (void)kill(-pgid, SIGTERM);
        (void)kill(pgid, SIGTERM);
        (void)kill(-pgid, SIGKILL);
        (void)kill(pgid, SIGKILL);
    }
    ShellJob *job;
    for (job = shell->jobs; job != NULL; job = job->next) {
        (void)kill(-job->pgid, SIGTERM);
        (void)kill(-job->pgid, SIGKILL);
    }
    (void)pthread_mutex_unlock(&shell->mutex);
}

void
shell_destroy(Shell *shell)
{
    if (shell == NULL)
        return;
    shell_request_stop(shell);
    if (shell->initialized)
        (void)pthread_join(shell->thread, NULL);
    free_commands(shell->command_head);
    free_events(shell->event_head);
    if (shell->cwd_fd >= 0)
        (void)close(shell->cwd_fd);
    free(shell->cwd_path);
    if (shell->initialized) {
        (void)pthread_cond_destroy(&shell->events_space);
        (void)pthread_cond_destroy(&shell->commands_ready);
        (void)pthread_mutex_destroy(&shell->mutex);
    }
    free(shell);
}
