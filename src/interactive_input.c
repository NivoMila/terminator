#include "interactive_input.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void
clear_pending(InteractiveInput *input)
{
    free(input->pending);
    input->pending = NULL;
    input->length = 0;
    input->active = 0;
    input->splice_next = 0;
}

InteractiveInputResult
interactive_input_enter(InteractiveInput *input, const char *line,
                        char **command_out, ParseError *error)
{
    ParseError local_error;
    size_t line_length, separator, total;
    char *candidate;
    int state;

    if (input == NULL || line == NULL || command_out == NULL) {
        errno = EINVAL;
        return INTERACTIVE_INPUT_INVALID;
    }
    if (error == NULL)
        error = &local_error;
    memset(error, 0, sizeof(*error));
    *command_out = NULL;
    line_length = strlen(line);
    separator = input->active && !input->splice_next ? 1 : 0;
    if (input->length > SIZE_MAX - line_length - separator - 1) {
        errno = EOVERFLOW;
        return INTERACTIVE_INPUT_INVALID;
    }
    total = input->length + separator + line_length;
    candidate = malloc(total + 1);
    if (candidate == NULL)
        return INTERACTIVE_INPUT_INVALID;
    if (input->length != 0)
        memcpy(candidate, input->pending, input->length);
    size_t offset = input->length;
    if (separator)
        candidate[offset++] = '\n';
    memcpy(candidate + offset, line, line_length);
    candidate[total] = '\0';

    state = parser_classify_input(candidate, error);
    if (state == 1) {
        size_t saved_length = total;
        input->splice_next = error != NULL && error->line_continuation;
        if (input->splice_next && saved_length != 0)
            candidate[--saved_length] = '\0';
        free(input->pending);
        input->pending = candidate;
        input->length = saved_length;
        input->active = 1;
        return INTERACTIVE_INPUT_CONTINUE;
    }
    clear_pending(input);
    *command_out = candidate;
    return state < 0 ? INTERACTIVE_INPUT_INVALID : INTERACTIVE_INPUT_READY;
}

int
interactive_input_cancel(InteractiveInput *input)
{
    int was_active;
    if (input == NULL)
        return 0;
    was_active = input->active;
    clear_pending(input);
    return was_active;
}

void
interactive_input_destroy(InteractiveInput *input)
{
    if (input != NULL)
        clear_pending(input);
}
