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

#endif
