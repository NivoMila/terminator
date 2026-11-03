#define _POSIX_C_SOURCE 200809L

#include "ipc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

static void
put_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void
put_u64(uint8_t *dst, uint64_t value)
{
    put_u32(dst, (uint32_t)(value >> 32));
    put_u32(dst + 4, (uint32_t)value);
}

static uint16_t
get_u16(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

static uint32_t
get_u32(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8) | (uint32_t)src[3];
}

static uint64_t
get_u64(const uint8_t *src)
{
    return ((uint64_t)get_u32(src) << 32) | get_u32(src + 4);
}

static int
message_type_valid(uint16_t type)
{
    return type >= IPC_MESSAGE_COMMAND && type <= IPC_MESSAGE_READY;
}

static int
header_payload_valid(IpcMessageType type, uint32_t length)
{
    switch (type) {
    case IPC_MESSAGE_READY:
        return length == 0;
    case IPC_MESSAGE_SHUTDOWN:
        return length == 0;
    case IPC_MESSAGE_COMMAND_STATUS:
    case IPC_MESSAGE_SHELL_EXIT:
        return length == 4;
    case IPC_MESSAGE_COMPLETION_RESPONSE:
        return length >= 4;
    case IPC_MESSAGE_JOB_STATE_UPDATE:
        return length == 16;
    case IPC_MESSAGE_ERROR:
        return length >= 4;
    case IPC_MESSAGE_COMMAND:
    case IPC_MESSAGE_COMPLETION_REQUEST:
    case IPC_MESSAGE_OUTPUT_CHUNK:
        return 1;
    }
    return 0;
}

static int
payload_valid(IpcMessageType type, const uint8_t *payload, uint32_t length)
{
    size_t offset;
    uint32_t count;
    uint32_t i;

    if (!header_payload_valid(type, length))
        return 0;
    if (type == IPC_MESSAGE_JOB_STATE_UPDATE &&
        (payload[12] > 3 || (payload[13] & (uint8_t)~1u) != 0 ||
         payload[14] != 0 || payload[15] != 0))
        return 0;
    if (type != IPC_MESSAGE_COMPLETION_RESPONSE)
        return 1;

    count = get_u32(payload);
    offset = 4;
    if ((uint64_t)count > ((uint64_t)length - 4) / 4)
        return 0;
    for (i = 0; i < count; ++i) {
        uint32_t item_length;
        if ((size_t)length - offset < 4)
            return 0;
        item_length = get_u32(payload + offset);
        offset += 4;
        if ((size_t)item_length > (size_t)length - offset)
            return 0;
        offset += item_length;
    }
    return offset == length;
}

static void
encode_header(uint8_t *header, const IpcMessage *message)
{
    put_u32(header, IPC_PROTOCOL_MAGIC);
    put_u16(header + 4, IPC_PROTOCOL_VERSION);
    put_u16(header + 6, (uint16_t)message->type);
    put_u32(header + 8, message->tab_id);
    put_u64(header + 12, message->request_id);
    put_u32(header + 20, message->payload_length);
}

void
ipc_message_dispose(IpcMessage *message)
{
    if (message == NULL)
        return;
    free(message->payload);
    memset(message, 0, sizeof(*message));
}

void
ipc_reader_init(IpcReader *reader)
{
    if (reader != NULL)
        memset(reader, 0, sizeof(*reader));
}

void
ipc_reader_reset(IpcReader *reader)
{
    if (reader == NULL)
        return;
    free(reader->payload);
    memset(reader, 0, sizeof(*reader));
}

static IpcIoResult
reader_fail(IpcReader *reader, int error_number)
{
    ipc_reader_reset(reader);
    errno = error_number;
    return IPC_IO_ERROR;
}

static int
reader_decode_header(IpcReader *reader)
{
    const uint8_t *header = reader->header;
    uint16_t type;

    if (get_u32(header) != IPC_PROTOCOL_MAGIC ||
        get_u16(header + 4) != IPC_PROTOCOL_VERSION) {
        errno = EPROTO;
        return -1;
    }
    type = get_u16(header + 6);
    if (!message_type_valid(type)) {
        errno = EPROTO;
        return -1;
    }
    reader->type = (IpcMessageType)type;
    reader->tab_id = get_u32(header + 8);
    reader->request_id = get_u64(header + 12);
    reader->payload_length = get_u32(header + 20);
    if (reader->payload_length > IPC_MAX_PAYLOAD_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }
    if (!header_payload_valid(reader->type, reader->payload_length)) {
        errno = EPROTO;
        return -1;
    }
    if (reader->payload_length != 0) {
        reader->payload = malloc(reader->payload_length);
        if (reader->payload == NULL)
            return -1;
    }
    return 0;
}

IpcIoResult
ipc_reader_read(IpcReader *reader, int fd, IpcMessage *out)
{
    if (reader == NULL || out == NULL || fd < 0) {
        errno = EINVAL;
        return IPC_IO_ERROR;
    }
    for (;;) {
        uint8_t *destination;
        size_t remaining;
        ssize_t amount;

        if (reader->header_used < IPC_HEADER_SIZE) {
            destination = reader->header + reader->header_used;
            remaining = IPC_HEADER_SIZE - reader->header_used;
        } else {
            if (reader->payload_used == reader->payload_length) {
                if (!payload_valid(reader->type, reader->payload,
                                   reader->payload_length))
                    return reader_fail(reader, EPROTO);
                out->type = reader->type;
                out->tab_id = reader->tab_id;
                out->request_id = reader->request_id;
                out->payload_length = reader->payload_length;
                out->payload = reader->payload;
                reader->payload = NULL;
                reader->header_used = 0;
                reader->payload_length = 0;
                reader->payload_used = 0;
                reader->type = 0;
                reader->tab_id = 0;
                reader->request_id = 0;
                return IPC_IO_MESSAGE;
            }
            destination = reader->payload + reader->payload_used;
            remaining = (size_t)reader->payload_length - reader->payload_used;
        }

        amount = read(fd, destination, remaining);
        if (amount > 0) {
            if (reader->header_used < IPC_HEADER_SIZE) {
                reader->header_used += (size_t)amount;
                if (reader->header_used == IPC_HEADER_SIZE &&
                    reader_decode_header(reader) < 0) {
                    int saved = errno;
                    return reader_fail(reader, saved);
                }
            } else {
                reader->payload_used += (size_t)amount;
            }
            continue;
        }
        if (amount == 0) {
            int partial = reader->header_used != 0 || reader->payload_used != 0;
            ipc_reader_reset(reader);
            return partial ? IPC_IO_TRUNCATED : IPC_IO_EOF;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return IPC_IO_NEED_MORE;
        return IPC_IO_ERROR;
    }
}

void
ipc_writer_init(IpcWriter *writer)
{
    if (writer != NULL)
        memset(writer, 0, sizeof(*writer));
}

void
ipc_writer_reset(IpcWriter *writer)
{
    if (writer == NULL)
        return;
    free(writer->frame);
    memset(writer, 0, sizeof(*writer));
}

int
ipc_writer_queue(IpcWriter *writer, const IpcMessage *message)
{
    size_t frame_length;
    uint8_t *frame;

    if (writer == NULL || message == NULL ||
        (message->payload_length != 0 && message->payload == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (writer->frame != NULL) {
        errno = EBUSY;
        return -1;
    }
    if (message->payload_length > IPC_MAX_PAYLOAD_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }
    if ((unsigned)message->type > UINT16_MAX ||
        !message_type_valid((uint16_t)message->type) ||
        !payload_valid(message->type, message->payload, message->payload_length)) {
        errno = EPROTO;
        return -1;
    }
    frame_length = IPC_HEADER_SIZE + (size_t)message->payload_length;
    frame = malloc(frame_length);
    if (frame == NULL)
        return -1;
    encode_header(frame, message);
    if (message->payload_length != 0)
        memcpy(frame + IPC_HEADER_SIZE, message->payload, message->payload_length);
    writer->frame = frame;
    writer->frame_length = frame_length;
    writer->frame_offset = 0;
    return 0;
}

IpcIoResult
ipc_writer_flush(IpcWriter *writer, int fd)
{
    if (writer == NULL || fd < 0 || writer->frame == NULL) {
        errno = EINVAL;
        return IPC_IO_ERROR;
    }
    while (writer->frame_offset < writer->frame_length) {
        ssize_t amount = write(fd, writer->frame + writer->frame_offset,
                               writer->frame_length - writer->frame_offset);
        if (amount > 0) {
            writer->frame_offset += (size_t)amount;
            continue;
        }
        if (amount == 0) {
            errno = EIO;
            return IPC_IO_ERROR;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return IPC_IO_NEED_MORE;
        return IPC_IO_ERROR;
    }
    ipc_writer_reset(writer);
    return IPC_IO_WRITTEN;
}
