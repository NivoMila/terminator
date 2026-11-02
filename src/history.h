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
void history_cursor_reset(HistoryCursor *cursor);
char *history_cursor_previous(History *history, HistoryCursor *cursor,
                              const char *current_input);
char *history_cursor_next(History *history, HistoryCursor *cursor);

#endif
