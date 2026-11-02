#include "editor.h"

#include <ctype.h>
#include <string.h>

void
editor_move_start(size_t *cursor)
{
    *cursor = 0;
}

void
editor_move_end(size_t length, size_t *cursor)
{
    *cursor = length;
}

void
editor_kill_to_start(char *text, size_t *length, size_t *cursor)
{
    if (*cursor > *length)
        *cursor = *length;
    memmove(text, text + *cursor, *length - *cursor + 1);
    *length -= *cursor;
    *cursor = 0;
}

void
editor_kill_to_end(char *text, size_t *length, size_t *cursor)
{
    if (*cursor > *length)
        *cursor = *length;
    *length = *cursor;
    text[*length] = '\0';
}

void
editor_kill_previous_word(char *text, size_t *length, size_t *cursor)
{
    size_t start;
    if (*cursor > *length)
        *cursor = *length;
    start = *cursor;
    while (start > 0 && isspace((unsigned char)text[start - 1]))
        --start;
    while (start > 0 && !isspace((unsigned char)text[start - 1]))
        --start;
    memmove(text + start, text + *cursor, *length - *cursor + 1);
    *length -= *cursor - start;
    *cursor = start;
}
