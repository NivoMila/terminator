#ifndef TERMINATOR_EDITOR_H
#define TERMINATOR_EDITOR_H

#include <stddef.h>

void editor_move_start(size_t *cursor);
void editor_move_end(size_t length, size_t *cursor);
void editor_kill_to_start(char *text, size_t *length, size_t *cursor);
void editor_kill_to_end(char *text, size_t *length, size_t *cursor);
void editor_kill_previous_word(char *text, size_t *length, size_t *cursor);

#endif
