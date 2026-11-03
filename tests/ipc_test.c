#define _POSIX_C_SOURCE 200809L

#include "ipc.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void
set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0);
    assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void
put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void
put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void
put_u64(uint8_t *p, uint64_t value)
{
    put_u32(p, (uint32_t)(value >> 32));
    put_u32(p + 4, (uint32_t)value);
}

static uint8_t *
make_frame(uint32_t magic, uint16_t version, uint16_t type, uint32_t tab,
           uint64_t request, const uint8_t *payload, uint32_t length,
           size_t *frame_length)
{
    uint8_t *frame = malloc(IPC_HEADER_SIZE + (size_t)length);
    assert(frame != NULL);
    put_u32(frame, magic);
    put_u16(frame + 4, version);
    put_u16(frame + 6, type);
    put_u32(frame + 8, tab);
    put_u64(frame + 12, request);
    put_u32(frame + 20, length);
    if (length != 0 && payload != NULL)
        memcpy(frame + IPC_HEADER_SIZE, payload, length);
    *frame_length = IPC_HEADER_SIZE + (size_t)length;
    return frame;
}

static void
write_all(int fd, const uint8_t *bytes, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t amount = write(fd, bytes + offset, length - offset);
        if (amount > 0)
            offset += (size_t)amount;
        else if (amount < 0 && errno == EINTR)
            continue;
        else
            assert(!"write_all failed");
    }
}

static void
test_fragmented_reader(void)
{
    int fds[2];
    IpcReader reader;
    IpcMessage message = {0};
    static const uint8_t payload[] = {0, 1, 2, 0xfe, 0xff};
    size_t frame_length, offset = 0;
    uint8_t *frame = make_frame(IPC_PROTOCOL_MAGIC, IPC_PROTOCOL_VERSION,
                                IPC_MESSAGE_OUTPUT_CHUNK, 17, 0x1020304050607080ULL,
                                payload, sizeof(payload), &frame_length);
    assert(pipe(fds) == 0);
    set_nonblocking(fds[0]);
    ipc_reader_init(&reader);
    while (offset < frame_length) {
        size_t chunk = (offset % 5) + 1;
        if (chunk > frame_length - offset)
            chunk = frame_length - offset;
        write_all(fds[1], frame + offset, chunk);
        offset += chunk;
        IpcIoResult result = ipc_reader_read(&reader, fds[0], &message);
        if (offset < frame_length)
            assert(result == IPC_IO_NEED_MORE);
        else
            assert(result == IPC_IO_MESSAGE);
    }
    assert(message.type == IPC_MESSAGE_OUTPUT_CHUNK);
    assert(message.tab_id == 17);
    assert(message.request_id == 0x1020304050607080ULL);
    assert(message.payload_length == sizeof(payload));
    assert(memcmp(message.payload, payload, sizeof(payload)) == 0);
    ipc_message_dispose(&message);
    assert(close(fds[1]) == 0);
    assert(ipc_reader_read(&reader, fds[0], &message) == IPC_IO_EOF);
    ipc_reader_reset(&reader);
    close(fds[0]);
    free(frame);
}

static void
test_writer_partial_nonblocking_and_copy(void)
{
    int fds[2];
    const size_t payload_length = 256 * 1024;
    uint8_t *source = malloc(payload_length);
    IpcMessage outgoing = {0}, incoming = {0};
    IpcReader reader;
    IpcWriter writer;
    size_t i;
    int saw_block = 0, received = 0;
    assert(source != NULL);
    for (i = 0; i < payload_length; ++i)
        source[i] = (uint8_t)(i * 37u);
    assert(pipe(fds) == 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);
    outgoing.type = IPC_MESSAGE_OUTPUT_CHUNK;
    outgoing.tab_id = 3;
    outgoing.request_id = 99;
    outgoing.payload = source;
    outgoing.payload_length = (uint32_t)payload_length;
    ipc_reader_init(&reader);
    ipc_writer_init(&writer);
    assert(ipc_writer_queue(&writer, &outgoing) == 0);
    memset(source, 0xa5, payload_length); /* queued frame owns a copy */
    free(source);

    for (i = 0; i < 1000 && !received; ++i) {
        IpcIoResult write_result = ipc_writer_flush(&writer, fds[1]);
        assert(write_result == IPC_IO_NEED_MORE || write_result == IPC_IO_WRITTEN);
        if (write_result == IPC_IO_NEED_MORE)
            saw_block = 1;
        IpcIoResult read_result = ipc_reader_read(&reader, fds[0], &incoming);
        if (read_result == IPC_IO_MESSAGE)
            received = 1;
        else
            assert(read_result == IPC_IO_NEED_MORE);
    }
    assert(saw_block); /* forces write() to make partial progress and hit EAGAIN */
    assert(received);
    assert(incoming.payload_length == payload_length);
    for (i = 0; i < payload_length; ++i)
        assert(incoming.payload[i] == (uint8_t)(i * 37u));
    ipc_message_dispose(&incoming);
    ipc_reader_reset(&reader);
    ipc_writer_reset(&writer);
    close(fds[0]);
    close(fds[1]);
}

static void
expect_bad_frame(uint8_t *frame, size_t length, int expected_errno)
{
    int fds[2];
    IpcReader reader;
    IpcMessage message = {0};
    assert(pipe(fds) == 0);
    set_nonblocking(fds[0]);
    write_all(fds[1], frame, length);
    ipc_reader_init(&reader);
    errno = 0;
    assert(ipc_reader_read(&reader, fds[0], &message) == IPC_IO_ERROR);
    assert(errno == expected_errno);
    ipc_reader_reset(&reader);
    ipc_message_dispose(&message);
    close(fds[0]);
    close(fds[1]);
}

static void
test_malformed_frames(void)
{
    size_t length;
    uint8_t *frame;
    uint8_t length_payload[4] = {0, 0, 0, 0};
    uint8_t malformed_completion[8] = {0, 0, 0, 1, 0, 0, 0, 9};

    frame = make_frame(0, IPC_PROTOCOL_VERSION, IPC_MESSAGE_SHUTDOWN,
                       0, 0, NULL, 0, &length);
    expect_bad_frame(frame, length, EPROTO);
    free(frame);

    frame = make_frame(IPC_PROTOCOL_MAGIC, IPC_PROTOCOL_VERSION + 1,
                       IPC_MESSAGE_SHUTDOWN, 0, 0, NULL, 0, &length);
    expect_bad_frame(frame, length, EPROTO);
    free(frame);

    frame = make_frame(IPC_PROTOCOL_MAGIC, IPC_PROTOCOL_VERSION, 999,
                       0, 0, NULL, 0, &length);
    expect_bad_frame(frame, length, EPROTO);
    free(frame);

    frame = make_frame(IPC_PROTOCOL_MAGIC, IPC_PROTOCOL_VERSION,
                       IPC_MESSAGE_COMMAND_STATUS, 0, 0, NULL, 0, &length);
    expect_bad_frame(frame, length, EPROTO);
    free(frame);

    frame = make_frame(IPC_PROTOCOL_MAGIC, IPC_PROTOCOL_VERSION,
                       IPC_MESSAGE_OUTPUT_CHUNK, 0, 0, NULL,
                       IPC_MAX_PAYLOAD_SIZE + 1u, &length);
    expect_bad_frame(frame, IPC_HEADER_SIZE, EMSGSIZE);
    free(frame);

    frame = make_frame(IPC_PROTOCOL_MAGIC, IPC_PROTOCOL_VERSION,
                       IPC_MESSAGE_COMPLETION_RESPONSE, 0, 0,
                       malformed_completion, sizeof(malformed_completion), &length);
    expect_bad_frame(frame, length, EPROTO);
    free(frame);

    frame = make_frame(IPC_PROTOCOL_MAGIC, IPC_PROTOCOL_VERSION,
                       IPC_MESSAGE_COMPLETION_RESPONSE, 0, 0,
                       length_payload, sizeof(length_payload), &length);
    /* An empty candidate list is a valid completion response. */
    {
        int fds[2];
        IpcReader reader;
        IpcMessage message = {0};
        assert(pipe(fds) == 0);
        set_nonblocking(fds[0]);
        write_all(fds[1], frame, length);
        ipc_reader_init(&reader);
        assert(ipc_reader_read(&reader, fds[0], &message) == IPC_IO_MESSAGE);
        assert(message.type == IPC_MESSAGE_COMPLETION_RESPONSE);
        ipc_message_dispose(&message);
        ipc_reader_reset(&reader);
        close(fds[0]);
        close(fds[1]);
    }
    free(frame);
}

static void
test_truncated_frames(void)
{
    int fds[2];
    IpcReader reader;
    IpcMessage message = {0};
    size_t length;
    uint8_t payload[5] = {1, 2, 3, 4, 5};
    uint8_t *frame = make_frame(IPC_PROTOCOL_MAGIC, IPC_PROTOCOL_VERSION,
                                IPC_MESSAGE_OUTPUT_CHUNK, 1, 2,
                                payload, sizeof(payload), &length);
    assert(pipe(fds) == 0);
    ipc_reader_init(&reader);
    write_all(fds[1], frame, 7);
    close(fds[1]);
    assert(ipc_reader_read(&reader, fds[0], &message) == IPC_IO_TRUNCATED);
    ipc_reader_reset(&reader);
    close(fds[0]);

    assert(pipe(fds) == 0);
    ipc_reader_init(&reader);
    write_all(fds[1], frame, IPC_HEADER_SIZE + 2);
    close(fds[1]);
    assert(ipc_reader_read(&reader, fds[0], &message) == IPC_IO_TRUNCATED);
    ipc_reader_reset(&reader);
    close(fds[0]);
    ipc_message_dispose(&message);
    free(frame);
}

static void
test_writer_rejects_invalid_messages(void)
{
    IpcWriter writer;
    IpcMessage message = {0};
    ipc_writer_init(&writer);
    message.type = (IpcMessageType)100;
    assert(ipc_writer_queue(&writer, &message) < 0 && errno == EPROTO);
    message.type = IPC_MESSAGE_SHUTDOWN;
    message.payload_length = 1;
    message.payload = (uint8_t *)"x";
    assert(ipc_writer_queue(&writer, &message) < 0 && errno == EPROTO);
    message.type = IPC_MESSAGE_OUTPUT_CHUNK;
    message.payload_length = IPC_MAX_PAYLOAD_SIZE + 1u;
    assert(ipc_writer_queue(&writer, &message) < 0 && errno == EMSGSIZE);
    ipc_writer_reset(&writer);
}

static void
test_all_message_types(void)
{
    int fds[2];
    IpcReader reader;
    IpcWriter writer;
    IpcMessage incoming = {0}, outgoing = {0};
    uint8_t status[4] = {0, 0, 0, 7};
    uint8_t completion[4] = {0, 0, 0, 0};
    uint8_t job_update[16] = {0};
    uint8_t error[4] = {0, 0, 0, 2};
    uint8_t command[] = "echo hello";
    struct {
        IpcMessageType type;
        const uint8_t *payload;
        uint32_t length;
    } cases[] = {
        {IPC_MESSAGE_COMMAND, command, sizeof(command) - 1},
        {IPC_MESSAGE_COMPLETION_REQUEST, NULL, 0},
        {IPC_MESSAGE_SHUTDOWN, NULL, 0},
        {IPC_MESSAGE_OUTPUT_CHUNK, command, sizeof(command) - 1},
        {IPC_MESSAGE_COMMAND_STATUS, status, sizeof(status)},
        {IPC_MESSAGE_SHELL_EXIT, status, sizeof(status)},
        {IPC_MESSAGE_COMPLETION_RESPONSE, completion, sizeof(completion)},
        {IPC_MESSAGE_JOB_STATE_UPDATE, job_update, sizeof(job_update)},
        {IPC_MESSAGE_ERROR, error, sizeof(error)},
        {IPC_MESSAGE_READY, NULL, 0}
    };
    size_t i;
    assert(pipe(fds) == 0);
    ipc_reader_init(&reader);
    ipc_writer_init(&writer);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        outgoing.type = cases[i].type;
        outgoing.tab_id = (uint32_t)(i + 1);
        outgoing.request_id = 100 + i;
        outgoing.payload = (uint8_t *)cases[i].payload;
        outgoing.payload_length = cases[i].length;
        assert(ipc_writer_queue(&writer, &outgoing) == 0);
        assert(ipc_writer_flush(&writer, fds[1]) == IPC_IO_WRITTEN);
        assert(ipc_reader_read(&reader, fds[0], &incoming) == IPC_IO_MESSAGE);
        assert(incoming.type == cases[i].type);
        assert(incoming.tab_id == i + 1 && incoming.request_id == 100 + i);
        assert(incoming.payload_length == cases[i].length);
        if (cases[i].length != 0)
            assert(memcmp(incoming.payload, cases[i].payload, cases[i].length) == 0);
        ipc_message_dispose(&incoming);
    }
    ipc_reader_reset(&reader);
    ipc_writer_reset(&writer);
    close(fds[0]);
    close(fds[1]);
}

typedef struct {
    int write_fd;
    pthread_t target;
    const uint8_t *frame;
    size_t frame_length;
} InterruptReadContext;

static volatile sig_atomic_t got_test_signal;

static void
record_signal(int signal_number)
{
    (void)signal_number;
    got_test_signal = 1;
}

static void *
interrupt_then_write(void *argument)
{
    InterruptReadContext *context = argument;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 30000000};
    (void)nanosleep(&pause, NULL);
    assert(pthread_kill(context->target, SIGUSR1) == 0);
    (void)nanosleep(&pause, NULL);
    write_all(context->write_fd, context->frame, context->frame_length);
    return NULL;
}

static void
test_reader_retries_eintr(void)
{
    int fds[2];
    struct sigaction action;
    pthread_t writer_thread;
    InterruptReadContext context;
    IpcReader reader;
    IpcMessage message = {0};
    size_t frame_length;
    uint8_t *frame = make_frame(IPC_PROTOCOL_MAGIC, IPC_PROTOCOL_VERSION,
                                IPC_MESSAGE_SHUTDOWN, 4, 5, NULL, 0, &frame_length);
    memset(&action, 0, sizeof(action));
    action.sa_handler = record_signal;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    assert(pipe(fds) == 0);
    ipc_reader_init(&reader);
    context.write_fd = fds[1];
    context.target = pthread_self();
    context.frame = frame;
    context.frame_length = frame_length;
    assert(pthread_create(&writer_thread, NULL, interrupt_then_write, &context) == 0);
    assert(ipc_reader_read(&reader, fds[0], &message) == IPC_IO_MESSAGE);
    assert(pthread_join(writer_thread, NULL) == 0);
    assert(got_test_signal);
    assert(message.type == IPC_MESSAGE_SHUTDOWN && message.tab_id == 4);
    ipc_message_dispose(&message);
    ipc_reader_reset(&reader);
    close(fds[0]);
    close(fds[1]);
    free(frame);
}

typedef struct {
    int read_fd;
    pthread_t target;
    size_t bytes_to_drain;
} InterruptWriteContext;

static void *
interrupt_then_drain(void *argument)
{
    InterruptWriteContext *context = argument;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 30000000};
    uint8_t scratch[4096];
    size_t drained = 0;
    (void)nanosleep(&pause, NULL);
    assert(pthread_kill(context->target, SIGUSR1) == 0);
    while (drained < context->bytes_to_drain) {
        size_t wanted = context->bytes_to_drain - drained;
        if (wanted > sizeof(scratch))
            wanted = sizeof(scratch);
        ssize_t amount = read(context->read_fd, scratch, wanted);
        if (amount > 0)
            drained += (size_t)amount;
        else if (amount < 0 && errno == EINTR)
            continue;
        else
            assert(!"failed draining full pipe");
    }
    return NULL;
}

static void
test_writer_retries_eintr(void)
{
    int fds[2];
    int flags;
    uint8_t filler[4096] = {0};
    size_t filled = 0;
    struct sigaction action;
    pthread_t drain_thread;
    InterruptWriteContext context;
    IpcWriter writer;
    IpcReader reader;
    IpcMessage outgoing = {0}, incoming = {0};
    uint8_t status[4] = {0, 0, 0, 1};

    memset(&action, 0, sizeof(action));
    action.sa_handler = record_signal;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    assert(pipe(fds) == 0);
    set_nonblocking(fds[1]);
    for (;;) {
        ssize_t amount = write(fds[1], filler, sizeof(filler));
        if (amount > 0)
            filled += (size_t)amount;
        else if (amount < 0 && errno == EINTR)
            continue;
        else {
            assert(amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
            break;
        }
    }
    assert(filled != 0);
    flags = fcntl(fds[1], F_GETFL);
    assert(flags >= 0 && fcntl(fds[1], F_SETFL, flags & ~O_NONBLOCK) == 0);

    ipc_writer_init(&writer);
    ipc_reader_init(&reader);
    outgoing.type = IPC_MESSAGE_COMMAND_STATUS;
    outgoing.tab_id = 8;
    outgoing.request_id = 11;
    outgoing.payload = status;
    outgoing.payload_length = sizeof(status);
    assert(ipc_writer_queue(&writer, &outgoing) == 0);
    context.read_fd = fds[0];
    context.target = pthread_self();
    context.bytes_to_drain = filled;
    got_test_signal = 0;
    assert(pthread_create(&drain_thread, NULL, interrupt_then_drain, &context) == 0);
    assert(ipc_writer_flush(&writer, fds[1]) == IPC_IO_WRITTEN);
    assert(pthread_join(drain_thread, NULL) == 0);
    assert(got_test_signal);
    assert(ipc_reader_read(&reader, fds[0], &incoming) == IPC_IO_MESSAGE);
    assert(incoming.type == IPC_MESSAGE_COMMAND_STATUS && incoming.tab_id == 8);
    assert(memcmp(incoming.payload, status, sizeof(status)) == 0);
    ipc_message_dispose(&incoming);
    ipc_writer_reset(&writer);
    ipc_reader_reset(&reader);
    close(fds[0]);
    close(fds[1]);
}

int
main(void)
{
    test_fragmented_reader();
    test_writer_partial_nonblocking_and_copy();
    test_malformed_frames();
    test_truncated_frames();
    test_writer_rejects_invalid_messages();
    test_all_message_types();
    test_reader_retries_eintr();
    test_writer_retries_eintr();
    puts("IPC framing tests passed");
    return 0;
}
