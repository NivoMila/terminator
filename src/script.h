#ifndef TERMINATOR_SCRIPT_H
#define TERMINATOR_SCRIPT_H

#include <stddef.h>

typedef struct ShellState ShellState;
typedef struct ScriptProgram ScriptProgram;

typedef struct {
    size_t line;
    char message[192];
} ScriptError;

/* Parse the complete script into an owned AST; no commands are executed. */
int script_parse(const char *text, ScriptProgram **program, ScriptError *error);
void script_program_free(ScriptProgram *program);
int script_execute(ShellState *shell, ScriptProgram *program,
                   int argc, char **argv, int *exit_status);

/* Execute an invocation. source_mode controls whether cwd/variables persist. */
int script_invoke(ShellState *shell, const char *path, int argc, char **argv,
                  int source_mode, int *exit_status);
const char *script_variable_get(ShellState *shell, const char *name);
/* Expand one interactive command using the script interpreter's expansion rules. */
int script_expand_command(ShellState *shell, const char *command, char **expanded);

#endif
