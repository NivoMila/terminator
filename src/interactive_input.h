#ifndef TERMINATOR_INTERACTIVE_INPUT_H
#define TERMINATOR_INTERACTIVE_INPUT_H

#include "parser.h"

#include <stddef.h>

typedef struct {
    char *pending;
    size_t length;
    int active;
    int splice_next;
} InteractiveInput;

typedef enum {
    INTERACTIVE_INPUT_READY,
    INTERACTIVE_INPUT_CONTINUE,
    INTERACTIVE_INPUT_INVALID
} InteractiveInputResult;

/* command_out is owned by the caller for READY/INVALID; it is NULL for CONTINUE. */
InteractiveInputResult interactive_input_enter(InteractiveInput *input,
                                                const char *line,
                                                char **command_out,
                                                ParseError *error);
int interactive_input_cancel(InteractiveInput *input);
void interactive_input_destroy(InteractiveInput *input);

#endif
