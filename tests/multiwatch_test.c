#define _POSIX_C_SOURCE 200809L
#include "shell.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { char *text; size_t length, capacity; int done, status; } Result;

static void append(Result *r, const char *s, size_t n) {
    if (r->length + n + 1 > r->capacity) {
        size_t c = r->capacity ? r->capacity : 2048;
        while (c < r->length + n + 1) c *= 2;
        char *p = realloc(r->text, c); assert(p != NULL); r->text = p; r->capacity = c;
    }
    memcpy(r->text + r->length, s, n); r->length += n; r->text[r->length] = 0;
}

static int make_wake(int fd[2]) {
    if (pipe(fd) < 0) return -1;
    for (int i = 0; i < 2; ++i) {
        int f = fcntl(fd[i], F_GETFL);
        if (f < 0 || fcntl(fd[i], F_SETFL, f | O_NONBLOCK) < 0) return -1;
    }
    return 0;
}

static void drain(int fd) { char b[256]; while (read(fd, b, sizeof(b)) > 0) {} }

static void collect_events(Shell *s, int wake, Result *r) {
    struct pollfd p = {.fd = wake, .events = POLLIN};
    int n; do { n = poll(&p, 1, 5000); } while (n < 0 && errno == EINTR);
    assert(n > 0); drain(wake);
    ShellEvent e;
    while (shell_pop_event(s, &e)) {
        if (e.type == SHELL_EVENT_OUTPUT) append(r, e.text, e.length);
        if (e.type == SHELL_EVENT_COMMAND_DONE) { r->done = 1; r->status = e.status; }
        shell_event_dispose(&e);
    }
}

static Result finish(Shell *s, int wake) {
    Result r = {0};
    while (!r.done) collect_events(s, wake, &r);
    if (!r.text) r.text = strdup("");
    assert(r.text != NULL); return r;
}

static void pause_ms(long ms) {
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

int main(void) {
    int wa[2], wb[2];
    assert(make_wake(wa) == 0 && make_wake(wb) == 0);
    Shell *a = shell_create(wa[1]), *b = shell_create(wb[1]);
    assert(a && b);

    assert(shell_submit(a, "multiWatch [\"printf one\"]") == 0);
    pause_ms(350);
    assert(shell_signal_foreground(a, 0) == 0);
    Result r = finish(a, wa[0]);
    assert(strstr(r.text, "\"printf one\", ") && strstr(r.text, "one"));
    assert(strstr(r.text, "----------------------------------------------------"));
    free(r.text);

    assert(shell_submit(a, "multiWatch [\"printf alpha\", \"printf beta\"]") == 0);
    pause_ms(350);
    assert(shell_signal_foreground(a, 0) == 0);
    r = finish(a, wa[0]);
    assert(strstr(r.text, "\"printf alpha\"") && strstr(r.text, "\"printf beta\""));
    free(r.text);

    assert(shell_submit(a, "multiWatch [\"printf repeat\"]") == 0);
    pause_ms(2350);
    assert(shell_signal_foreground(a, 0) == 0);
    r = finish(a, wa[0]);
    char *first = strstr(r.text, "\"printf repeat\"");
    assert(first != NULL && strstr(first + 1, "\"printf repeat\"") != NULL);
    char *timestamp1 = strchr(first, ',');
    char *timestamp2 = strchr(strstr(first + 1, "\"printf repeat\""), ',');
    assert(timestamp1 && timestamp2 && strtoll(timestamp2 + 1, NULL, 10) -
           strtoll(timestamp1 + 1, NULL, 10) >= 2);
    free(r.text);

    assert(shell_submit(a, "multiWatch [\"sh -c 'sleep 1; printf slow'\", \"sh -c 'sleep 1; printf fast'\"]") == 0);
    pause_ms(1400);
    assert(shell_signal_foreground(a, 0) == 0);
    r = finish(a, wa[0]);
    assert(strstr(r.text, "slow") && strstr(r.text, "fast"));
    free(r.text);

    assert(shell_submit(a, "multiWatch [\"seq 1 3000\"]") == 0);
    pause_ms(350);
    assert(shell_signal_foreground(a, 0) == 0);
    r = finish(a, wa[0]);
    assert(strstr(r.text, "3000") != NULL);
    free(r.text);

    assert(shell_submit(a, "multiWatch [\"terminator-command-that-does-not-exist\"]") == 0);
    pause_ms(250);
    assert(shell_signal_foreground(a, 0) == 0);
    r = finish(a, wa[0]);
    assert(strstr(r.text, "terminator-command-that-does-not-exist") != NULL);
    free(r.text);

    assert(shell_submit(a, "multiWatch [\"sleep 30\"]") == 0);
    assert(shell_submit(b, "multiWatch [\"printf other-tab\"]") == 0);
    pause_ms(250);
    assert(shell_signal_foreground(a, 0) == 0);
    r = finish(a, wa[0]); free(r.text);
    assert(shell_signal_foreground(b, 0) == 0);
    r = finish(b, wb[0]);
    assert(strstr(r.text, "other-tab") != NULL); free(r.text);

    assert(shell_submit(a, "multiWatch [\"sleep 30\"]") == 0);
    pause_ms(250);
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    shell_destroy(a);
    clock_gettime(CLOCK_MONOTONIC, &after);
    assert(after.tv_sec - before.tv_sec < 2);
    shell_destroy(b);
    close(wa[0]); close(wa[1]); close(wb[0]); close(wb[1]);
    puts("multiWatch tests passed");
    return 0;
}
