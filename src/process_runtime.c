#define _POSIX_C_SOURCE 200809L

#include "process_runtime.h"

#include "ipc.h"
#include "history.h"
#include "shell_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define CHILD_OPTION "--terminator-internal-process-child"

typedef struct Request {
    IpcMessageType type;
    char *command;
    uint64_t id;
    struct Request *next;
} Request;

struct ProcessRuntime {
    uint32_t tab_id;
    pid_t pid;
    int command_fd;
    int response_fd;
    pthread_t monitor;
    pthread_mutex_t mutex;
    int mutex_ready;
    int monitor_started;
    int stopping;
    int exited;
    int exit_sent;
    int ready;
    int pending_interrupt;
    int pending_suspend;
    int pending_shutdown;
    uint64_t next_id;
    Request *head;
    Request *tail;
    ProcessRuntimeEvent event;
    void *event_context;
};

typedef struct {
    int fd;
    uint32_t tab_id;
    uint64_t request_id;
} ChildOutput;

static volatile sig_atomic_t child_control_fd = -1;
static volatile sig_atomic_t child_shutdown_requested;

static void child_control_handler(int signal_number)
{
    unsigned char value = signal_number == SIGUSR1 ? 1 :
                          signal_number == SIGUSR2 ? 2 : 3;
    int saved = errno;
    if (signal_number == SIGTERM)
        child_shutdown_requested = 1;
    if (child_control_fd >= 0) {
        ssize_t ignored = write((int)child_control_fd, &value, 1);
        (void)ignored;
    }
    errno = saved;
}

static int child_control_install(int *read_fd, int *write_fd)
{
    struct sigaction action;
    int control_pipe[2];
    int flags;
    if (pipe(control_pipe) < 0)
        return -1;
    *read_fd = control_pipe[0];
    *write_fd = control_pipe[1];
    flags = fcntl(*read_fd, F_GETFL);
    if (flags < 0 || fcntl(*read_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    flags = fcntl(*write_fd, F_GETFL);
    if (flags < 0 || fcntl(*write_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    child_control_fd = *write_fd;
    memset(&action, 0, sizeof(action));
    action.sa_handler = child_control_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &action, NULL) < 0 ||
        sigaction(SIGUSR2, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0)
        return -1;
    return 0;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void runtime_notify(ProcessRuntime *runtime, ShellEventType type,
                           const char *text, size_t length, int status,
                           uint64_t request_id, char **matches,
                           size_t match_count)
{
    (void)runtime->event(runtime->event_context, type, text, length, status,
                         request_id, matches, match_count);
}

static int set_fd_flag(int fd, int command, int bit)
{
    int value;
    do { value = fcntl(fd, command == F_SETFD ? F_GETFD : F_GETFL); }
    while (value < 0 && errno == EINTR);
    if (value < 0) return -1;
    do { value = fcntl(fd, command, value | bit); }
    while (value < 0 && errno == EINTR);
    return value;
}

static int write_message(int fd, const IpcMessage *message)
{
    IpcWriter writer;
    IpcIoResult result;
    ipc_writer_init(&writer);
    if (ipc_writer_queue(&writer, message) < 0)
        return -1;
    do { result = ipc_writer_flush(&writer, fd); }
    while (result == IPC_IO_NEED_MORE);
    ipc_writer_reset(&writer);
    return result == IPC_IO_WRITTEN ? 0 : -1;
}

static int child_emit(void *context, ShellEventType type, const char *text,
                      size_t length, int status)
{
    ChildOutput *output = context;
    IpcMessage message = {0};
    (void)status;
    if (type != SHELL_EVENT_OUTPUT && type != SHELL_EVENT_JOB_STATE)
        return 0;
    message.type = type == SHELL_EVENT_JOB_STATE ?
                   IPC_MESSAGE_JOB_STATE_UPDATE : IPC_MESSAGE_OUTPUT_CHUNK;
    message.tab_id = output->tab_id;
    message.request_id = type == SHELL_EVENT_JOB_STATE ? 0 : output->request_id;
    message.payload = (uint8_t *)text;
    message.payload_length = (uint32_t)length;
    return write_message(output->fd, &message);
}

static int child_send_status(int fd, uint32_t tab_id, uint64_t request_id,
                             IpcMessageType type, int status)
{
    uint8_t payload[4];
    IpcMessage message = {0};
    put_u32(payload, (uint32_t)(int32_t)status);
    message.type = type;
    message.tab_id = tab_id;
    message.request_id = request_id;
    message.payload = payload;
    message.payload_length = sizeof(payload);
    return write_message(fd, &message);
}

static int child_send_ready(int fd, uint32_t tab_id)
{
    IpcMessage message = {0};
    message.type = IPC_MESSAGE_READY;
    message.tab_id = tab_id;
    return write_message(fd, &message);
}

static void child_send_error(int fd, uint32_t tab_id, uint64_t request_id,
                             int code, const char *detail)
{
    size_t detail_length = strlen(detail);
    uint8_t *payload;
    IpcMessage message = {0};
    if (detail_length > IPC_MAX_PAYLOAD_SIZE - 4)
        detail_length = IPC_MAX_PAYLOAD_SIZE - 4;
    payload = malloc(detail_length + 4);
    if (payload == NULL)
        return;
    put_u32(payload, (uint32_t)code);
    memcpy(payload + 4, detail, detail_length);
    message.type = IPC_MESSAGE_ERROR;
    message.tab_id = tab_id;
    message.request_id = request_id;
    message.payload = payload;
    message.payload_length = (uint32_t)(detail_length + 4);
    (void)write_message(fd, &message);
    free(payload);
}

static int child_send_completion(int fd, uint32_t tab_id, uint64_t request_id,
                                 char **matches, size_t count)
{
    size_t length = 4, offset = 4, i;
    uint8_t *payload;
    IpcMessage response = {0};
    int result;
    if (count > UINT32_MAX) { errno = EOVERFLOW; return -1; }
    for (i = 0; i < count; ++i) {
        size_t item_length = strlen(matches[i]);
        if (item_length > UINT32_MAX || length > IPC_MAX_PAYLOAD_SIZE - 4 ||
            item_length > IPC_MAX_PAYLOAD_SIZE - length - 4) {
            errno = EMSGSIZE;
            return -1;
        }
        length += 4 + item_length;
    }
    payload = malloc(length);
    if (payload == NULL) return -1;
    put_u32(payload, (uint32_t)count);
    for (i = 0; i < count; ++i) {
        size_t item_length = strlen(matches[i]);
        put_u32(payload + offset, (uint32_t)item_length);
        offset += 4;
        memcpy(payload + offset, matches[i], item_length);
        offset += item_length;
    }
    response.type = IPC_MESSAGE_COMPLETION_RESPONSE;
    response.tab_id = tab_id;
    response.request_id = request_id;
    response.payload = payload;
    response.payload_length = (uint32_t)length;
    result = write_message(fd, &response);
    free(payload);
    return result;
}

static int child_process_message(ShellState *state, ChildOutput *output,
                                 IpcMessage *message,
                                 int *done, int *shutdown_request)
{
    int should_exit = 0, status;
    char *command;
    if (message->tab_id != output->tab_id) {
        child_send_error(output->fd, output->tab_id, message->request_id,
                         EPROTO, "tab ID mismatch");
        *done = 1;
        return -1;
    }
    switch (message->type) {
    case IPC_MESSAGE_SHUTDOWN:
        *shutdown_request = 1;
        *done = 1;
        return 0;
    case IPC_MESSAGE_COMPLETION_REQUEST:
    {
        char *prefix;
        char **matches = NULL;
        size_t match_count = 0;
        int result;
        if (message->payload_length != 0 &&
            memchr(message->payload, '\0', message->payload_length) != NULL) {
            child_send_error(output->fd, output->tab_id, message->request_id,
                             EINVAL, "completion prefix contains a NUL byte");
            return 0;
        }
        prefix = malloc((size_t)message->payload_length + 1);
        if (prefix == NULL) {
            child_send_error(output->fd, output->tab_id, message->request_id,
                             ENOMEM, "unable to allocate completion prefix");
            return 0;
        }
        if (message->payload_length != 0)
            memcpy(prefix, message->payload, message->payload_length);
        prefix[message->payload_length] = '\0';
        result = shell_state_complete_path(state, prefix, &matches, &match_count);
        free(prefix);
        if (result < 0) {
            child_send_error(output->fd, output->tab_id, message->request_id,
                             errno, "unable to complete path");
        } else if (child_send_completion(output->fd, output->tab_id,
                                         message->request_id, matches,
                                         match_count) < 0) {
            child_send_error(output->fd, output->tab_id, message->request_id,
                             errno, "completion response is too large");
        }
        shell_completion_free(matches, match_count);
        return 0;
    }
    case IPC_MESSAGE_COMMAND:
        if (message->payload_length != 0 &&
            memchr(message->payload, '\0', message->payload_length) != NULL) {
            child_send_error(output->fd, output->tab_id, message->request_id,
                             EINVAL, "command contains a NUL byte");
            return 0;
        }
        command = malloc((size_t)message->payload_length + 1);
        if (command == NULL) {
            child_send_error(output->fd, output->tab_id, message->request_id,
                             ENOMEM, "unable to allocate command");
            return 0;
        }
        if (message->payload_length != 0)
            memcpy(command, message->payload, message->payload_length);
        command[message->payload_length] = '\0';
        output->request_id = message->request_id;
        status = shell_state_execute(state, command, &should_exit);
        free(command);
        if (child_send_status(output->fd, output->tab_id, message->request_id,
                              IPC_MESSAGE_COMMAND_STATUS, status) < 0)
            return -1;
        if (should_exit) {
            (void)child_send_status(output->fd, output->tab_id,
                                    message->request_id, IPC_MESSAGE_SHELL_EXIT,
                                    status);
            *done = 1;
        }
        return 0;
    default:
        child_send_error(output->fd, output->tab_id, message->request_id,
                         EPROTO, "unexpected request type");
        *done = 1;
        return -1;
    }
}

static int parse_int_arg(const char *text, int *value)
{
    char *end;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed < 0 || parsed > 0x7fffffffL)
        return -1;
    *value = (int)parsed;
    return 0;
}

int process_runtime_is_child_invocation(int argc, char **argv)
{
    return argc >= 2 && strcmp(argv[1], CHILD_OPTION) == 0;
}

int process_runtime_child_entry(int argc, char **argv)
{
    int input_fd, output_fd, control_read = -1, control_write = -1;
    int done = 0, shutdown_request = 0;
    char *end;
    unsigned long tab_value;
    ShellState state;
    History *history = NULL;
    ChildOutput output;
    IpcReader reader;
    struct sigaction ignore_pipe;
    if (argc != 5 || strcmp(argv[1], CHILD_OPTION) != 0 ||
        parse_int_arg(argv[2], &input_fd) < 0 ||
        parse_int_arg(argv[3], &output_fd) < 0) {
        (void)fprintf(stderr, "terminator: invalid internal process invocation\n");
        return 2;
    }
    errno = 0;
    tab_value = strtoul(argv[4], &end, 10);
    if (errno != 0 || *argv[4] == '\0' || *end != '\0' || tab_value > UINT32_MAX)
        return 2;

    child_shutdown_requested = 0;
    if (child_control_install(&control_read, &control_write) < 0) {
        close(input_fd); close(output_fd);
        return 1;
    }

    memset(&ignore_pipe, 0, sizeof(ignore_pipe));
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    (void)sigaction(SIGPIPE, &ignore_pipe, NULL);
    output.fd = output_fd;
    output.tab_id = (uint32_t)tab_value;
    output.request_id = 0;
    history = history_open_process_shared(NULL);
    if (history == NULL) {
        child_send_error(output_fd, output.tab_id, 0, errno,
                         "unable to attach shared history");
        close(input_fd); close(output_fd);
        close(control_read); close(control_write);
        return 1;
    }
    if (shell_state_init(&state, history, child_emit, &output) < 0) {
        child_send_error(output_fd, output.tab_id, 0, errno,
                         "unable to initialize shell state");
        close(input_fd);
        close(output_fd);
        close(control_read); close(control_write);
        history_close(history);
        return 1;
    }
    state.control_fd = control_read;
    state.process_backend = 1;
    if (child_send_ready(output_fd, output.tab_id) < 0) {
        shell_state_destroy(&state);
        close(input_fd); close(output_fd);
        close(control_read); close(control_write);
        history_close(history);
        return 1;
    }
    ipc_reader_init(&reader);
    while (!done) {
        struct pollfd poll_fds[2] = {
            { .fd = input_fd, .events = POLLIN },
            { .fd = control_read, .events = POLLIN }
        };
        int polled = poll(poll_fds, 2, 100);
        if (polled < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (polled == 0) {
            shell_state_reap_jobs(&state);
            continue;
        }
        if (poll_fds[1].revents & POLLIN) {
            if (shell_state_service_control(&state) || child_shutdown_requested) {
                shutdown_request = 1;
                done = 1;
            }
        }
        for (; !done && (poll_fds[0].revents & (POLLIN | POLLHUP));) {
            IpcMessage message = {0};
            IpcIoResult result = ipc_reader_read(&reader, input_fd, &message);
            if (result == IPC_IO_NEED_MORE)
                break;
            if (result == IPC_IO_EOF) {
                done = 1;
                break;
            }
            if (result != IPC_IO_MESSAGE) {
                child_send_error(output_fd, output.tab_id, 0,
                                 result == IPC_IO_TRUNCATED ? EPROTO : errno,
                                 "invalid or truncated request frame");
                done = 1;
                break;
            }
            if (child_process_message(&state, &output, &message, &done,
                                      &shutdown_request) < 0)
                done = 1;
            if (child_shutdown_requested) {
                shutdown_request = 1;
                done = 1;
            }
            ipc_message_dispose(&message);
            if (done)
                break;
        }
        if (!done)
            shell_state_reap_jobs(&state);
    }
    ipc_reader_reset(&reader);
    shell_state_shutdown(&state);
    if (shutdown_request)
        (void)child_send_status(output_fd, output.tab_id, 0,
                                IPC_MESSAGE_SHELL_EXIT, 0);
    shell_state_destroy(&state);
    history_close(history);
    close(input_fd);
    close(output_fd);
    child_control_fd = -1;
    close(control_read);
    close(control_write);
    return 0;
}

static void free_requests(Request *request)
{
    while (request != NULL) {
        Request *next = request->next;
        free(request->command);
        free(request);
        request = next;
    }
}

static void emit_exit(ProcessRuntime *runtime, int status)
{
    int emit = 0;
    (void)pthread_mutex_lock(&runtime->mutex);
    if (!runtime->exit_sent) {
        runtime->exit_sent = 1;
        emit = 1;
    }
    (void)pthread_mutex_unlock(&runtime->mutex);
    if (emit)
        runtime_notify(runtime, SHELL_EVENT_EXIT, NULL, 0, status, 0, NULL, 0);
}

static void request_free(Request *request)
{
    if (request == NULL) return;
    free(request->command);
    free(request);
}

static int decode_completion(const IpcMessage *message, char ***matches,
                             size_t *match_count)
{
    uint32_t count = get_u32(message->payload);
    size_t offset = 4, i;
    char **items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count != 0 && items == NULL) return -1;
    for (i = 0; i < count; ++i) {
        uint32_t length = get_u32(message->payload + offset);
        offset += 4;
        if (memchr(message->payload + offset, '\0', length) != NULL) {
            shell_completion_free(items, i);
            errno = EPROTO;
            return -1;
        }
        items[i] = malloc((size_t)length + 1);
        if (items[i] == NULL) {
            shell_completion_free(items, i);
            return -1;
        }
        memcpy(items[i], message->payload + offset, length);
        items[i][length] = '\0';
        offset += length;
    }
    *matches = items;
    *match_count = count;
    return 0;
}

static void *monitor_child(void *argument)
{
    ProcessRuntime *runtime = argument;
    IpcReader reader;
    IpcWriter writer;
    Request *active = NULL;
    int channel_open = 1, shell_exit_seen = 0, child_status = 1;
    int active_sent = 0;
    uint64_t last_status_id = 0;
    struct timespec stop_time = {0, 0};
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &set, NULL);
    ipc_reader_init(&reader);
    ipc_writer_init(&writer);

    while (channel_open) {
        struct pollfd fds[2];
        int have_output, timeout = 100, polled;
        IpcIoResult io;
        IpcMessage incoming = {0};
        Request *finished = NULL;
        int queue_failed = 0;
        IpcMessageType failed_type = 0;
        uint64_t failed_id = 0;
        (void)pthread_mutex_lock(&runtime->mutex);
        if (active == NULL && runtime->head != NULL) {
            active = runtime->head;
            runtime->head = active->next;
            if (runtime->head == NULL) runtime->tail = NULL;
            active->next = NULL;
            active_sent = 0;
        }
        if (runtime->stopping && stop_time.tv_sec == 0) {
            (void)clock_gettime(CLOCK_MONOTONIC, &stop_time);
            if (active == NULL && writer.frame == NULL) {
                IpcMessage shutdown_message = {0};
                shutdown_message.type = IPC_MESSAGE_SHUTDOWN;
                shutdown_message.tab_id = runtime->tab_id;
                shutdown_message.request_id = 0;
                if (ipc_writer_queue(&writer, &shutdown_message) < 0)
                    channel_open = 0;
            }
        }
        if (active != NULL && !active_sent && writer.frame == NULL) {
            IpcMessage request = {0};
            request.type = active->type;
            request.tab_id = runtime->tab_id;
            request.request_id = active->id;
            request.payload = (uint8_t *)active->command;
            request.payload_length = (uint32_t)strlen(active->command);
            if (ipc_writer_queue(&writer, &request) < 0) {
                finished = active;
                failed_type = active->type;
                failed_id = active->id;
                active = NULL;
                active_sent = 0;
                queue_failed = 1;
            }
        }
        have_output = writer.frame != NULL;
        (void)pthread_mutex_unlock(&runtime->mutex);
        free_requests(finished);
        if (queue_failed) {
            static const char failure[] = "terminator: unable to queue process command\n";
            if (failed_type == IPC_MESSAGE_COMPLETION_REQUEST) {
                runtime_notify(runtime, SHELL_EVENT_COMPLETION, NULL, 0, 1,
                               failed_id, NULL, 0);
            } else {
                runtime_notify(runtime, SHELL_EVENT_OUTPUT, failure,
                               sizeof(failure) - 1, 0, 0, NULL, 0);
                runtime_notify(runtime, SHELL_EVENT_COMMAND_DONE, NULL, 0, 1,
                               0, NULL, 0);
            }
        }
        if (!channel_open) break;

        fds[0].fd = runtime->response_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = runtime->command_fd;
        fds[1].events = have_output ? POLLOUT : 0;
        fds[1].revents = 0;
        polled = poll(fds, 2, timeout);
        if (polled < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & POLLOUT) {
            io = ipc_writer_flush(&writer, runtime->command_fd);
            if (io == IPC_IO_WRITTEN) {
                if (active != NULL)
                    active_sent = 1;
            } else if (io == IPC_IO_ERROR) {
                channel_open = 0;
            }
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            for (;;) {
                io = ipc_reader_read(&reader, runtime->response_fd, &incoming);
                if (io == IPC_IO_NEED_MORE) break;
                if (io == IPC_IO_EOF || io == IPC_IO_TRUNCATED || io == IPC_IO_ERROR) {
                    channel_open = 0;
                    break;
                }
                if (incoming.tab_id != runtime->tab_id) {
                    ipc_message_dispose(&incoming);
                    channel_open = 0;
                    break;
                }
                if (incoming.type == IPC_MESSAGE_READY) {
                    int interrupt, suspend, shutdown;
                    if (incoming.request_id != 0 ||
                        incoming.payload_length != 0 || runtime->ready) {
                        ipc_message_dispose(&incoming);
                        channel_open = 0;
                        break;
                    }
                    (void)pthread_mutex_lock(&runtime->mutex);
                    runtime->ready = 1;
                    interrupt = runtime->pending_interrupt;
                    suspend = runtime->pending_suspend;
                    shutdown = runtime->pending_shutdown;
                    runtime->pending_interrupt = 0;
                    runtime->pending_suspend = 0;
                    runtime->pending_shutdown = 0;
                    (void)pthread_mutex_unlock(&runtime->mutex);
                    if (interrupt) (void)kill(runtime->pid, SIGUSR1);
                    if (suspend) (void)kill(runtime->pid, SIGUSR2);
                    if (shutdown) (void)kill(runtime->pid, SIGTERM);
                    ipc_message_dispose(&incoming);
                    continue;
                }
                if (incoming.type != IPC_MESSAGE_OUTPUT_CHUNK &&
                    incoming.type != IPC_MESSAGE_COMMAND_STATUS &&
                    incoming.type != IPC_MESSAGE_COMPLETION_RESPONSE &&
                    incoming.type != IPC_MESSAGE_SHELL_EXIT &&
                    incoming.type != IPC_MESSAGE_JOB_STATE_UPDATE &&
                    incoming.type != IPC_MESSAGE_ERROR) {
                    ipc_message_dispose(&incoming);
                    channel_open = 0;
                    break;
                }
                if ((incoming.type == IPC_MESSAGE_OUTPUT_CHUNK ||
                     incoming.type == IPC_MESSAGE_COMMAND_STATUS) &&
                    (active == NULL || active->type != IPC_MESSAGE_COMMAND ||
                     active->id != incoming.request_id)) {
                    ipc_message_dispose(&incoming);
                    channel_open = 0;
                    break;
                }
                if (incoming.type == IPC_MESSAGE_COMPLETION_RESPONSE &&
                    (active == NULL ||
                     active->type != IPC_MESSAGE_COMPLETION_REQUEST ||
                     active->id != incoming.request_id)) {
                    ipc_message_dispose(&incoming);
                    channel_open = 0;
                    break;
                }
                if (incoming.type == IPC_MESSAGE_JOB_STATE_UPDATE &&
                    incoming.request_id != 0) {
                    ipc_message_dispose(&incoming);
                    channel_open = 0;
                    break;
                }
                if (incoming.type == IPC_MESSAGE_SHELL_EXIT &&
                    incoming.request_id != 0 &&
                    last_status_id != incoming.request_id) {
                    ipc_message_dispose(&incoming);
                    channel_open = 0;
                    break;
                }
                if (incoming.type == IPC_MESSAGE_ERROR && incoming.request_id != 0 &&
                    (active == NULL || active->id != incoming.request_id)) {
                    ipc_message_dispose(&incoming);
                    channel_open = 0;
                    break;
                }
                if (incoming.type == IPC_MESSAGE_OUTPUT_CHUNK) {
                    runtime_notify(runtime, SHELL_EVENT_OUTPUT,
                        (const char *)incoming.payload, incoming.payload_length,
                        0, incoming.request_id, NULL, 0);
                } else if (incoming.type == IPC_MESSAGE_JOB_STATE_UPDATE) {
                    runtime_notify(runtime, SHELL_EVENT_JOB_STATE,
                        (const char *)incoming.payload, incoming.payload_length,
                        incoming.payload[12], 0, NULL, 0);
                } else if (incoming.type == IPC_MESSAGE_COMMAND_STATUS) {
                    int status = (int32_t)get_u32(incoming.payload);
                    last_status_id = incoming.request_id;
                    runtime_notify(runtime, SHELL_EVENT_COMMAND_DONE,
                                   NULL, 0, status, incoming.request_id, NULL, 0);
                    if (active != NULL && active->id == incoming.request_id) {
                        free(active->command);
                        free(active);
                        active = NULL;
                        active_sent = 0;
                    }
                } else if (incoming.type == IPC_MESSAGE_COMPLETION_RESPONSE) {
                    char **matches = NULL;
                    size_t match_count = 0;
                    int status = decode_completion(&incoming, &matches,
                                                   &match_count) == 0 ? 0 : errno;
                    runtime_notify(runtime, SHELL_EVENT_COMPLETION, NULL, 0,
                                   status, incoming.request_id, matches,
                                   match_count);
                    shell_completion_free(matches, match_count);
                    request_free(active);
                    active = NULL;
                    active_sent = 0;
                } else if (incoming.type == IPC_MESSAGE_SHELL_EXIT) {
                    child_status = (int32_t)get_u32(incoming.payload);
                    shell_exit_seen = 1;
                    emit_exit(runtime, child_status);
                } else if (incoming.type == IPC_MESSAGE_ERROR) {
                    char prefix[128];
                    uint32_t code = get_u32(incoming.payload);
                    size_t detail_length = incoming.payload_length - 4;
                    int prefix_length = snprintf(prefix, sizeof(prefix),
                                                 "terminator: process child error %u: ",
                                                 code);
                    if (active != NULL &&
                        active->type == IPC_MESSAGE_COMPLETION_REQUEST) {
                        runtime_notify(runtime, SHELL_EVENT_COMPLETION, NULL, 0,
                                       (int)code, incoming.request_id, NULL, 0);
                        request_free(active);
                        active = NULL;
                        active_sent = 0;
                    } else {
                        if (prefix_length > 0)
                            runtime_notify(runtime, SHELL_EVENT_OUTPUT, prefix,
                                (size_t)prefix_length, 0, incoming.request_id,
                                NULL, 0);
                        if (detail_length != 0)
                            runtime_notify(runtime, SHELL_EVENT_OUTPUT,
                                (const char *)incoming.payload + 4,
                                detail_length, 0, incoming.request_id, NULL, 0);
                        runtime_notify(runtime, SHELL_EVENT_OUTPUT, "\n", 1,
                                       0, incoming.request_id, NULL, 0);
                        if (active != NULL &&
                            active->type == IPC_MESSAGE_COMMAND) {
                            runtime_notify(runtime, SHELL_EVENT_COMMAND_DONE,
                                           NULL, 0, 1, incoming.request_id,
                                           NULL, 0);
                            request_free(active);
                            active = NULL;
                            active_sent = 0;
                        }
                    }
                }
                ipc_message_dispose(&incoming);
                memset(&incoming, 0, sizeof(incoming));
            }
        }
        if (fds[0].revents & (POLLNVAL | POLLERR))
            channel_open = 0;

        if (stop_time.tv_sec != 0) {
            struct timespec now;
            int64_t elapsed_ms;
            (void)clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed_ms = (int64_t)(now.tv_sec - stop_time.tv_sec) * 1000 +
                         (now.tv_nsec - stop_time.tv_nsec) / 1000000;
            if (elapsed_ms >= 1000)
                (void)kill(runtime->pid, SIGKILL);
            else if (elapsed_ms >= 500)
                (void)kill(runtime->pid, SIGTERM);
        }
    }

    ipc_reader_reset(&reader);
    ipc_writer_reset(&writer);
    if (active != NULL) {
        free(active->command);
        free(active);
    }
    (void)close(runtime->command_fd);
    (void)close(runtime->response_fd);
    while (waitpid(runtime->pid, &child_status, 0) < 0 && errno == EINTR) { }
    if (!shell_exit_seen) {
        int status = WIFEXITED(child_status) ? WEXITSTATUS(child_status) :
                     WIFSIGNALED(child_status) ? 128 + WTERMSIG(child_status) : 1;
        emit_exit(runtime, status);
    }
    (void)pthread_mutex_lock(&runtime->mutex);
    runtime->exited = 1;
    (void)pthread_mutex_unlock(&runtime->mutex);
    return NULL;
}

static void close_all_except(int max_fd, int keep_a, int keep_b)
{
    int fd;
    for (fd = STDERR_FILENO + 1; fd < max_fd; ++fd)
        if (fd != keep_a && fd != keep_b)
            (void)close(fd);
}

ProcessRuntime *process_runtime_create(uint32_t tab_id, int x11_fd,
                                       ProcessRuntimeEvent event,
                                       void *event_context)
{
    int to_child[2] = {-1, -1}, from_child[2] = {-1, -1};
    int max_fd, flags;
    char input_text[32], output_text[32], tab_text[32];
    char *child_argv[6];
    ProcessRuntime *runtime = NULL;
    pid_t pid;
    if (event == NULL) { errno = EINVAL; return NULL; }
    runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) return NULL;
    runtime->command_fd = runtime->response_fd = -1;
    runtime->tab_id = tab_id;
    runtime->event = event;
    runtime->event_context = event_context;
    runtime->next_id = 1;
    if (pthread_mutex_init(&runtime->mutex, NULL) != 0) goto fail;
    runtime->mutex_ready = 1;
    if (pipe(to_child) < 0 || pipe(from_child) < 0) goto fail;
    for (int i = 0; i < 2; ++i) {
        if (set_fd_flag(to_child[i], F_SETFD, FD_CLOEXEC) < 0 ||
            set_fd_flag(from_child[i], F_SETFD, FD_CLOEXEC) < 0)
            goto fail;
    }
    max_fd = (int)sysconf(_SC_OPEN_MAX);
    if (max_fd < 64) max_fd = 65536;
    (void)snprintf(input_text, sizeof(input_text), "%d", to_child[0]);
    (void)snprintf(output_text, sizeof(output_text), "%d", from_child[1]);
    (void)snprintf(tab_text, sizeof(tab_text), "%u", tab_id);
    child_argv[0] = (char *)"terminator";
    child_argv[1] = (char *)CHILD_OPTION;
    child_argv[2] = input_text;
    child_argv[3] = output_text;
    child_argv[4] = tab_text;
    child_argv[5] = NULL;
    pid = fork();
    if (pid < 0) goto fail;
    if (pid == 0) {
        int descriptor_flags;
        descriptor_flags = fcntl(to_child[0], F_GETFD);
        if (descriptor_flags >= 0)
            (void)fcntl(to_child[0], F_SETFD, descriptor_flags & ~FD_CLOEXEC);
        descriptor_flags = fcntl(from_child[1], F_GETFD);
        if (descriptor_flags >= 0)
            (void)fcntl(from_child[1], F_SETFD, descriptor_flags & ~FD_CLOEXEC);
        if (x11_fd > STDERR_FILENO && x11_fd != to_child[0] &&
            x11_fd != from_child[1])
            (void)close(x11_fd);
        close_all_except(max_fd, to_child[0], from_child[1]);
        execve("/proc/self/exe", child_argv, environ);
        _exit(127);
    }
    (void)close(to_child[0]); to_child[0] = -1;
    (void)close(from_child[1]); from_child[1] = -1;
    runtime->command_fd = to_child[1]; to_child[1] = -1;
    runtime->response_fd = from_child[0]; from_child[0] = -1;
    flags = fcntl(runtime->command_fd, F_GETFL);
    if (flags < 0 || fcntl(runtime->command_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail_child;
    flags = fcntl(runtime->response_fd, F_GETFL);
    if (flags < 0 || fcntl(runtime->response_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail_child;
    runtime->pid = pid;
    if (pthread_create(&runtime->monitor, NULL, monitor_child, runtime) != 0)
        goto fail_child;
    runtime->monitor_started = 1;
    return runtime;

fail_child:
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) { }
fail:
    if (to_child[0] >= 0) close(to_child[0]);
    if (to_child[1] >= 0) close(to_child[1]);
    if (from_child[0] >= 0) close(from_child[0]);
    if (from_child[1] >= 0) close(from_child[1]);
    if (runtime != NULL) {
        if (runtime->command_fd >= 0) close(runtime->command_fd);
        if (runtime->response_fd >= 0) close(runtime->response_fd);
        if (runtime->mutex_ready) pthread_mutex_destroy(&runtime->mutex);
        free(runtime);
    }
    return NULL;
}

static int queue_request(ProcessRuntime *runtime, IpcMessageType type,
                         const char *text, uint64_t *request_id)
{
    Request *request;
    size_t length;
    if (runtime == NULL || text == NULL) { errno = EINVAL; return -1; }
    length = strlen(text);
    if (length > IPC_MAX_PAYLOAD_SIZE) { errno = EMSGSIZE; return -1; }
    request = calloc(1, sizeof(*request));
    if (request == NULL) return -1;
    request->command = strdup(text);
    if (request->command == NULL) { free(request); return -1; }
    request->type = type;
    (void)pthread_mutex_lock(&runtime->mutex);
    if (runtime->stopping || runtime->exited) {
        (void)pthread_mutex_unlock(&runtime->mutex);
        free_requests(request);
        errno = EPIPE;
        return -1;
    }
    request->id = runtime->next_id++;
    if (request->id == 0) request->id = runtime->next_id++;
    if (request_id != NULL)
        *request_id = request->id;
    if (runtime->tail == NULL) runtime->head = request;
    else runtime->tail->next = request;
    runtime->tail = request;
    (void)pthread_mutex_unlock(&runtime->mutex);
    return 0;
}

int process_runtime_submit(ProcessRuntime *runtime, const char *command)
{
    return queue_request(runtime, IPC_MESSAGE_COMMAND, command, NULL);
}

int process_runtime_signal(ProcessRuntime *runtime, int suspend)
{
    pid_t child;
    int result;
    if (runtime == NULL) { errno = EINVAL; return -1; }
    (void)pthread_mutex_lock(&runtime->mutex);
    child = (runtime->exited || runtime->exit_sent) ? -1 :
            runtime->ready ? runtime->pid : 0;
    if (child == 0) {
        if (suspend) runtime->pending_suspend = 1;
        else runtime->pending_interrupt = 1;
        (void)pthread_mutex_unlock(&runtime->mutex);
        return 0;
    }
    (void)pthread_mutex_unlock(&runtime->mutex);
    if (child <= 0) { errno = ESRCH; return -1; }
    result = kill(child, suspend ? SIGUSR2 : SIGUSR1);
    return result;
}

int process_runtime_complete(ProcessRuntime *runtime, const char *prefix,
                             uint64_t *request_id)
{
    if (request_id == NULL) { errno = EINVAL; return -1; }
    return queue_request(runtime, IPC_MESSAGE_COMPLETION_REQUEST, prefix,
                         request_id);
}

int process_runtime_child_pid(const ProcessRuntime *runtime)
{
    return runtime == NULL ? -1 : (int)runtime->pid;
}

void process_runtime_shutdown(ProcessRuntime *runtime)
{
    if (runtime == NULL) return;
    (void)pthread_mutex_lock(&runtime->mutex);
    runtime->stopping = 1;
    pid_t child = (runtime->exited || runtime->exit_sent) ? -1 :
                  runtime->ready ? runtime->pid : 0;
    if (child == 0)
        runtime->pending_shutdown = 1;
    (void)pthread_mutex_unlock(&runtime->mutex);
    if (child > 0)
        (void)kill(child, SIGTERM);
}

void process_runtime_destroy(ProcessRuntime *runtime)
{
    if (runtime == NULL) return;
    process_runtime_shutdown(runtime);
    if (runtime->monitor_started)
        (void)pthread_join(runtime->monitor, NULL);
    free_requests(runtime->head);
    if (runtime->mutex_ready)
        (void)pthread_mutex_destroy(&runtime->mutex);
    free(runtime);
}

int process_runtime_parse_args(int argc, char **argv, int *use_process)
{
    if (use_process == NULL || argc < 1 || argv == NULL) {
        errno = EINVAL;
        return -1;
    }
    *use_process = 0;
    if (argc == 1) return 0;
    if (argc == 2 && strcmp(argv[1], "useProcess=True") == 0) {
        *use_process = 1;
        return 0;
    }
    errno = EINVAL;
    return -1;
}
