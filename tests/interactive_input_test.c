#include "interactive_input.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static InteractiveInputResult
enter(InteractiveInput *input, const char *line, char **command, ParseError *error)
{
    return interactive_input_enter(input, line, command, error);
}

int
main(void)
{
    InteractiveInput input = {0};
    ParseError error;
    char *command = NULL;

    assert(enter(&input, "echo \"first", &command, &error) == INTERACTIVE_INPUT_CONTINUE);
    assert(input.active && command == NULL);
    assert(enter(&input, "second\"", &command, &error) == INTERACTIVE_INPUT_READY);
    assert(strcmp(command, "echo \"first\nsecond\"") == 0);
    free(command);

    assert(enter(&input, "echo $(printf first", &command, &error) == INTERACTIVE_INPUT_CONTINUE);
    assert(enter(&input, "second)", &command, &error) == INTERACTIVE_INPUT_READY);
    assert(strcmp(command, "echo $(printf first\nsecond)") == 0);
    free(command);

    assert(enter(&input, "echo $(printf '%s' $(printf inner", &command, &error) ==
           INTERACTIVE_INPUT_CONTINUE);
    assert(enter(&input, "))", &command, &error) == INTERACTIVE_INPUT_READY);
    assert(strcmp(command, "echo $(printf '%s' $(printf inner\n))") == 0);
    free(command);

    assert(enter(&input, "echo `printf first", &command, &error) == INTERACTIVE_INPUT_CONTINUE);
    assert(enter(&input, "second`", &command, &error) == INTERACTIVE_INPUT_READY);
    assert(strcmp(command, "echo `printf first\nsecond`") == 0);
    free(command);

    assert(enter(&input, "echo $(printf '%s' `printf", &command, &error) ==
           INTERACTIVE_INPUT_CONTINUE);
    assert(enter(&input, "nested`)", &command, &error) == INTERACTIVE_INPUT_READY);
    assert(strcmp(command, "echo $(printf '%s' `printf\nnested`)") == 0);
    free(command);

    assert(enter(&input, "echo joined\\", &command, &error) == INTERACTIVE_INPUT_CONTINUE);
    assert(input.splice_next);
    assert(enter(&input, "word", &command, &error) == INTERACTIVE_INPUT_READY);
    assert(strcmp(command, "echo joinedword") == 0);
    free(command);

    assert(enter(&input, "echo |", &command, &error) == INTERACTIVE_INPUT_INVALID);
    assert(!input.active && command != NULL);
    assert(error.incomplete == 0);
    free(command);

    assert(enter(&input, "echo $(printf open", &command, &error) == INTERACTIVE_INPUT_CONTINUE);
    assert(interactive_input_cancel(&input) == 1);
    assert(!input.active && input.pending == NULL);
    assert(interactive_input_cancel(&input) == 0);
    assert(enter(&input, "echo usable", &command, &error) == INTERACTIVE_INPUT_READY);
    assert(strcmp(command, "echo usable") == 0);
    free(command);

    interactive_input_destroy(&input);
    puts("interactive input tests passed");
    return 0;
}
