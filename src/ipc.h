#ifndef TERMINATOR_IPC_H
#define TERMINATOR_IPC_H

#include <stddef.h>
#include <stdint.h>

#define IPC_PROTOCOL_MAGIC UINT32_C(0x5445524d) /* "TERM" */
#define IPC_PROTOCOL_VERSION UINT16_C(1)
#define IPC_HEADER_SIZE 24u
#define IPC_MAX_PAYLOAD_SIZE (1024u * 1024u)

typedef enum {
    IPC_MESSAGE_COMMAND = 1,
    IPC_MESSAGE_COMPLETION_REQUEST = 2,
    IPC_MESSAGE_SHUTDOWN = 3,
    IPC_MESSAGE_OUTPUT_CHUNK = 4,
    IPC_MESSAGE_COMMAND_STATUS = 5,
    IPC_MESSAGE_SHELL_EXIT = 6,
    IPC_MESSAGE_COMPLETION_RESPONSE = 7,
    IPC_MESSAGE_JOB_STATE_UPDATE = 8,
    IPC_MESSAGE_ERROR = 9,
    IPC_MESSAGE_READY = 10
} IpcMessageType;

typedef struct {
    IpcMessageType type;
    uint32_t tab_id;
    uint64_t request_id;
    uint32_t payload_length;
    /* Reader-allocated; ownership transfers to the caller on MESSAGE. */
    uint8_t *payload;
} IpcMessage;

typedef struct {
    uint8_t header[IPC_HEADER_SIZE];
    size_t header_used;
    uint8_t *payload;
    uint32_t payload_length;
    size_t payload_used;
    IpcMessageType type;
    uint32_t tab_id;
    uint64_t request_id;
} IpcReader;

typedef struct {
    uint8_t *frame;
    size_t frame_length;
    size_t frame_offset;
} IpcWriter;

typedef enum {
    IPC_IO_ERROR = -1,
    IPC_IO_NEED_MORE = 0,
    IPC_IO_MESSAGE = 1,
    IPC_IO_EOF = 2,
    IPC_IO_TRUNCATED = 3,
    IPC_IO_WRITTEN = 4
} IpcIoResult;

void ipc_message_dispose(IpcMessage *message);
void ipc_reader_init(IpcReader *reader);
void ipc_reader_reset(IpcReader *reader);
/* out must be empty or previously disposed. Reads until one message, EOF,
 * truncation, a fatal protocol/I/O error, or EAGAIN/EWOULDBLOCK. */
IpcIoResult ipc_reader_read(IpcReader *reader, int fd, IpcMessage *out);

void ipc_writer_init(IpcWriter *writer);
void ipc_writer_reset(IpcWriter *writer);
/* Queue copies the payload. Only one frame may be queued at a time. */
int ipc_writer_queue(IpcWriter *writer, const IpcMessage *message);
/* Writes until the frame completes or a nonblocking descriptor would block. */
IpcIoResult ipc_writer_flush(IpcWriter *writer, int fd);

#endif
