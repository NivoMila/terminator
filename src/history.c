#define _POSIX_C_SOURCE 200809L

#include "history.h"

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

enum { HISTORY_LIMIT = 10000 };

struct History {
    pthread_mutex_t mutex;
    char *path;
    char **items;
    size_t count;
};

static void
history_clear_items(History *history)
{
    size_t i;
    for (i = 0; i < history->count; ++i)
        free(history->items[i]);
    free(history->items);
    history->items = NULL;
    history->count = 0;
}

static int
history_append_memory(History *history, const char *text, size_t length)
{
    char **grown;
    char *copy;
    if (length == 0)
        return 0;
    copy = strndup(text, length);
    if (copy == NULL)
        return -1;
    if (history->count == HISTORY_LIMIT) {
        free(history->items[0]);
        memmove(history->items, history->items + 1,
                (HISTORY_LIMIT - 1) * sizeof(*history->items));
        --history->count;
    }
    grown = realloc(history->items, (history->count + 1) * sizeof(*grown));
    if (grown == NULL) {
        free(copy);
        return -1;
    }
    history->items = grown;
    history->items[history->count++] = copy;
    return 0;
}

static int
history_read_locked(History *history, int fd)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int duplicate = dup(fd);
    if (duplicate < 0)
        return -1;
    if (lseek(duplicate, 0, SEEK_SET) < 0) {
        int saved = errno;
        close(duplicate);
        errno = saved;
        return -1;
    }
    file = fdopen(duplicate, "r");
    if (file == NULL) {
        int saved = errno;
        close(duplicate);
        errno = saved;
        return -1;
    }
    history_clear_items(history);
    while ((length = getline(&line, &capacity, file)) >= 0) {
        if (length == 0 || line[length - 1] != '\n')
            continue; /* Ignore a torn final record. */
        if (history_append_memory(history, line, (size_t)length - 1) < 0) {
            free(line);
            fclose(file);
            return -1;
        }
    }
    free(line);
    if (ferror(file)) {
        int saved = errno == 0 ? EIO : errno;
        fclose(file);
        errno = saved;
        return -1;
    }
    return fclose(file);
}

static int
write_all(int fd, const char *bytes, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t amount = write(fd, bytes + written, length - written);
        if (amount > 0)
            written += (size_t)amount;
        else if (amount < 0 && errno == EINTR)
            continue;
        else
            return -1;
    }
    return 0;
}

static int
history_persist_locked(History *history, int fd)
{
    size_t i;
    if (ftruncate(fd, 0) < 0 || lseek(fd, 0, SEEK_SET) < 0)
        return -1;
    for (i = 0; i < history->count; ++i) {
        size_t length = strlen(history->items[i]);
        if (write_all(fd, history->items[i], length) < 0 ||
            write_all(fd, "\n", 1) < 0)
            return -1;
    }
    return fsync(fd);
}

static void
history_refresh_locked(History *history)
{
    int fd = open(history->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    if (flock(fd, LOCK_SH) == 0) {
        (void)history_read_locked(history, fd);
        (void)flock(fd, LOCK_UN);
    }
    (void)close(fd);
}

History *
history_open(const char *path)
{
    const char *home = getenv("HOME");
    History *history = calloc(1, sizeof(*history));
    int fd;
    if (history == NULL)
        return NULL;
    if (path == NULL) {
        if (home == NULL || *home == '\0') {
            free(history);
            errno = ENOENT;
            return NULL;
        }
        size_t length = strlen(home);
        history->path = malloc(length + sizeof("/.terminator_history"));
        if (history->path != NULL)
            (void)snprintf(history->path, length + sizeof("/.terminator_history"),
                           "%s/.terminator_history", home);
    } else {
        history->path = strdup(path);
    }
    if (history->path == NULL || pthread_mutex_init(&history->mutex, NULL) != 0) {
        free(history->path);
        free(history);
        return NULL;
    }
    fd = open(history->path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        goto fail;
    if (flock(fd, LOCK_EX) < 0 || history_read_locked(history, fd) < 0) {
        int saved = errno;
        (void)flock(fd, LOCK_UN);
        (void)close(fd);
        errno = saved;
        goto fail;
    }
    (void)flock(fd, LOCK_UN);
    (void)close(fd);
    return history;

fail:
    history_clear_items(history);
    (void)pthread_mutex_destroy(&history->mutex);
    free(history->path);
    free(history);
    return NULL;
}

void
history_close(History *history)
{
    if (history == NULL)
        return;
    history_clear_items(history);
    free(history->path);
    (void)pthread_mutex_destroy(&history->mutex);
    free(history);
}

size_t
history_count(History *history)
{
    size_t count;
    (void)pthread_mutex_lock(&history->mutex);
    history_refresh_locked(history);
    count = history->count;
    (void)pthread_mutex_unlock(&history->mutex);
    return count;
}

char *
history_get(History *history, size_t index)
{
    char *copy = NULL;
    (void)pthread_mutex_lock(&history->mutex);
    history_refresh_locked(history);
    if (index < history->count)
        copy = strdup(history->items[index]);
    (void)pthread_mutex_unlock(&history->mutex);
    return copy;
}

int
history_add(History *history, const char *command)
{
    int fd;
    int result = -1;
    if (command == NULL || command[0] == '\0')
        return 0;
    const unsigned char *scan = (const unsigned char *)command;
    while (*scan != '\0' && isspace(*scan))
        ++scan;
    if (*scan == '\0')
        return 0;
    if (strchr(command, '\n') != NULL) {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&history->mutex);
    fd = open(history->path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        goto done;
    if (flock(fd, LOCK_EX) < 0)
        goto close_done;
    if (history_read_locked(history, fd) < 0)
        goto unlock_done;
    if (history_append_memory(history, command, strlen(command)) < 0)
        goto unlock_done;
    result = history_persist_locked(history, fd);

unlock_done:
    (void)flock(fd, LOCK_UN);
close_done:
    (void)close(fd);
done:
    (void)pthread_mutex_unlock(&history->mutex);
    return result;
}

void
history_cursor_reset(HistoryCursor *cursor)
{
    free(cursor->draft);
    cursor->draft = NULL;
    cursor->index = 0;
    cursor->active = 0;
}

char *
history_cursor_previous(History *history, HistoryCursor *cursor,
                        const char *current_input)
{
    size_t count = history_count(history);
    if (count == 0)
        return NULL;
    if (!cursor->active) {
        cursor->draft = strdup(current_input);
        if (cursor->draft == NULL)
            return NULL;
        cursor->index = count;
        cursor->active = 1;
    }
    if (cursor->index == 0)
        return NULL;
    return history_get(history, --cursor->index);
}

char *
history_cursor_next(History *history, HistoryCursor *cursor)
{
    size_t count;
    if (!cursor->active)
        return NULL;
    count = history_count(history);
    if (cursor->index + 1 < count)
        return history_get(history, ++cursor->index);
    char *draft = strdup(cursor->draft == NULL ? "" : cursor->draft);
    history_cursor_reset(cursor);
    return draft;
}
