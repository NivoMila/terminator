#ifndef TERMINATOR_SHELL_INTERNAL_H
#define TERMINATOR_SHELL_INTERNAL_H

#include "shell.h"
#include "history.h"
#include "process_runtime.h"

#include <pthread.h>
#include <sys/types.h>

typedef struct ShellState ShellState;

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

typedef int (*ShellStateEmit)(void *context, ShellEventType type,
                              const char *text, size_t length, int status);

/* Execution state has no worker queue or thread lifecycle. A runtime supplies
 * the output sink and invokes shell_state_execute() when it receives work. */
struct ShellState {
    pthread_mutex_t mutex;
    int stopping;
    int mutex_initialized;
    int shutdown_complete;
    int process_backend;
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
    History *history;
    int control_fd;
    ShellStateEmit emit;
    void *emit_context;
};

/* Thread-mode runtime handle. The public Shell API continues to refer to this
 * adapter; process-mode code can own ShellState without starting a thread. */
struct Shell {
    int process_mode;
    ProcessRuntime *process;
    ShellState state;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t commands_ready;
    pthread_cond_t events_space;
    int initialized;
    int stopping;
    int worker_exited;
    int synchronization_initialized;
    int wake_write_fd;
    ShellCommand *command_head;
    ShellCommand *command_tail;
    ShellEventNode *event_head;
    ShellEventNode *event_tail;
    size_t queued_output_bytes;
};

int shell_emit(ShellState *state, ShellEventType type,
               const char *text, size_t length, int status);
int execute_line(ShellState *state, const char *line, int *should_exit);
int execute_interactive_line(ShellState *state, const char *line, int *should_exit);
void execute_reap_jobs(ShellState *state);
void execute_cleanup_jobs(ShellState *state);

int shell_state_init(ShellState *state, History *history,
                     ShellStateEmit emit, void *emit_context);
int shell_state_execute(ShellState *state, const char *line, int *should_exit);
void shell_state_reap_jobs(ShellState *state);
int shell_state_signal_foreground(ShellState *state, int suspend);
int shell_state_service_control(ShellState *state);
void shell_state_request_shutdown(ShellState *state);
int shell_state_complete_path(ShellState *state, const char *prefix,
                              char ***matches, size_t *match_count);
void shell_state_shutdown(ShellState *state);
void shell_state_destroy(ShellState *state);

#endif
