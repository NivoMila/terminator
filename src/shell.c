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

static void shell_state_cancel(ShellState *state);

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
        shell_completion_free(event->event.matches, event->event.match_count);
        free(event);
        event = next;
    }
}

int
shell_emit(ShellState *state, ShellEventType type,
           const char *text, size_t length, int status)
{
    if (state == NULL || state->emit == NULL) {
        errno = EPIPE;
        return -1;
    }
    return state->emit(state->emit_context, type, text, length, status);
}

static int
thread_emit(void *context, ShellEventType type,
           const char *text, size_t length, int status)
{
    Shell *shell = context;
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
    if (shell->process_mode && shell->stopping && type == SHELL_EVENT_OUTPUT) {
        (void)pthread_mutex_unlock(&shell->mutex);
        free(node->event.text);
        free(node);
        return 0;
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

static int
process_emit(void *context, ShellEventType type, const char *text,
             size_t length, int status, uint64_t request_id,
             char **matches, size_t match_count)
{
    Shell *shell = context;
    ShellEventNode *node;
    unsigned char notification = 1;
    size_t i;
    if (type != SHELL_EVENT_COMPLETION)
        return thread_emit(context, type, text, length, status);
    node = calloc(1, sizeof(*node));
    if (node == NULL)
        return -1;
    node->event.type = type;
    node->event.status = status;
    node->event.request_id = request_id;
    if (match_count != 0) {
        node->event.matches = calloc(match_count, sizeof(*node->event.matches));
        if (node->event.matches == NULL) {
            free(node);
            return -1;
        }
        for (i = 0; i < match_count; ++i) {
            node->event.matches[i] = strdup(matches[i]);
            if (node->event.matches[i] == NULL) {
                node->event.match_count = i;
                shell_completion_free(node->event.matches, i);
                free(node);
                return -1;
            }
            ++node->event.match_count;
        }
    }
    if (pthread_mutex_lock(&shell->mutex) != 0) {
        shell_completion_free(node->event.matches, node->event.match_count);
        free(node);
        return -1;
    }
    if (shell->event_tail == NULL)
        shell->event_head = node;
    else
        shell->event_tail->next = node;
    shell->event_tail = node;
    (void)pthread_mutex_unlock(&shell->mutex);
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
    ShellState *state = &shell->state;

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
                shell_state_reap_jobs(state);
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

        status = shell_state_execute(state, command->line, &should_exit);
        free(command->line);
        free(command);
        (void)shell_emit(state, SHELL_EVENT_COMMAND_DONE, NULL, 0, status);

        if (should_exit) {
            (void)shell_emit(state, SHELL_EVENT_EXIT, NULL, 0, status);
            break;
        }
    }

    shell_state_shutdown(state);

    (void)pthread_mutex_lock(&shell->mutex);
    shell->worker_exited = 1;
    (void)pthread_mutex_unlock(&shell->mutex);
    return NULL;
}

int
shell_state_init(ShellState *state, History *history,
                 ShellStateEmit emit, void *emit_context)
{
    if (state == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(state, 0, sizeof(*state));
    state->cwd_fd = -1;
    state->control_fd = -1;
    state->cwd_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (state->cwd_fd >= 0 && state->cwd_fd <= STDERR_FILENO) {
        int moved = fcntl(state->cwd_fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (moved < 0) {
            (void)close(state->cwd_fd);
            state->cwd_fd = -1;
        } else {
            (void)close(state->cwd_fd);
            state->cwd_fd = moved;
        }
    }
    state->cwd_path = getcwd(NULL, 0);
    if (state->cwd_fd < 0 || state->cwd_path == NULL)
        goto fail;
    if (pthread_mutex_init(&state->mutex, NULL) != 0)
        goto fail;
    state->mutex_initialized = 1;
    state->history = history;
    state->emit = emit;
    state->emit_context = emit_context;
    return 0;

fail:
    if (state->cwd_fd >= 0)
        (void)close(state->cwd_fd);
    free(state->cwd_path);
    state->cwd_fd = -1;
    state->cwd_path = NULL;
    return -1;
}

int
shell_state_execute(ShellState *state, const char *line, int *should_exit)
{
    if (state == NULL || line == NULL || should_exit == NULL) {
        errno = EINVAL;
        return -1;
    }
    return execute_interactive_line(state, line, should_exit);
}

void
shell_state_reap_jobs(ShellState *state)
{
    if (state != NULL)
        execute_reap_jobs(state);
}

void
shell_state_request_shutdown(ShellState *state)
{
    if (state != NULL)
        shell_state_cancel(state);
}

int
shell_state_service_control(ShellState *state)
{
    unsigned char signals[32];
    int shutdown_requested = 0;
    if (state == NULL || state->control_fd < 0)
        return 0;
    for (;;) {
        ssize_t amount = read(state->control_fd, signals, sizeof(signals));
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (amount <= 0)
            break;
        for (ssize_t i = 0; i < amount; ++i) {
            if (signals[i] == 1)
                (void)shell_state_signal_foreground(state, 0);
            else if (signals[i] == 2)
                (void)shell_state_signal_foreground(state, 1);
            else if (signals[i] == 3) {
                shell_state_request_shutdown(state);
                shutdown_requested = 1;
            }
        }
    }
    return shutdown_requested;
}

static void
shell_state_cancel(ShellState *state)
{
    ShellJob *job;
    pid_t pgid;
    if (state == NULL || !state->mutex_initialized)
        return;
    (void)pthread_mutex_lock(&state->mutex);
    state->stopping = 1;
    state->script_cancelled = 1;
    pgid = state->active_pgid;
    if (pgid > 0) {
        (void)kill(-pgid, SIGTERM);
        (void)kill(pgid, SIGTERM);
        (void)kill(-pgid, SIGKILL);
        (void)kill(pgid, SIGKILL);
    }
    for (job = state->jobs; job != NULL; job = job->next) {
        (void)kill(-job->pgid, SIGTERM);
        (void)kill(-job->pgid, SIGKILL);
    }
    (void)pthread_mutex_unlock(&state->mutex);
}

void
shell_state_shutdown(ShellState *state)
{
    if (state == NULL || !state->mutex_initialized || state->shutdown_complete)
        return;
    shell_state_cancel(state);
    execute_cleanup_jobs(state);
    (void)pthread_mutex_lock(&state->mutex);
    state->active_pgid = 0;
    state->shutdown_complete = 1;
    (void)pthread_mutex_unlock(&state->mutex);
}

void
shell_state_destroy(ShellState *state)
{
    ShellVariable *variable;
    if (state == NULL)
        return;
    shell_state_shutdown(state);
    variable = state->variables;
    while (variable != NULL) {
        ShellVariable *next = variable->next;
        free(variable->name);
        free(variable->value);
        free(variable);
        variable = next;
    }
    if (state->cwd_fd >= 0)
        (void)close(state->cwd_fd);
    free(state->cwd_path);
    free(state->capture_buffer);
    if (state->mutex_initialized)
        (void)pthread_mutex_destroy(&state->mutex);
    memset(state, 0, sizeof(*state));
    state->cwd_fd = -1;
}

Shell *
shell_create(int wake_write_fd)
{
    Shell *shell = calloc(1, sizeof(*shell));
    if (shell == NULL)
        return NULL;
    shell->wake_write_fd = wake_write_fd;
    shell->process_mode = 0;
    if (install_child_signal_handler() < 0)
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
    shell->synchronization_initialized = 1;
    if (shell_state_init(&shell->state, NULL, thread_emit, shell) < 0)
        goto fail_sync;
    if (pthread_create(&shell->thread, NULL, shell_worker, shell) != 0) {
        shell_state_destroy(&shell->state);
        goto fail_sync;
    }
    shell->initialized = 1;
    return shell;

fail_sync:
    (void)pthread_cond_destroy(&shell->events_space);
    (void)pthread_cond_destroy(&shell->commands_ready);
    (void)pthread_mutex_destroy(&shell->mutex);
    shell->synchronization_initialized = 0;
fail:
    free(shell);
    return NULL;
}

Shell *
shell_create_process(int wake_write_fd, int x11_fd, uint32_t tab_id)
{
    Shell *shell = calloc(1, sizeof(*shell));
    if (shell == NULL)
        return NULL;
    shell->wake_write_fd = wake_write_fd;
    shell->process_mode = 1;
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
    shell->synchronization_initialized = 1;
    shell->process = process_runtime_create(tab_id, x11_fd, process_emit, shell);
    if (shell->process == NULL)
        goto fail_sync;
    shell->initialized = 1;
    return shell;

fail_sync:
    (void)pthread_cond_destroy(&shell->events_space);
    (void)pthread_cond_destroy(&shell->commands_ready);
    (void)pthread_mutex_destroy(&shell->mutex);
    shell->synchronization_initialized = 0;
fail:
    free(shell);
    return NULL;
}

void
shell_set_history(Shell *shell, History *history)
{
    if (shell == NULL || shell->process_mode)
        return;
    (void)pthread_mutex_lock(&shell->state.mutex);
    shell->state.history = history;
    (void)pthread_mutex_unlock(&shell->state.mutex);
}

int
shell_submit(Shell *shell, const char *line)
{
    if (shell != NULL && shell->process_mode)
        return process_runtime_submit(shell->process, line);
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
shell_state_signal_foreground(ShellState *state, int suspend)
{
    pid_t pgid;
    int script_active;
    int signal_number = suspend ? SIGTSTP : SIGINT;
    ShellJob *job;
    if (state == NULL) {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&state->mutex);
    if (state->script_active && !suspend)
        state->script_cancelled = 1;
    if (state->multiwatch_active) {
        if (!suspend) {
            state->multiwatch_stop = 1;
            for (job = state->jobs; job != NULL; job = job->next) {
                if (job->multiwatch) {
                    (void)kill(-job->pgid, SIGTERM);
                }
            }
        }
        (void)pthread_mutex_unlock(&state->mutex);
        return 0;
    }
    pgid = state->active_pgid;
    script_active = state->script_active;
    (void)pthread_mutex_unlock(&state->mutex);
    if (pgid <= 0) {
        if (script_active) return 0;
        errno = ESRCH;
        return -1;
    }
    return kill(-pgid, signal_number);
}

int
shell_signal_foreground(Shell *shell, int suspend)
{
    if (shell == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (shell->process_mode) {
        return process_runtime_signal(shell->process, suspend);
    }
    return shell_state_signal_foreground(&shell->state, suspend);
}

void
shell_completion_free(char **matches, size_t match_count)
{
    size_t i;
    if (matches == NULL)
        return;
    for (i = 0; i < match_count; ++i)
        free(matches[i]);
    free(matches);
}

int
shell_state_complete_path(ShellState *state, const char *prefix,
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

    if (state == NULL || prefix == NULL || matches == NULL || match_count == NULL) {
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

    (void)pthread_mutex_lock(&state->mutex);
    directory_fd = openat(state->cwd_fd, directory,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        int saved = errno;
        (void)pthread_mutex_unlock(&state->mutex);
        free(directory);
        errno = saved;
        return -1;
    }
    stream = fdopendir(directory_fd);
    if (stream == NULL) {
        int saved = errno;
        (void)close(directory_fd);
        (void)pthread_mutex_unlock(&state->mutex);
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
    (void)pthread_mutex_unlock(&state->mutex);
    free(directory);
    *matches = items;
    *match_count = count;
    return 0;

fail_locked:
    {
        int saved = errno == 0 ? ENOMEM : errno;
        if (stream != NULL)
            (void)closedir(stream);
        (void)pthread_mutex_unlock(&state->mutex);
        free(directory);
        shell_completion_free(items, count);
        errno = saved;
        return -1;
    }
}

int
shell_complete_path(Shell *shell, const char *prefix,
                    char ***matches, size_t *match_count)
{
    if (shell == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (shell->process_mode) {
        errno = ENOTSUP;
        return -1;
    }
    return shell_state_complete_path(&shell->state, prefix, matches, match_count);
}

int
shell_request_completion(Shell *shell, const char *prefix,
                         uint64_t *request_id)
{
    if (shell == NULL || prefix == NULL || request_id == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!shell->process_mode) {
        errno = ENOTSUP;
        return -1;
    }
    return process_runtime_complete(shell->process, prefix, request_id);
}

void
shell_event_dispose(ShellEvent *event)
{
    free(event->text);
    shell_completion_free(event->matches, event->match_count);
    memset(event, 0, sizeof(*event));
}

void
shell_request_stop(Shell *shell)
{
    if (shell == NULL || !shell->initialized)
        return;
    if (shell->process_mode) {
        (void)pthread_mutex_lock(&shell->mutex);
        shell->stopping = 1;
        (void)pthread_cond_broadcast(&shell->events_space);
        (void)pthread_mutex_unlock(&shell->mutex);
        process_runtime_shutdown(shell->process);
        return;
    }
    (void)pthread_mutex_lock(&shell->mutex);
    shell->stopping = 1;
    (void)pthread_cond_broadcast(&shell->commands_ready);
    (void)pthread_cond_broadcast(&shell->events_space);
    (void)pthread_mutex_unlock(&shell->mutex);
    shell_state_cancel(&shell->state);
}

void
shell_destroy(Shell *shell)
{
    if (shell == NULL)
        return;
    shell_request_stop(shell);
    if (shell->process_mode) {
        process_runtime_destroy(shell->process);
        shell->process = NULL;
    } else if (shell->initialized) {
        (void)pthread_join(shell->thread, NULL);
    }
    free_commands(shell->command_head);
    free_events(shell->event_head);
    if (!shell->process_mode)
        shell_state_destroy(&shell->state);
    if (shell->synchronization_initialized) {
        (void)pthread_cond_destroy(&shell->events_space);
        (void)pthread_cond_destroy(&shell->commands_ready);
        (void)pthread_mutex_destroy(&shell->mutex);
    }
    free(shell);
}
