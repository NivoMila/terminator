#define _POSIX_C_SOURCE 200809L

#include "shell_internal.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { char *text; size_t length; } Output;

static int collect(void *context, ShellEventType type, const char *text,
                   size_t length, int status)
{
    Output *out = context;
    char *grown;
    (void)status;
    if (type != SHELL_EVENT_OUTPUT || length == 0) return 0;
    grown = realloc(out->text, out->length + length + 1);
    if (grown == NULL) return -1;
    out->text = grown;
    memcpy(out->text + out->length, text, length);
    out->length += length;
    out->text[out->length] = '\0';
    return 0;
}

static void reset_output(Output *out)
{
    free(out->text);
    out->text = NULL;
    out->length = 0;
}

static int run(ShellState *state, Output *out, const char *line)
{
    int should_exit = 0;
    reset_output(out);
    return shell_state_execute(state, line, &should_exit);
}

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    size_t length = strlen(text), offset = 0;
    assert(fd >= 0);
    while (offset < length) {
        ssize_t n = write(fd, text + offset, length - offset);
        if (n > 0) offset += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else assert(!"write failed");
    }
    assert(close(fd) == 0);
}

static void read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    assert(fd >= 0);
    n = read(fd, buf, cap - 1);
    assert(n >= 0);
    buf[n] = '\0';
    assert(close(fd) == 0);
}

static void pause_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
}

typedef struct { ShellState *state; int status; } WatchRun;
static void *run_watch(void *arg)
{
    WatchRun *w = arg;
    int should_exit = 0;
    w->status = shell_state_execute(w->state,
        "multiWatch [\"printf state-watch\"]", &should_exit);
    return NULL;
}

int main(void)
{
    char original_cwd[4096], temp[] = "/tmp/terminator-state-XXXXXX";
    char history_path[4096], script_path[4096], result_path[4096];
    char content[256], **matches = NULL;
    size_t match_count = 0;
    History *history;
    ShellState state;
    Output output = {0};

    assert(getcwd(original_cwd, sizeof(original_cwd)) != NULL);
    assert(mkdtemp(temp) != NULL);
    assert(snprintf(history_path, sizeof(history_path), "%s/history", temp) > 0);
    assert(snprintf(script_path, sizeof(script_path), "%s/run.sh", temp) > 0);
    assert(snprintf(result_path, sizeof(result_path), "%s/result", temp) > 0);
    history = history_open(history_path);
    assert(history != NULL);
    assert(history_add(history, "state-history-marker") == 0);
    assert(shell_state_init(&state, history, collect, &output) == 0);

    /* Per-state cwd and command execution do not chdir the hosting process. */
    assert(run(&state, &output, "cd /tmp") == 0);
    assert(getcwd(content, sizeof(content)) != NULL);
    assert(strcmp(content, original_cwd) == 0);

    /* Pipeline and redirection flow through the reusable executor. */
    {
        char command[8192];
        assert(snprintf(command, sizeof(command),
                        "printf state-engine | tr a-z A-Z > %s", result_path) > 0);
        assert(run(&state, &output, command) == 0);
        read_file(result_path, content, sizeof(content));
        assert(strcmp(content, "STATE-ENGINE") == 0);
    }

    /* Script interpreter and history built-in retain their shared state. */
    write_file(script_path, "echo state-script-output\n");
    {
        char command[8192];
        assert(snprintf(command, sizeof(command), "bash %s", script_path) > 0);
        assert(run(&state, &output, command) == 0);
        assert(output.text != NULL && strstr(output.text, "state-script-output") != NULL);
    }
    assert(run(&state, &output, "history") == 0);
    assert(output.text != NULL && strstr(output.text, "state-history-marker") != NULL);

    /* Completion is callable directly against the state-owned logical cwd. */
    assert(run(&state, &output, "cd /tmp") == 0);
    assert(shell_state_complete_path(&state, "terminator-state-", &matches,
                                     &match_count) == 0);
    assert(match_count > 0);
    shell_completion_free(matches, match_count);
    matches = NULL;

    /* Background job bookkeeping and reaping are engine operations. */
    assert(run(&state, &output, "sleep 0.05 &") == 0);
    pause_ms(200);
    shell_state_reap_jobs(&state);
    assert(run(&state, &output, "jobs") == 0);

    /* multiWatch can be interrupted through the runtime-neutral state hook. */
    {
        WatchRun watch = { .state = &state, .status = -1 };
        pthread_t thread;
        assert(pthread_create(&thread, NULL, run_watch, &watch) == 0);
        pause_ms(200);
        assert(shell_state_signal_foreground(&state, 0) == 0);
        assert(pthread_join(thread, NULL) == 0);
        assert(watch.status == 0);
        assert(output.text != NULL && strstr(output.text, "state-watch") != NULL);
    }

    /* Destruction cleans up owned jobs and state resources. */
    assert(run(&state, &output, "sleep 30 &") == 0);
    assert(state.jobs != NULL);
    {
        pid_t pgid = state.jobs->pgid;
        shell_state_destroy(&state);
        errno = 0;
        assert(kill(-pgid, 0) < 0 && errno == ESRCH);
    }

    reset_output(&output);
    history_close(history);
    char history_lock_path[4200];
    assert(snprintf(history_lock_path, sizeof(history_lock_path), "%s.lock",
                    history_path) > 0);
    assert(unlink(history_lock_path) == 0);
    unlink(history_path);
    unlink(script_path);
    unlink(result_path);
    assert(rmdir(temp) == 0);
    puts("shell state interface tests passed");
    return 0;
}
