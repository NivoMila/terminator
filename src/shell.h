#ifndef TERMINATOR_SHELL_H
#define TERMINATOR_SHELL_H

#include <stddef.h>

typedef struct Shell Shell;

typedef enum {
    SHELL_EVENT_OUTPUT,
    SHELL_EVENT_COMMAND_DONE,
    SHELL_EVENT_EXIT
} ShellEventType;

typedef struct {
    ShellEventType type;
    char *text;
    size_t length;
    int status;
} ShellEvent;

/* wake_write_fd is shared with the GUI and must remain open until shell_destroy. */
Shell *shell_create(int wake_write_fd);
int shell_submit(Shell *shell, const char *line);
/* Signal only the active foreground process group; returns kill()'s result. */
int shell_signal_foreground(Shell *shell, int suspend);
int shell_complete_path(Shell *shell, const char *prefix,
                       char ***matches, size_t *match_count);
void shell_completion_free(char **matches, size_t match_count);
int shell_pop_event(Shell *shell, ShellEvent *event);
void shell_event_dispose(ShellEvent *event);
void shell_request_stop(Shell *shell);
void shell_destroy(Shell *shell);

#endif
