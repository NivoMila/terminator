#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "history.h"

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum { HISTORY_LIMIT = 10000 };
#ifndef HISTORY_SHARED_ARENA_SIZE
#define HISTORY_SHARED_ARENA_SIZE (16u * 1024u * 1024u)
#endif
#ifndef HISTORY_SHARED_RECORD_LIMIT
#define HISTORY_SHARED_RECORD_LIMIT (1024u * 1024u)
#endif

#define SHARED_HISTORY_MAGIC UINT64_C(0x5445524d48495354)
#define SHARED_HISTORY_VERSION 2u
static const char MULTILINE_HISTORY_MARKER[] = "\036M";

typedef struct {
    uint64_t offset;
    uint32_t length;
    uint32_t reserved;
} SharedHistoryEntry;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t header_size;
    uint64_t mapping_size;
    pthread_mutex_t mutex;
    sem_t semaphore;
    uint32_t count;
    uint32_t reserved;
    uint64_t arena_used;
    uint64_t generation;
    uint64_t file_dev;
    uint64_t file_ino;
    uint64_t file_size;
    int64_t file_mtime_sec;
    int64_t file_ctime_sec;
    uint32_t file_mtime_nsec;
    uint32_t file_ctime_nsec;
    SharedHistoryEntry entries[HISTORY_LIMIT];
    unsigned char arena[];
} SharedHistory;

struct History {
    pthread_mutex_t mutex;
    int mutex_initialized;
    char *path;
    char *lock_path;
    int lock_fd;
    char **items;
    size_t count;
    int shared_backend;
    char shm_name[96];
    int shm_fd;
    SharedHistory *shared;
    size_t shared_size;
#ifdef TERMINATOR_HISTORY_TESTING
    int test_failpoint;
#endif
};

enum {
    TEST_FAIL_NONE = 0,
    TEST_FAIL_WRITE = 1,
    TEST_FAIL_FSYNC = 2,
    TEST_FAIL_RENAME = 3,
    TEST_CRASH_AFTER_STAGE_WRITE = 4,
    TEST_CRASH_AFTER_STAGE_FSYNC = 5,
    TEST_CRASH_AFTER_RENAME = 6,
    TEST_CRASH_DURING_SHARED_SYNC = 7,
    TEST_CRASH_AFTER_DIR_FSYNC = 8,
    TEST_FAIL_DIR_FSYNC = 9,
    TEST_CRASH_AFTER_MUTEX_BEFORE_SEM = 10,
    TEST_CRASH_AFTER_SEM_ACQUIRE = 11,
    TEST_CRASH_DURING_MUTATION = 12,
    TEST_CRASH_AFTER_SEM_RELEASE = 13,
    TEST_CRASH_DURING_SEM_RECOVERY = 14,
    TEST_CRASH_DURING_INDEX_RECOVERY = 15,
    TEST_CRASH_DURING_SEM_RESTORE = 16,
    TEST_FORCE_RETIREMENT = 17,
    TEST_FORCE_RETIREMENT_UNLINK_FAILURE = 18
};

static _Atomic uint64_t temp_sequence;
#ifdef TERMINATOR_HISTORY_TESTING
static int next_open_failpoint;
#endif

static size_t shared_header_size(void)
{
    return offsetof(SharedHistory, arena);
}

static size_t shared_mapping_size(void)
{
    return shared_header_size() + (size_t)HISTORY_SHARED_ARENA_SIZE;
}

static void history_clear_items(History *history)
{
    for (size_t i = 0; i < history->count; ++i)
        free(history->items[i]);
    free(history->items);
    history->items = NULL;
    history->count = 0;
}

static void history_replace_items(History *destination, History *source)
{
    history_clear_items(destination);
    destination->items = source->items;
    destination->count = source->count;
    source->items = NULL;
    source->count = 0;
}

static int history_append_memory(History *history, const char *text, size_t length)
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
    grown = realloc(history->items, (history->count + 1) * sizeof(*history->items));
    if (grown == NULL) {
        free(copy);
        return -1;
    }
    history->items = grown;
    history->items[history->count++] = copy;
    return 0;
}

static int decode_multiline_record(const char *record, size_t length,
                                   char **decoded, size_t *decoded_length)
{
    size_t prefix = sizeof(MULTILINE_HISTORY_MARKER) - 1;
    if (length < prefix || memcmp(record, MULTILINE_HISTORY_MARKER, prefix) != 0)
        return 0;
    if ((length - prefix) % 2 != 0) {
        errno = EILSEQ;
        return -1;
    }
    size_t bytes = (length - prefix) / 2;
    char *text = malloc(bytes + 1);
    if (text == NULL)
        return -1;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned char hi = (unsigned char)record[prefix + i * 2];
        unsigned char lo = (unsigned char)record[prefix + i * 2 + 1];
        int high = hi >= '0' && hi <= '9' ? hi - '0' :
                   hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : -1;
        int low = lo >= '0' && lo <= '9' ? lo - '0' :
                  lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : -1;
        if (high < 0 || low < 0) {
            free(text);
            errno = EILSEQ;
            return -1;
        }
        text[i] = (char)((high << 4) | low);
        if (text[i] == '\0') {
            free(text);
            errno = EILSEQ;
            return -1;
        }
    }
    text[bytes] = '\0';
    *decoded = text;
    *decoded_length = bytes;
    return 1;
}

static int history_read_fd(History *history, int fd)
{
    History loaded = {0};
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
    while ((length = getline(&line, &capacity, file)) >= 0) {
        char *decoded = NULL;
        size_t decoded_length = 0;
        int decoded_record;
        if (length == 0 || line[length - 1] != '\n' ||
            memchr(line, '\0', (size_t)length - 1) != NULL) {
            free(line);
            fclose(file);
            history_clear_items(&loaded);
            errno = EILSEQ;
            return -1;
        }
        decoded_record = decode_multiline_record(line, (size_t)length - 1,
                                                &decoded, &decoded_length);
        int append_result;
        if (decoded_record < 0) {
            int saved = errno == 0 ? EILSEQ : errno;
            free(decoded);
            free(line);
            fclose(file);
            history_clear_items(&loaded);
            errno = saved;
            return -1;
        }
        append_result = decoded_record == 1 ?
            history_append_memory(&loaded, decoded, decoded_length) :
            history_append_memory(&loaded, line, (size_t)length - 1);
        free(decoded);
        if (append_result < 0) {
            int saved = errno;
            free(line);
            fclose(file);
            history_clear_items(&loaded);
            errno = saved;
            return -1;
        }
    }
    free(line);
    if (ferror(file)) {
        int saved = errno == 0 ? EIO : errno;
        fclose(file);
        history_clear_items(&loaded);
        errno = saved;
        return -1;
    }
    if (fclose(file) < 0) {
        history_clear_items(&loaded);
        return -1;
    }
    history_replace_items(history, &loaded);
    return 0;
}

static int write_all(int fd, const char *bytes, size_t length)
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

static int history_write_records(History *history, int fd)
{
    for (size_t i = 0; i < history->count; ++i) {
        size_t length = strlen(history->items[i]);
        if (strchr(history->items[i], '\n') != NULL ||
            strncmp(history->items[i], MULTILINE_HISTORY_MARKER,
                    sizeof(MULTILINE_HISTORY_MARKER) - 1) == 0) {
            static const char digits[] = "0123456789ABCDEF";
            if (write_all(fd, MULTILINE_HISTORY_MARKER,
                          sizeof(MULTILINE_HISTORY_MARKER) - 1) < 0)
                return -1;
            for (size_t j = 0; j < length; ++j) {
                char pair[2] = {digits[(unsigned char)history->items[i][j] >> 4],
                                digits[(unsigned char)history->items[i][j] & 0x0f]};
                if (write_all(fd, pair, sizeof(pair)) < 0)
                    return -1;
            }
            if (write_all(fd, "\n", 1) < 0)
                return -1;
        } else if (write_all(fd, history->items[i], length) < 0 ||
                   write_all(fd, "\n", 1) < 0) {
            return -1;
        }
    }
    return 0;
}

static int history_parent_dir(const char *path, char **directory)
{
    char *copy = strdup(path);
    if (copy == NULL)
        return -1;
    char *slash = strrchr(copy, '/');
    if (slash == NULL) {
        free(copy);
        *directory = strdup(".");
    } else if (slash == copy) {
        slash[1] = '\0';
        *directory = copy;
    } else {
        *slash = '\0';
        *directory = copy;
    }
    return *directory == NULL ? -1 : 0;
}

static int history_cleanup_staged_files(History *history)
{
    char *directory = NULL;
    const char *base = strrchr(history->path, '/');
    size_t prefix_length;
    DIR *dir;
    struct dirent *entry;
    if (history_parent_dir(history->path, &directory) < 0)
        return -1;
    base = base == NULL ? history->path : base + 1;
    prefix_length = strlen(base);
    dir = opendir(directory);
    free(directory);
    if (dir == NULL)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, base, prefix_length) == 0 &&
            strncmp(entry->d_name + prefix_length, ".tmp.", 5) == 0) {
            char *full = malloc(strlen(history->path) +
                                strlen(entry->d_name + prefix_length) + 1);
            if (full == NULL) {
                closedir(dir);
                return -1;
            }
            (void)snprintf(full, strlen(history->path) +
                           strlen(entry->d_name + prefix_length) + 1,
                           "%s%s", history->path,
                           entry->d_name + prefix_length);
            if (unlink(full) < 0 && errno != ENOENT) {
                int saved = errno;
                free(full);
                closedir(dir);
                errno = saved;
                return -1;
            }
            free(full);
        }
    }
    int saved = errno;
    closedir(dir);
    errno = saved;
    return 0;
}

static int test_failpoint(History *history, int point)
{
#ifdef TERMINATOR_HISTORY_TESTING
    if (history->test_failpoint == point) {
        errno = EIO;
        return 1;
    }
#else
    (void)history;
    (void)point;
#endif
    return 0;
}

static void test_crashpoint(History *history, int point)
{
#ifdef TERMINATOR_HISTORY_TESTING
    if (history->test_failpoint == point)
        _exit(80 + point);
#else
    (void)history;
    (void)point;
#endif
}

static int history_persist_atomic(History *history, int *renamed)
{
    char *temporary = NULL, *directory = NULL;
    size_t path_length = strlen(history->path);
    int fd = -1, dir_fd = -1, result = -1;
    *renamed = 0;
    temporary = malloc(path_length + 80);
    if (temporary == NULL)
        return -1;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        uint64_t sequence = atomic_fetch_add(&temp_sequence, 1);
        (void)snprintf(temporary, path_length + 80, "%s.tmp.%ld.%llu",
                       history->path, (long)getpid(),
                       (unsigned long long)sequence);
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    if (fd < 0)
        goto done;
    if (test_failpoint(history, TEST_FAIL_WRITE)) {
        (void)write_all(fd, "partial", 7);
        errno = EIO;
        goto close_temp;
    }
    if (history_write_records(history, fd) < 0)
        goto close_temp;
    test_crashpoint(history, TEST_CRASH_AFTER_STAGE_WRITE);
    if (fsync(fd) < 0 || test_failpoint(history, TEST_FAIL_FSYNC))
        goto close_temp;
    test_crashpoint(history, TEST_CRASH_AFTER_STAGE_FSYNC);
    if (close(fd) < 0) {
        fd = -1;
        goto done;
    }
    fd = -1;
    if (test_failpoint(history, TEST_FAIL_RENAME) ||
        rename(temporary, history->path) < 0)
        goto done;
    *renamed = 1;
    test_crashpoint(history, TEST_CRASH_AFTER_RENAME);
    if (history_parent_dir(history->path, &directory) < 0)
        goto done;
    dir_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0 || fsync(dir_fd) < 0 ||
        test_failpoint(history, TEST_FAIL_DIR_FSYNC))
        goto done;
    test_crashpoint(history, TEST_CRASH_AFTER_DIR_FSYNC);
    result = 0;
    goto done;

close_temp:
    {
        int saved = errno == 0 ? EIO : errno;
        (void)close(fd);
        fd = -1;
        errno = saved;
    }
done:
    {
        int saved = errno;
        if (fd >= 0)
            (void)close(fd);
        if (dir_fd >= 0)
            (void)close(dir_fd);
        if (!*renamed && temporary != NULL)
            (void)unlink(temporary);
        free(directory);
        free(temporary);
        errno = saved;
    }
    return result;
}

static int lock_file(History *history)
{
    int result;
#ifdef TERMINATOR_HISTORY_TESTING
    const char *notify_text = getenv("TERMINATOR_HISTORY_TEST_LOCK_NOTIFY_FD");
    if (notify_text != NULL) {
        char *end = NULL;
        long notify_fd = strtol(notify_text, &end, 10);
        if (end != notify_text && *end == '\0' && notify_fd >= 0 &&
            notify_fd <= INT_MAX) {
            int probe = flock(history->lock_fd, LOCK_EX | LOCK_NB);
            if (probe < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                const char byte = 'b';
                ssize_t written;
                do { written = write((int)notify_fd, &byte, 1); }
                while (written < 0 && errno == EINTR);
            } else if (probe == 0) {
                (void)flock(history->lock_fd, LOCK_UN);
            }
        }
    }
#endif
    do { result = flock(history->lock_fd, LOCK_EX); }
    while (result < 0 && errno == EINTR);
    return result;
}

static int shared_segment_unlink(History *history)
{
#ifdef TERMINATOR_HISTORY_TESTING
    if (history->test_failpoint == TEST_FORCE_RETIREMENT_UNLINK_FAILURE) {
        errno = EACCES;
        return -1;
    }
#endif
    int result;
    do { result = shm_unlink(history->shm_name); }
    while (result < 0 && errno == EINTR);
    return result;
}

static void shared_retire_name_or_fail_stop(History *history)
{
    if (shared_segment_unlink(history) < 0 && errno != ENOENT)
        _exit(125);
}

static int shared_header_valid(const History *history)
{
    const SharedHistory *shared = history->shared;
    uint64_t expected = 0;
    if (shared == NULL || shared->magic != SHARED_HISTORY_MAGIC ||
        shared->version != SHARED_HISTORY_VERSION ||
        shared->header_size != shared_header_size() ||
        shared->mapping_size != history->shared_size ||
        shared->count > HISTORY_LIMIT ||
        shared->reserved != 0 ||
        shared->arena_used > HISTORY_SHARED_ARENA_SIZE)
        return 0;
    for (uint32_t i = 0; i < shared->count; ++i) {
        const SharedHistoryEntry *entry = &shared->entries[i];
        if (entry->reserved != 0 || entry->offset != expected ||
            entry->length > HISTORY_SHARED_RECORD_LIMIT ||
            (uint64_t)entry->length + 1 > HISTORY_SHARED_ARENA_SIZE - expected)
            return 0;
        const unsigned char *record = shared->arena + entry->offset;
        if (record[entry->length] != '\0' ||
            memchr(record, '\0', entry->length) != NULL)
            return 0;
        expected += (uint64_t)entry->length + 1;
    }
    return expected == shared->arena_used;
}

static int shared_header_compatible(const History *history)
{
    const SharedHistory *shared = history->shared;
    return shared != NULL && shared->magic == SHARED_HISTORY_MAGIC &&
           shared->version == SHARED_HISTORY_VERSION &&
           shared->header_size == shared_header_size() &&
           shared->mapping_size == history->shared_size;
}

static int shared_fingerprint_matches(const SharedHistory *shared,
                                      const struct stat *st)
{
    return shared->file_dev == (uint64_t)st->st_dev &&
           shared->file_ino == (uint64_t)st->st_ino &&
           shared->file_size == (uint64_t)st->st_size &&
           shared->file_mtime_sec == st->st_mtim.tv_sec &&
           shared->file_mtime_nsec == (uint32_t)st->st_mtim.tv_nsec &&
           shared->file_ctime_sec == st->st_ctim.tv_sec &&
           shared->file_ctime_nsec == (uint32_t)st->st_ctim.tv_nsec;
}

static void shared_set_fingerprint(SharedHistory *shared, const struct stat *st)
{
    shared->file_dev = (uint64_t)st->st_dev;
    shared->file_ino = (uint64_t)st->st_ino;
    shared->file_size = (uint64_t)st->st_size;
    shared->file_mtime_sec = st->st_mtim.tv_sec;
    shared->file_mtime_nsec = (uint32_t)st->st_mtim.tv_nsec;
    shared->file_ctime_sec = st->st_ctim.tv_sec;
    shared->file_ctime_nsec = (uint32_t)st->st_ctim.tv_nsec;
}

static int history_fits_shared(const History *history)
{
    uint64_t total = 0;
    for (size_t i = 0; i < history->count; ++i) {
        size_t length = strlen(history->items[i]);
        if (length > HISTORY_SHARED_RECORD_LIMIT ||
            length >= HISTORY_SHARED_ARENA_SIZE ||
            total > HISTORY_SHARED_ARENA_SIZE - (length + 1)) {
            errno = ENOSPC;
            return 0;
        }
        total += length + 1;
    }
    return 1;
}

static void shared_copy_from_history(History *history, const struct stat *st)
{
    SharedHistory *shared = history->shared;
    uint64_t offset = 0;
    shared->count = 0;
    shared->arena_used = 0;
    for (size_t i = 0; i < history->count; ++i) {
        size_t length = strlen(history->items[i]);
        SharedHistoryEntry *entry = &shared->entries[i];
        entry->offset = offset;
        entry->length = (uint32_t)length;
        entry->reserved = 0;
        memcpy(shared->arena + offset, history->items[i], length + 1);
        offset += length + 1;
        shared->count = (uint32_t)(i + 1);
        shared->arena_used = offset;
#ifdef TERMINATOR_HISTORY_TESTING
        if (history->test_failpoint == TEST_CRASH_DURING_SHARED_SYNC && i == 0)
            _exit(80 + TEST_CRASH_DURING_SHARED_SYNC);
#endif
    }
    if (st != NULL)
        shared_set_fingerprint(shared, st);
    ++shared->generation;
}

static int shared_copy_to_history(History *history)
{
    History snapshot = {0};
    SharedHistory *shared = history->shared;
    if (!shared_header_valid(history)) {
        errno = EPROTO;
        return -1;
    }
    for (uint32_t i = 0; i < shared->count; ++i) {
        SharedHistoryEntry *entry = &shared->entries[i];
        if (history_append_memory(&snapshot,
                (const char *)shared->arena + entry->offset, entry->length) < 0) {
            history_clear_items(&snapshot);
            return -1;
        }
    }
    history_replace_items(history, &snapshot);
    return 0;
}

static int history_load_file(History *history, struct stat *st)
{
    int fd = open(history->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT)
        fd = open(history->path, O_RDONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (fstat(fd, st) < 0 || history_read_fd(history, fd) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return close(fd);
}

static uint64_t hash_path(const char *path)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int shared_map_current_locked(History *history)
{
    struct stat current, named;
    int named_fd = shm_open(history->shm_name, O_RDWR | O_CREAT, 0600);
    if (named_fd < 0)
        return -1;
    if (fstat(named_fd, &named) < 0) {
        int saved = errno;
        close(named_fd);
        errno = saved;
        return -1;
    }
    int replace = history->shm_fd < 0;
    if (!replace && fstat(history->shm_fd, &current) == 0)
        replace = current.st_dev != named.st_dev || current.st_ino != named.st_ino;
    if (replace) {
        if (history->shared != NULL)
            (void)munmap(history->shared, history->shared_size);
        if (history->shm_fd >= 0)
            (void)close(history->shm_fd);
        history->shared = NULL;
        history->shm_fd = named_fd;
        named_fd = -1;
    }
    if (named_fd >= 0)
        close(named_fd);
    if (fstat(history->shm_fd, &current) < 0)
        return -1;
    if ((size_t)current.st_size != history->shared_size) {
        errno = EPROTO;
        return -1;
    }
    if (history->shared == NULL) {
        void *mapping = mmap(NULL, history->shared_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, history->shm_fd, 0);
        if (mapping == MAP_FAILED)
            return -1;
        history->shared = mapping;
    }
    return 0;
}

static int shared_create_new_locked(History *history, const struct stat *st)
{
    pthread_mutexattr_t attributes;
    int mutex_locked = 0;
    if (!history_fits_shared(history))
        return -1;
    if (shared_segment_unlink(history) < 0 && errno != ENOENT)
        return -1;
    if (history->shared != NULL) {
        (void)munmap(history->shared, history->shared_size);
        history->shared = NULL;
    }
    if (history->shm_fd >= 0) {
        (void)close(history->shm_fd);
        history->shm_fd = -1;
    }
    history->shm_fd = shm_open(history->shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (history->shm_fd < 0 ||
        ftruncate(history->shm_fd, (off_t)history->shared_size) < 0)
        return -1;
    void *mapping = mmap(NULL, history->shared_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, history->shm_fd, 0);
    if (mapping == MAP_FAILED) {
        history->shared = NULL;
        return -1;
    }
    history->shared = mapping;
    memset(history->shared, 0, shared_header_size());
    int error = pthread_mutexattr_init(&attributes);
    if (error == 0)
        error = pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
    if (error == 0)
        error = pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);
    if (error != 0) {
        errno = error;
        return -1;
    }
    error = pthread_mutex_init(&history->shared->mutex, &attributes);
    (void)pthread_mutexattr_destroy(&attributes);
    if (error != 0) {
        errno = error;
        return -1;
    }
    error = pthread_mutex_lock(&history->shared->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    mutex_locked = 1;
    if (sem_init(&history->shared->semaphore, 1, 1) < 0)
        goto init_fail;
    history->shared->version = SHARED_HISTORY_VERSION;
    history->shared->header_size = (uint32_t)shared_header_size();
    history->shared->mapping_size = history->shared_size;
    shared_copy_from_history(history, st);
    history->shared->magic = SHARED_HISTORY_MAGIC;
    error = pthread_mutex_unlock(&history->shared->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;

init_fail:
    {
        int saved = errno;
        if (mutex_locked)
            (void)pthread_mutex_unlock(&history->shared->mutex);
        errno = saved;
        return -1;
    }
}

static int shared_rebuild_locked(History *history, int replace_segment)
{
    struct stat st;
    if (history_load_file(history, &st) < 0)
        return -1;
    if (!history_fits_shared(history))
        return -1;
    if (replace_segment)
        return shared_create_new_locked(history, &st);
    shared_copy_from_history(history, &st);
    return 0;
}

/* Called only while this process owns both the stable file lock and the
 * robust process-shared mutex. The invariant forbids semaphore waiters here. */
static int shared_semaphore_normalize_locked(History *history)
{
#ifdef TERMINATOR_HISTORY_TESTING
    if (history->test_failpoint == TEST_FORCE_RETIREMENT ||
        history->test_failpoint == TEST_FORCE_RETIREMENT_UNLINK_FAILURE) {
        errno = EINVAL;
        return -1;
    }
#endif
    for (;;) {
        if (sem_trywait(&history->shared->semaphore) == 0)
            continue;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN)
            return -1;
        break;
    }
#ifdef TERMINATOR_HISTORY_TESTING
    test_crashpoint(history, TEST_CRASH_DURING_SEM_RESTORE);
#endif
    return sem_post(&history->shared->semaphore);
}

static int shared_semaphore_wait_locked(History *history)
{
#ifdef TERMINATOR_HISTORY_TESTING
    test_crashpoint(history, TEST_CRASH_AFTER_MUTEX_BEFORE_SEM);
#endif
    while (sem_wait(&history->shared->semaphore) < 0) {
        if (errno == EINTR)
            continue;
        return -1;
    }
#ifdef TERMINATOR_HISTORY_TESTING
    test_crashpoint(history, TEST_CRASH_AFTER_SEM_ACQUIRE);
#endif
    return 0;
}

static int shared_end_locked(History *history);

static int shared_begin_locked(History *history)
{
    int lock_result;
    int sem_locked = 0;
    struct stat st;
    if (lock_file(history) < 0)
        return -1;
    if (shared_map_current_locked(history) < 0) {
        if (errno != EPROTO || shared_rebuild_locked(history, 1) < 0)
            goto fail_file_lock;
    }
    if (!shared_header_compatible(history)) {
        if (shared_rebuild_locked(history, 1) < 0)
            goto fail_file_lock;
    }
    lock_result = pthread_mutex_lock(&history->shared->mutex);
    if (lock_result == EOWNERDEAD) {
        if (shared_semaphore_normalize_locked(history) < 0) {
            int saved = errno;
            (void)pthread_mutex_unlock(&history->shared->mutex);
            shared_retire_name_or_fail_stop(history);
            errno = saved;
            goto fail_file_lock;
        }
#ifdef TERMINATOR_HISTORY_TESTING
        test_crashpoint(history, TEST_CRASH_DURING_SEM_RECOVERY);
#endif
        if (shared_rebuild_locked(history, 0) < 0) {
            (void)pthread_mutex_unlock(&history->shared->mutex);
            goto fail_file_lock;
        }
#ifdef TERMINATOR_HISTORY_TESTING
        test_crashpoint(history, TEST_CRASH_DURING_INDEX_RECOVERY);
#endif
        lock_result = pthread_mutex_consistent(&history->shared->mutex);
        if (lock_result != 0) {
            (void)pthread_mutex_unlock(&history->shared->mutex);
            errno = lock_result;
            goto fail_file_lock;
        }
        if (shared_semaphore_wait_locked(history) < 0) {
            int saved = errno;
            (void)pthread_mutex_unlock(&history->shared->mutex);
            (void)shared_rebuild_locked(history, 1);
            errno = saved;
            goto fail_file_lock;
        }
        sem_locked = 1;
    } else if (lock_result == ENOTRECOVERABLE) {
        if (shared_rebuild_locked(history, 1) < 0)
            goto fail_file_lock;
        lock_result = pthread_mutex_lock(&history->shared->mutex);
        if (lock_result != 0) {
            errno = lock_result;
            goto fail_file_lock;
        }
        if (shared_semaphore_wait_locked(history) < 0) {
            int saved = errno;
            (void)pthread_mutex_unlock(&history->shared->mutex);
            (void)shared_rebuild_locked(history, 1);
            errno = saved;
            goto fail_file_lock;
        }
        sem_locked = 1;
    } else if (lock_result != 0) {
        errno = lock_result;
        goto fail_file_lock;
    }

    if (!sem_locked) {
        if (shared_semaphore_wait_locked(history) < 0) {
            int saved = errno;
            (void)pthread_mutex_unlock(&history->shared->mutex);
            (void)shared_rebuild_locked(history, 1);
            errno = saved;
            goto fail_file_lock;
        }
        sem_locked = 1;
    }

    if (!shared_header_valid(history)) {
        if (shared_rebuild_locked(history, 0) < 0)
            goto fail_shared_lock;
    } else if (stat(history->path, &st) < 0) {
        if (errno != ENOENT || history_load_file(history, &st) < 0)
            goto fail_shared_lock;
        if (!history_fits_shared(history))
            goto fail_shared_lock;
        shared_copy_from_history(history, &st);
    } else if (!shared_fingerprint_matches(history->shared, &st)) {
        if (history_load_file(history, &st) < 0 || !history_fits_shared(history))
            goto fail_shared_lock;
        shared_copy_from_history(history, &st);
    }
    if (shared_copy_to_history(history) < 0)
        goto fail_shared_lock;
    return 0;

fail_shared_lock:
    (void)shared_end_locked(history);
    return -1;
fail_file_lock:
    (void)flock(history->lock_fd, LOCK_UN);
    return -1;
}

static int shared_end_locked(History *history)
{
    int result = 0;
    int saved = 0;
    int normalize_result;
#ifdef TERMINATOR_HISTORY_TESTING
    if (history->test_failpoint == TEST_FORCE_RETIREMENT_UNLINK_FAILURE)
        (void)raise(SIGSTOP);
#endif
    int post_result;
#ifdef TERMINATOR_HISTORY_TESTING
    if (history->test_failpoint == TEST_FORCE_RETIREMENT ||
        history->test_failpoint == TEST_FORCE_RETIREMENT_UNLINK_FAILURE) {
        errno = EINVAL;
        post_result = -1;
    } else
#endif
        post_result = sem_post(&history->shared->semaphore);
    if (post_result < 0) {
        saved = errno;
        result = -1;
#ifdef TERMINATOR_HISTORY_TESTING
        if (history->test_failpoint == TEST_FORCE_RETIREMENT ||
            history->test_failpoint == TEST_FORCE_RETIREMENT_UNLINK_FAILURE) {
            errno = EINVAL;
            normalize_result = -1;
        } else
#endif
            normalize_result = shared_semaphore_normalize_locked(history);
        if (normalize_result < 0) {
            /* Retire the damaged segment without unlocking its semaphore. No
             * participant can access it until this flock is released. If the
             * name cannot be removed, terminate while still owning the robust
             * mutex and flock so the next process observes owner death. */
            shared_retire_name_or_fail_stop(history);
            goto unlock_file;
        }
    }
#ifdef TERMINATOR_HISTORY_TESTING
    test_crashpoint(history, TEST_CRASH_AFTER_SEM_RELEASE);
#endif
    {
        int error = pthread_mutex_unlock(&history->shared->mutex);
        if (error != 0 && result == 0) {
            result = -1;
            saved = error;
        }
    }
unlock_file:
    (void)flock(history->lock_fd, LOCK_UN);
    if (result < 0)
        errno = saved;
    return result;
}

/* Caller owns History.mutex. Shared mode snapshots file changes while holding
 * the stable flock and robust mutex; the local array remains private. */
static int history_refresh_locked(History *history)
{
    if (history->shared_backend) {
        if (shared_begin_locked(history) < 0)
            return -1;
        return shared_end_locked(history);
    }
    if (lock_file(history) < 0)
        return -1;
    int fd = open(history->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT)
        fd = open(history->path, O_RDONLY | O_CREAT | O_CLOEXEC, 0600);
    int result = fd < 0 ? -1 : history_read_fd(history, fd);
    int saved = errno;
    if (fd >= 0)
        close(fd);
    (void)flock(history->lock_fd, LOCK_UN);
    errno = saved;
    return result;
}

static int history_path_setup(History *history, const char *path)
{
    const char *home = getenv("HOME");
    if (path == NULL) {
        if (home == NULL || *home == '\0') {
            errno = ENOENT;
            return -1;
        }
        size_t length = strlen(home);
        history->path = malloc(length + sizeof("/.terminator_history"));
        if (history->path != NULL)
            (void)snprintf(history->path, length + sizeof("/.terminator_history"),
                           "%s/.terminator_history", home);
    } else {
        history->path = strdup(path);
    }
    if (history->path == NULL)
        return -1;
    size_t length = strlen(history->path);
    history->lock_path = malloc(length + sizeof(".lock"));
    if (history->lock_path == NULL)
        return -1;
    (void)snprintf(history->lock_path, length + sizeof(".lock"), "%s.lock",
                   history->path);
    history->lock_fd = open(history->lock_path,
                            O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    return history->lock_fd < 0 ? -1 : 0;
}

static History *history_open_internal(const char *path, int shared_backend)
{
    History *history = calloc(1, sizeof(*history));
    if (history == NULL)
        return NULL;
    history->lock_fd = -1;
    history->shm_fd = -1;
    history->shared_backend = shared_backend;
#ifdef TERMINATOR_HISTORY_TESTING
    history->test_failpoint = next_open_failpoint;
    next_open_failpoint = TEST_FAIL_NONE;
#endif
    if (pthread_mutex_init(&history->mutex, NULL) != 0)
        goto fail;
    history->mutex_initialized = 1;
    if (history_path_setup(history, path) < 0)
        goto fail;
    if (shared_backend) {
        uint64_t hash = hash_path(history->path);
        (void)snprintf(history->shm_name, sizeof(history->shm_name),
                       "/terminator_history_%lu_%016llx_v%u",
                       (unsigned long)getuid(), (unsigned long long)hash,
                       SHARED_HISTORY_VERSION);
        history->shared_size = shared_mapping_size();
        (void)pthread_mutex_lock(&history->mutex);
        if (lock_file(history) < 0)
            goto fail_unlock_local;
        if (history_cleanup_staged_files(history) < 0) {
            (void)flock(history->lock_fd, LOCK_UN);
            goto fail_unlock_local;
        }
        int fd = open(history->path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            (void)flock(history->lock_fd, LOCK_UN);
            goto fail_unlock_local;
        }
        close(fd);
        if (shared_map_current_locked(history) < 0) {
            if (errno != EPROTO || shared_rebuild_locked(history, 1) < 0) {
                (void)flock(history->lock_fd, LOCK_UN);
                goto fail_unlock_local;
            }
        }
        if (!shared_header_compatible(history) &&
            shared_rebuild_locked(history, 1) < 0) {
            (void)flock(history->lock_fd, LOCK_UN);
            goto fail_unlock_local;
        }
        (void)flock(history->lock_fd, LOCK_UN);
        if (shared_begin_locked(history) < 0) {
            (void)pthread_mutex_unlock(&history->mutex);
            goto fail;
        }
        if (shared_end_locked(history) < 0) {
            (void)pthread_mutex_unlock(&history->mutex);
            goto fail;
        }
        (void)pthread_mutex_unlock(&history->mutex);
    } else {
        (void)pthread_mutex_lock(&history->mutex);
        if (lock_file(history) < 0)
            goto fail_unlock_local;
        if (history_cleanup_staged_files(history) < 0) {
            (void)flock(history->lock_fd, LOCK_UN);
            goto fail_unlock_local;
        }
        int fd = open(history->path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            (void)flock(history->lock_fd, LOCK_UN);
            goto fail;
        }
        struct stat ignored;
        int result = fstat(fd, &ignored) < 0 ? -1 : history_read_fd(history, fd);
        int saved = errno;
        close(fd);
        (void)flock(history->lock_fd, LOCK_UN);
        errno = saved;
        (void)pthread_mutex_unlock(&history->mutex);
        if (result < 0)
            goto fail;
    }
    return history;

fail_unlock_local:
    (void)pthread_mutex_unlock(&history->mutex);

fail:
    {
        int saved = errno == 0 ? EIO : errno;
        history_clear_items(history);
        if (history->shared != NULL)
            (void)munmap(history->shared, history->shared_size);
        if (history->shm_fd >= 0)
            (void)close(history->shm_fd);
        if (history->lock_fd >= 0)
            (void)close(history->lock_fd);
        if (history->mutex_initialized)
            (void)pthread_mutex_destroy(&history->mutex);
        free(history->lock_path);
        free(history->path);
        free(history);
        errno = saved;
        return NULL;
    }
}

History *history_open(const char *path)
{
    return history_open_internal(path, 0);
}

History *history_open_process_shared(const char *path)
{
    return history_open_internal(path, 1);
}

void history_close(History *history)
{
    if (history == NULL)
        return;
    history_clear_items(history);
    if (history->shared != NULL)
        (void)munmap(history->shared, history->shared_size);
    if (history->shm_fd >= 0)
        (void)close(history->shm_fd);
    if (history->lock_fd >= 0)
        (void)close(history->lock_fd);
    if (history->mutex_initialized)
        (void)pthread_mutex_destroy(&history->mutex);
    free(history->lock_path);
    free(history->path);
    free(history);
}

size_t history_count(History *history)
{
    size_t count = 0;
    (void)pthread_mutex_lock(&history->mutex);
    if (history_refresh_locked(history) == 0)
        count = history->count;
    (void)pthread_mutex_unlock(&history->mutex);
    return count;
}

char *history_get(History *history, size_t index)
{
    char *copy = NULL;
    (void)pthread_mutex_lock(&history->mutex);
    if (history_refresh_locked(history) == 0 && index < history->count)
        copy = strdup(history->items[index]);
    (void)pthread_mutex_unlock(&history->mutex);
    return copy;
}

static size_t longest_common_substring(const char *left, const char *right)
{
    size_t left_length = strlen(left), right_length = strlen(right);
    size_t *previous, *current, best = 0;
    if (left_length == 0 || right_length == 0)
        return 0;
    previous = calloc(right_length + 1, sizeof(*previous));
    current = calloc(right_length + 1, sizeof(*current));
    if (previous == NULL || current == NULL) {
        free(previous);
        free(current);
        return SIZE_MAX;
    }
    for (size_t i = 1; i <= left_length; ++i) {
        for (size_t j = 1; j <= right_length; ++j) {
            current[j] = left[i - 1] == right[j - 1] ? previous[j - 1] + 1 : 0;
            if (current[j] > best)
                best = current[j];
        }
        size_t *swap = previous;
        previous = current;
        current = swap;
        memset(current, 0, (right_length + 1) * sizeof(*current));
    }
    free(previous);
    free(current);
    return best;
}

int history_search(History *history, const char *term,
                   char ***matches, size_t *match_count)
{
    size_t best = 0, exact = SIZE_MAX, count = 0;
    char **result = NULL;
    if (history == NULL || term == NULL || matches == NULL || match_count == NULL) {
        errno = EINVAL;
        return -1;
    }
    *matches = NULL;
    *match_count = 0;
    (void)pthread_mutex_lock(&history->mutex);
    if (history_refresh_locked(history) < 0) {
        (void)pthread_mutex_unlock(&history->mutex);
        return -1;
    }
    for (size_t i = 0; i < history->count; ++i)
        if (strcmp(history->items[i], term) == 0)
            exact = i;
    if (exact != SIZE_MAX) {
        result = calloc(1, sizeof(*result));
        if (result != NULL)
            result[0] = strdup(history->items[exact]);
        if (result == NULL || result[0] == NULL) {
            free(result);
            (void)pthread_mutex_unlock(&history->mutex);
            return -1;
        }
        count = 1;
    } else {
        size_t *lengths = calloc(history->count ? history->count : 1,
                                 sizeof(*lengths));
        if (lengths == NULL) {
            (void)pthread_mutex_unlock(&history->mutex);
            return -1;
        }
        for (size_t i = 0; i < history->count; ++i) {
            lengths[i] = longest_common_substring(history->items[i], term);
            if (lengths[i] == SIZE_MAX) {
                free(lengths);
                (void)pthread_mutex_unlock(&history->mutex);
                return -1;
            }
            if (lengths[i] > best)
                best = lengths[i];
        }
        if (best > 2) {
            for (size_t i = 0; i < history->count; ++i)
                if (lengths[i] == best)
                    ++count;
            result = calloc(count, sizeof(*result));
            if (result == NULL) {
                free(lengths);
                (void)pthread_mutex_unlock(&history->mutex);
                return -1;
            }
            size_t out = 0;
            for (size_t i = 0; i < history->count; ++i) {
                if (lengths[i] == best) {
                    result[out] = strdup(history->items[i]);
                    if (result[out] == NULL) {
                        while (out > 0)
                            free(result[--out]);
                        free(result);
                        free(lengths);
                        (void)pthread_mutex_unlock(&history->mutex);
                        return -1;
                    }
                    ++out;
                }
            }
        }
        free(lengths);
    }
    (void)pthread_mutex_unlock(&history->mutex);
    *matches = result;
    *match_count = count;
    return 0;
}

int history_recent(History *history, size_t limit, char ***entries,
                   size_t *entry_count, size_t *first_index)
{
    char **result = NULL;
    size_t count, start;
    if (history == NULL || entries == NULL || entry_count == NULL ||
        first_index == NULL) {
        errno = EINVAL;
        return -1;
    }
    *entries = NULL;
    *entry_count = 0;
    *first_index = 0;
    (void)pthread_mutex_lock(&history->mutex);
    if (history_refresh_locked(history) < 0) {
        (void)pthread_mutex_unlock(&history->mutex);
        return -1;
    }
    count = history->count < limit ? history->count : limit;
    start = history->count - count;
    if (count != 0) {
        result = calloc(count, sizeof(*result));
        if (result == NULL) {
            (void)pthread_mutex_unlock(&history->mutex);
            return -1;
        }
        for (size_t i = 0; i < count; ++i) {
            result[i] = strdup(history->items[start + i]);
            if (result[i] == NULL) {
                while (i > 0)
                    free(result[--i]);
                free(result);
                (void)pthread_mutex_unlock(&history->mutex);
                return -1;
            }
        }
    }
    (void)pthread_mutex_unlock(&history->mutex);
    *entries = result;
    *entry_count = count;
    *first_index = start;
    return 0;
}

int history_add(History *history, const char *command)
{
    int result = -1;
    int renamed = 0;
    if (history == NULL || command == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (command[0] == '\0')
        return 0;
    const unsigned char *scan = (const unsigned char *)command;
    while (*scan != '\0' && isspace(*scan))
        ++scan;
    if (*scan == '\0')
        return 0;

    (void)pthread_mutex_lock(&history->mutex);
    if (history->shared_backend) {
        if (shared_begin_locked(history) < 0)
            goto done;
        if (history_append_memory(history, command, strlen(command)) < 0)
            goto shared_done;
#ifdef TERMINATOR_HISTORY_TESTING
        test_crashpoint(history, TEST_CRASH_DURING_MUTATION);
#endif
        if (!history_fits_shared(history))
            goto restore_shared;
        if (history_persist_atomic(history, &renamed) < 0) {
            int saved = errno;
            if (renamed) {
                struct stat st;
                if (stat(history->path, &st) == 0)
                    shared_copy_from_history(history, &st);
            } else {
                (void)shared_rebuild_locked(history, 0);
            }
            errno = saved;
            goto shared_done;
        }
        struct stat st;
        if (stat(history->path, &st) < 0) {
            int saved = errno;
            (void)shared_rebuild_locked(history, 0);
            errno = saved;
            goto shared_done;
        }
        shared_copy_from_history(history, &st);
        result = 0;
        goto shared_done;

restore_shared:
        {
            int saved = errno;
            (void)shared_rebuild_locked(history, 0);
            errno = saved;
        }
shared_done:
        if (shared_end_locked(history) < 0 && result == 0)
            result = -1;
    } else {
        if (lock_file(history) < 0)
            goto done;
        int fd = open(history->path, O_RDONLY | O_CLOEXEC);
        if (fd < 0 && errno == ENOENT)
            fd = open(history->path, O_RDONLY | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0 || history_read_fd(history, fd) < 0) {
            int saved = errno;
            if (fd >= 0) close(fd);
            (void)flock(history->lock_fd, LOCK_UN);
            errno = saved;
            goto done;
        }
        close(fd);
        if (history_append_memory(history, command, strlen(command)) < 0) {
            (void)flock(history->lock_fd, LOCK_UN);
            goto done;
        }
        if (history_persist_atomic(history, &renamed) == 0) {
            result = 0;
        } else {
            int saved = errno;
            struct stat ignored;
            if (history_load_file(history, &ignored) < 0)
                history_clear_items(history);
            errno = saved;
        }
        (void)flock(history->lock_fd, LOCK_UN);
    }
done:
    (void)pthread_mutex_unlock(&history->mutex);
    return result;
}

void history_cursor_reset(HistoryCursor *cursor)
{
    if (cursor == NULL)
        return;
    free(cursor->draft);
    cursor->draft = NULL;
    cursor->index = 0;
    cursor->active = 0;
}

char *history_cursor_previous(History *history, HistoryCursor *cursor,
                             const char *current_input)
{
    if (history == NULL || cursor == NULL || current_input == NULL)
        return NULL;
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

char *history_cursor_next(History *history, HistoryCursor *cursor)
{
    if (history == NULL || cursor == NULL || !cursor->active)
        return NULL;
    size_t count = history_count(history);
    if (cursor->index + 1 < count)
        return history_get(history, ++cursor->index);
    char *draft = strdup(cursor->draft == NULL ? "" : cursor->draft);
    history_cursor_reset(cursor);
    return draft;
}

#ifdef TERMINATOR_HISTORY_TESTING
int history_test_set_failpoint(History *history, int failpoint)
{
    if (history == NULL || !history->shared_backend) {
        errno = EINVAL;
        return -1;
    }
    history->test_failpoint = failpoint;
    return 0;
}

void history_test_set_next_open_failpoint(int failpoint)
{
    next_open_failpoint = failpoint;
}

int history_test_die_holding_lock(History *history)
{
    if (history == NULL || !history->shared_backend) {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&history->mutex);
    if (shared_begin_locked(history) < 0)
        return -1;
    _exit(79);
}

int history_test_corrupt_segment(History *history, int version)
{
    if (history == NULL || !history->shared_backend) {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&history->mutex);
    if (shared_begin_locked(history) < 0) {
        (void)pthread_mutex_unlock(&history->mutex);
        return -1;
    }
    if (version)
        history->shared->version = SHARED_HISTORY_VERSION + 1;
    else if (history->shared->count != 0)
        history->shared->entries[0].offset = UINT64_MAX;
    int result = shared_end_locked(history);
    (void)pthread_mutex_unlock(&history->mutex);
    return result;
}

int history_test_unlink_segment(History *history)
{
    if (history == NULL || !history->shared_backend) {
        errno = EINVAL;
        return -1;
    }
    return shm_unlink(history->shm_name);
}
#endif
