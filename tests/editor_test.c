#include "editor.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
    char text[64] = "one two three";
    size_t length = strlen(text), cursor = 7;

    editor_move_start(&cursor);
    assert(cursor == 0);
    editor_move_end(length, &cursor);
    assert(cursor == length);

    cursor = 7;
    editor_kill_previous_word(text, &length, &cursor);
    assert(strcmp(text, "one  three") == 0 && length == strlen(text) && cursor == 4);
    editor_kill_to_end(text, &length, &cursor);
    assert(strcmp(text, "one ") == 0 && length == 4 && cursor == 4);

    strcpy(text, "prefix middle suffix");
    length = strlen(text);
    cursor = 7;
    editor_kill_to_start(text, &length, &cursor);
    assert(strcmp(text, "middle suffix") == 0 && length == strlen(text) && cursor == 0);

    strcpy(text, "one two   ");
    length = strlen(text);
    cursor = length;
    editor_kill_previous_word(text, &length, &cursor);
    assert(strcmp(text, "one ") == 0 && length == 4 && cursor == 4);

    puts("editor tests passed");
    return 0;
}
