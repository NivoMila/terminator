#ifndef TERMINATOR_SHELL_INTERNAL_H
#define TERMINATOR_SHELL_INTERNAL_H

#include "shell.h"

#include <pthread.h>
#include <sys/types.h>

typedef struct ShellCommand {
    char *line;
    struct ShellCommand *next;
} ShellCommand;

typedef struct ShellEventNode {
    ShellEvent event;
    struct ShellEventNode *next;
} ShellEventNode;

typedef struct ShellVariable {
    char *name;
    char *value;
    struct ShellVariable *next;
} ShellVariable;

typedef struct ShellJob {
    unsigned id;
    pid_t pgid;
    pid_t *pids;
    unsigned char *states; /* 0 running, 1 stopped, 2 exited */
    int *status_fds;
    size_t count;
    size_t remaining;
    int capture_fd;
    int stderr_fd;
    int split_stderr;
    int foreground;
    int completed;
    int completion_notified;
    int stopped_notified;
    int multiwatch;
    int multiwatch_output_started;
    int multiwatch_output_finished;
    size_t multiwatch_slot;
    size_t multiwatch_output_bytes;
    int last_status;
    char *command;
    struct ShellJob *next;
} ShellJob;

struct Shell {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t commands_ready;
    pthread_cond_t events_space;
    int initialized;
    int stopping;
    int worker_exited;
    pid_t active_pgid;
    int multiwatch_active;
    int multiwatch_stop;
    ShellVariable *variables;
    char **script_args;
    size_t script_argc;
    int script_status;
    int script_active;
    int script_cancelled;
    char *capture_buffer;
    size_t capture_length;
    size_t capture_capacity;
    int capturing_output;
    int capture_failed;
    unsigned next_job_id;
    ShellJob *jobs;
    int cwd_fd;
    char *cwd_path;
    int wake_write_fd;
    ShellCommand *command_head;
    ShellCommand *command_tail;
    ShellEventNode *event_head;
    ShellEventNode *event_tail;
    size_t queued_output_bytes;
};

int shell_emit(Shell *shell, ShellEventType type,
               const char *text, size_t length, int status);
int execute_line(Shell *shell, const char *line, int *should_exit);
void execute_reap_jobs(Shell *shell);
void execute_cleanup_jobs(Shell *shell);

#endif
