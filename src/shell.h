#ifndef TERMINATOR_SHELL_H
#define TERMINATOR_SHELL_H

#include <stddef.h>
#include <stdint.h>

typedef struct Shell Shell;
typedef struct History History;

typedef enum {
    SHELL_EVENT_OUTPUT,
    SHELL_EVENT_COMMAND_DONE,
    SHELL_EVENT_EXIT,
    SHELL_EVENT_COMPLETION,
    SHELL_EVENT_JOB_STATE
} ShellEventType;

typedef struct {
    ShellEventType type;
    char *text;
    size_t length;
    int status;
    uint64_t request_id;
    char **matches;
    size_t match_count;
} ShellEvent;

/* wake_write_fd is shared with the GUI and must remain open until shell_destroy. */
Shell *shell_create(int wake_write_fd);
/* Process backend used by the GUI parent. Child closes inherited descriptors. */
Shell *shell_create_process(int wake_write_fd, int x11_fd, uint32_t tab_id);
/* Set the shared GUI-owned history before submitting commands. */
void shell_set_history(Shell *shell, History *history);
int shell_submit(Shell *shell, const char *line);
/* Signal only the active foreground process group; returns kill()'s result. */
int shell_signal_foreground(Shell *shell, int suspend);
int shell_complete_path(Shell *shell, const char *prefix,
                       char ***matches, size_t *match_count);
/* Asynchronous process-mode completion; result arrives as SHELL_EVENT_COMPLETION. */
int shell_request_completion(Shell *shell, const char *prefix,
                             uint64_t *request_id);
void shell_completion_free(char **matches, size_t match_count);
int shell_pop_event(Shell *shell, ShellEvent *event);
void shell_event_dispose(ShellEvent *event);
void shell_request_stop(Shell *shell);
void shell_destroy(Shell *shell);

#endif
