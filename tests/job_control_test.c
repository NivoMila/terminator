#define _POSIX_C_SOURCE 200809L

#include "shell.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *text;
    size_t length;
    size_t capacity;
    int status;
} Result;

static void
append(Result *result, const char *text, size_t length)
{
    if (result->length + length + 1 > result->capacity) {
        size_t capacity = result->capacity == 0 ? 1024 : result->capacity;
        while (capacity < result->length + length + 1)
            capacity *= 2;
        char *grown = realloc(result->text, capacity);
        assert(grown != NULL);
        result->text = grown;
        result->capacity = capacity;
    }
    memcpy(result->text + result->length, text, length);
    result->length += length;
    result->text[result->length] = '\0';
}

static void
drain_wake(int fd)
{
    char bytes[256];
    while (read(fd, bytes, sizeof(bytes)) > 0)
        ;
}

static Result
collect_until_done(Shell *shell, int wake_read)
{
    Result result = {0};
    int done = 0;
    while (!done) {
        struct pollfd pfd = {.fd = wake_read, .events = POLLIN};
        int ready;
        do {
            ready = poll(&pfd, 1, 5000);
        } while (ready < 0 && errno == EINTR);
        assert(ready > 0);
        if (pfd.revents & POLLIN)
            drain_wake(wake_read);
        ShellEvent event;
        while (shell_pop_event(shell, &event)) {
            if (event.type == SHELL_EVENT_OUTPUT)
                append(&result, event.text, event.length);
            else if (event.type == SHELL_EVENT_COMMAND_DONE) {
                result.status = event.status;
                done = 1;
            }
            shell_event_dispose(&event);
        }
    }
    if (result.text == NULL)
        result.text = strdup("");
    assert(result.text != NULL);
    return result;
}

static Result
run_line(Shell *shell, int wake_read, const char *line)
{
    assert(shell_submit(shell, line) == 0);
    return collect_until_done(shell, wake_read);
}

static Result
run_and_signal(Shell *shell, int wake_read, const char *line, int suspend)
{
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 150000000};
    assert(shell_submit(shell, line) == 0);
    (void)nanosleep(&delay, NULL);
    assert(shell_signal_foreground(shell, suspend) == 0);
    return collect_until_done(shell, wake_read);
}

static void
pause_ms(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000
    };
    (void)nanosleep(&delay, NULL);
}

static void
expect_text(Result *result, const char *text)
{
    assert(strstr(result->text, text) != NULL);
    free(result->text);
}

int
main(void)
{
    int wake[2];
    Shell *shell;
    Result result;

    assert(pipe(wake) == 0);
    assert(fcntl(wake[0], F_SETFL, fcntl(wake[0], F_GETFL) | O_NONBLOCK) == 0);
    assert(fcntl(wake[1], F_SETFL, fcntl(wake[1], F_GETFL) | O_NONBLOCK) == 0);
    shell = shell_create(wake[1]);
    assert(shell != NULL);

    result = run_line(shell, wake[0], "printf foreground-ok");
    expect_text(&result, "foreground-ok");
    assert(result.status == 0);

    struct timespec bg_start, bg_finish;
    (void)clock_gettime(CLOCK_MONOTONIC, &bg_start);
    result = run_line(shell, wake[0], "sleep 1 &");
    (void)clock_gettime(CLOCK_MONOTONIC, &bg_finish);
    long bg_elapsed_ms = (long)(bg_finish.tv_sec - bg_start.tv_sec) * 1000L +
                         (bg_finish.tv_nsec - bg_start.tv_nsec) / 1000000L;
    assert(bg_elapsed_ms >= 0 && bg_elapsed_ms < 800);
    expect_text(&result, "[2]");
    result = run_line(shell, wake[0], "jobs");
    expect_text(&result, "Running");
    result = run_line(shell, wake[0], "fg %2");
    assert(result.status == 0);
    free(result.text);

    result = run_and_signal(shell, wake[0], "sleep 2", 1);
    assert(result.status == 128 + SIGTSTP);
    expect_text(&result, "Stopped");
    result = run_line(shell, wake[0], "jobs");
    expect_text(&result, "Stopped");
    result = run_line(shell, wake[0], "bg %3");
    expect_text(&result, "[3]");
    result = run_line(shell, wake[0], "jobs");
    expect_text(&result, "Running");
    result = run_line(shell, wake[0], "fg %3");
    assert(result.status == 0);
    free(result.text);

    assert(shell_signal_foreground(shell, 0) < 0); /* Idle Ctrl+C is harmless. */
    result = run_and_signal(shell, wake[0], "sleep 5", 0);
    assert(result.status == 128 + SIGINT);
    free(result.text);

    result = run_line(shell, wake[0],
                      "sh -c 'sleep 0.2; printf abc' | tr a-z A-Z | wc -c &");
    expect_text(&result, "[5]");
    result = run_line(shell, wake[0], "fg %5");
    expect_text(&result, "3");

    result = run_line(shell, wake[0], "sleep 0.1 &");
    char *job_number_end;
    char *job_start = strrchr(result.text, '[');
    assert(job_start != NULL);
    char *pid_start = strchr(job_start, ']');
    assert(pid_start != NULL);
    long child_pid = strtol(pid_start + 1, &job_number_end, 10);
    assert(job_number_end != pid_start + 1 && child_pid > 0);
    free(result.text);
    pause_ms(300);
    result = run_line(shell, wake[0], "jobs");
    expect_text(&result, "Done");
    char proc_path[64];
    (void)snprintf(proc_path, sizeof(proc_path), "/proc/%ld/stat", child_pid);
    FILE *proc_stat = fopen(proc_path, "r");
    if (proc_stat != NULL) {
        char proc_line[512];
        assert(fgets(proc_line, sizeof(proc_line), proc_stat) != NULL);
        fclose(proc_stat);
        char *state = strrchr(proc_line, ')');
        assert(state != NULL && state[2] != 'Z');
    }

    shell_destroy(shell);
    close(wake[0]);
    close(wake[1]);

    assert(pipe(wake) == 0);
    assert(fcntl(wake[0], F_SETFL, fcntl(wake[0], F_GETFL) | O_NONBLOCK) == 0);
    assert(fcntl(wake[1], F_SETFL, fcntl(wake[1], F_GETFL) | O_NONBLOCK) == 0);
    shell = shell_create(wake[1]);
    assert(shell != NULL);
    result = run_line(shell, wake[0], "sleep 30 &");
    free(result.text);
    result = run_line(shell, wake[0], "sleep 30 | cat &");
    free(result.text);
    struct timespec start, finish;
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    shell_destroy(shell);
    (void)clock_gettime(CLOCK_MONOTONIC, &finish);
    assert(finish.tv_sec - start.tv_sec < 1);
    close(wake[0]);
    close(wake[1]);
    pause_ms(150);
    puts("job control tests passed");
    return 0;
}
