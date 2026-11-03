#ifndef TERMINATOR_HISTORY_H
#define TERMINATOR_HISTORY_H

#include <stddef.h>

typedef struct History History;

typedef struct {
    size_t index;
    int active;
    char *draft;
} HistoryCursor;

History *history_open(const char *path);
/* Process-mode backend. Every attach for the same path shares its fixed-size
 * POSIX shared-memory index, robust process-shared mutex, and semaphore. */
History *history_open_process_shared(const char *path);
void history_close(History *history);
size_t history_count(History *history);
char *history_get(History *history, size_t index);
int history_add(History *history, const char *command);
/* Return the newest exact match, or every command tied for the longest
 * common contiguous substring longer than two bytes. Caller frees results. */
int history_search(History *history, const char *term,
                   char ***matches, size_t *match_count);
/* Caller frees the returned array and its strings. */
int history_recent(History *history, size_t limit,
                   char ***entries, size_t *entry_count, size_t *first_index);
void history_cursor_reset(HistoryCursor *cursor);
char *history_cursor_previous(History *history, HistoryCursor *cursor,
                              const char *current_input);
char *history_cursor_next(History *history, HistoryCursor *cursor);

#ifdef TERMINATOR_HISTORY_TESTING
enum {
    HISTORY_TEST_FAIL_WRITE = 1,
    HISTORY_TEST_FAIL_FSYNC = 2,
    HISTORY_TEST_FAIL_RENAME = 3,
    HISTORY_TEST_CRASH_AFTER_STAGE_WRITE = 4,
    HISTORY_TEST_CRASH_AFTER_STAGE_FSYNC = 5,
    HISTORY_TEST_CRASH_AFTER_RENAME = 6,
    HISTORY_TEST_CRASH_DURING_SHARED_SYNC = 7,
    HISTORY_TEST_CRASH_AFTER_DIR_FSYNC = 8,
    HISTORY_TEST_FAIL_DIR_FSYNC = 9,
    HISTORY_TEST_CRASH_AFTER_MUTEX_BEFORE_SEM = 10,
    HISTORY_TEST_CRASH_AFTER_SEM_ACQUIRE = 11,
    HISTORY_TEST_CRASH_DURING_MUTATION = 12,
    HISTORY_TEST_CRASH_AFTER_SEM_RELEASE = 13,
    HISTORY_TEST_CRASH_DURING_SEM_RECOVERY = 14,
    HISTORY_TEST_CRASH_DURING_INDEX_RECOVERY = 15,
    HISTORY_TEST_CRASH_DURING_SEM_RESTORE = 16
};
int history_test_set_failpoint(History *history, int failpoint);
void history_test_set_next_open_failpoint(int failpoint);
int history_test_die_holding_lock(History *history);
int history_test_corrupt_segment(History *history, int version);
int history_test_unlink_segment(History *history);
#endif

#endif
