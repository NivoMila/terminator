#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "history.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int status;
} Result;

static void
append(Result *result, const char *data, size_t length)
{
    if (result->length + length + 1 > result->capacity) {
        size_t next = result->capacity == 0 ? 4096 : result->capacity;
        while (next < result->length + length + 1)
            next *= 2;
        char *grown = realloc(result->data, next);
        assert(grown != NULL);
        result->data = grown;
        result->capacity = next;
    }
    memcpy(result->data + result->length, data, length);
    result->length += length;
    result->data[result->length] = '\0';
}

static Result
run_line(Shell *shell, int wake_read, const char *line)
{
    Result result = {0};
    int complete = 0;
    assert(shell_submit(shell, line) == 0);
    while (!complete) {
        struct pollfd pfd = {.fd = wake_read, .events = POLLIN};
        int ready;
        do {
            ready = poll(&pfd, 1, 5000);
        } while (ready < 0 && errno == EINTR);
        assert(ready > 0);
        if (pfd.revents & POLLIN) {
            char bytes[256];
            while (read(wake_read, bytes, sizeof(bytes)) > 0)
                ;
        }
        ShellEvent event;
        while (shell_pop_event(shell, &event)) {
            if (event.type == SHELL_EVENT_OUTPUT)
                append(&result, event.text, event.length);
            else if (event.type == SHELL_EVENT_COMMAND_DONE) {
                result.status = event.status;
                complete = 1;
            }
            shell_event_dispose(&event);
        }
    }
    if (result.data == NULL)
        result.data = strdup("");
    assert(result.data != NULL);
    return result;
}

static void
expect_contains(Shell *shell, int wake_read, const char *line, const char *text)
{
    Result result = run_line(shell, wake_read, line);
    if (strstr(result.data, text) == NULL)
        fprintf(stderr, "command failed expectation: %s; output: %s\n", line, result.data);
    assert(strstr(result.data, text) != NULL);
    free(result.data);
}

static void
read_file(const char *path, char *buffer, size_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t amount;
    assert(fd >= 0);
    amount = read(fd, buffer, size - 1);
    assert(amount >= 0);
    buffer[amount] = '\0';
    close(fd);
}

int
main(void)
{
    char directory[] = "/tmp/terminator-stage4-XXXXXX";
    char history_path[] = "/tmp/terminator-shell-history-XXXXXX";
    char path[512];
    char contents[256];
    int wake[2];
    Shell *shell;
    History *history;
    Result result;

    assert(mkdtemp(directory) != NULL);
    assert(pipe(wake) == 0);
    int flags = fcntl(wake[0], F_GETFL, 0);
    assert(flags >= 0 && fcntl(wake[0], F_SETFL, flags | O_NONBLOCK) == 0);
    shell = shell_create(wake[1]);
    assert(shell != NULL);
    int history_fd = mkstemp(history_path);
    assert(history_fd >= 0);
    close(history_fd);
    history = history_open(history_path);
    assert(history != NULL && history_add(history, "previously-run") == 0);
    shell_set_history(shell, history);
    result = run_line(shell, wake[0], "history");
    assert(strstr(result.data, "previously-run") != NULL);
    free(result.data);

    expect_contains(shell, wake[0], "echo hello | wc -c", "6");
    expect_contains(shell, wake[0], "printf hello | wc -c", "5");
    expect_contains(shell, wake[0], "printf hello | tr h H | wc -c", "5");
    expect_contains(shell, wake[0], "sh -c 'echo stage-error >&2' | cat", "stage-error");
    result = run_line(shell, wake[0], "false | sh -c 'exit 7'");
    assert(result.status == 7);
    free(result.data);

    (void)snprintf(path, sizeof(path), "%s/input.txt", directory);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, "from-input\n", 11) == 11);
    close(fd);
    result = run_line(shell, wake[0], "cd /tmp/terminator-stage4-does-not-exist");
    free(result.data);
    char command[1024];
    (void)snprintf(command, sizeof(command), "cd %s", directory);
    result = run_line(shell, wake[0], command);
    free(result.data);
    expect_contains(shell, wake[0], "cat < input.txt", "from-input");
    expect_contains(shell, wake[0], "cat < input.txt | tr a-z A-Z", "FROM-INPUT");

    (void)snprintf(path, sizeof(path), "%s/dir space", directory);
    assert(mkdir(path, 0700) == 0);
    char nested[512];
    (void)snprintf(nested, sizeof(nested), "%s/dir space/child dir", directory);
    assert(mkdir(nested, 0700) == 0);
    (void)snprintf(path, sizeof(path), "%s/dir space/child dir/child file", directory);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    close(fd);
    (void)snprintf(path, sizeof(path), "%s/match-one", directory);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    close(fd);
    (void)snprintf(path, sizeof(path), "%s/match-two", directory);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    close(fd);
    char **matches = NULL;
    size_t match_count = 0;
    assert(shell_complete_path(shell, "dir s", &matches, &match_count) == 0);
    assert(match_count == 1 && strcmp(matches[0], "dir space/") == 0);
    shell_completion_free(matches, match_count);
    assert(shell_complete_path(shell, "dir space/child d", &matches, &match_count) == 0);
    assert(match_count == 1 && strcmp(matches[0], "dir space/child dir/") == 0);
    shell_completion_free(matches, match_count);
    assert(shell_complete_path(shell, "dir space/child dir/child f", &matches,
                               &match_count) == 0);
    assert(match_count == 1 && strcmp(matches[0], "dir space/child dir/child file") == 0);
    shell_completion_free(matches, match_count);
    assert(shell_complete_path(shell, "match-", &matches, &match_count) == 0);
    assert(match_count == 2);
    shell_completion_free(matches, match_count);

    result = run_line(shell, wake[0], "printf first > result.txt");
    free(result.data);
    result = run_line(shell, wake[0], "printf second > result.txt");
    free(result.data);
    (void)snprintf(path, sizeof(path), "%s/result.txt", directory);
    read_file(path, contents, sizeof(contents));
    assert(strcmp(contents, "second") == 0);
    result = run_line(shell, wake[0], "printf plus >> result.txt");
    free(result.data);
    read_file(path, contents, sizeof(contents));
    assert(strcmp(contents, "secondplus") == 0);

    /* Both directions retain parser order while targeting distinct streams. */
    result = run_line(shell, wake[0], "cat < input.txt > copied.txt");
    free(result.data);
    (void)snprintf(path, sizeof(path), "%s/copied.txt", directory);
    read_file(path, contents, sizeof(contents));
    assert(strcmp(contents, "from-input\n") == 0);
    result = run_line(shell, wake[0], "cat > copied.txt < input.txt");
    free(result.data);
    read_file(path, contents, sizeof(contents));
    assert(strcmp(contents, "from-input\n") == 0);
    result = run_line(shell, wake[0], "echo ignored > side-effect.txt < missing-later");
    assert(result.status != 0 && strstr(result.data, "missing-later") != NULL);
    free(result.data);
    (void)snprintf(path, sizeof(path), "%s/side-effect.txt", directory);
    assert(access(path, F_OK) == 0);

    result = run_line(shell, wake[0], "cat < missing-input-file");
    assert(result.status != 0 && strstr(result.data, "missing-input-file") != NULL);
    free(result.data);
    expect_contains(shell, wake[0], "seq 1 100000 | wc -l", "100000");

    shell_destroy(shell);
    history_close(history);
    assert(unlink(history_path) == 0);
    close(wake[0]);
    close(wake[1]);

    assert(pipe(wake) == 0);
    shell = shell_create(wake[1]);
    assert(shell != NULL);
    assert(shell_submit(shell, "sleep 30 | cat") == 0);
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
    (void)nanosleep(&delay, NULL);
    shell_destroy(shell);
    close(wake[0]);
    close(wake[1]);
    (void)snprintf(path, sizeof(path), "%s/copied.txt", directory);
    assert(unlink(path) == 0);
    (void)snprintf(path, sizeof(path), "%s/side-effect.txt", directory);
    assert(unlink(path) == 0);
    (void)snprintf(path, sizeof(path), "%s/input.txt", directory);
    assert(unlink(path) == 0);
    (void)snprintf(path, sizeof(path), "%s/dir space/child dir/child file", directory);
    assert(unlink(path) == 0);
    (void)snprintf(path, sizeof(path), "%s/dir space/child dir", directory);
    assert(rmdir(path) == 0);
    (void)snprintf(path, sizeof(path), "%s/dir space", directory);
    assert(rmdir(path) == 0);
    (void)snprintf(path, sizeof(path), "%s/match-one", directory);
    assert(unlink(path) == 0);
    (void)snprintf(path, sizeof(path), "%s/match-two", directory);
    assert(unlink(path) == 0);
    (void)snprintf(path, sizeof(path), "%s/result.txt", directory);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
    puts("shell pipeline/redirection tests passed");
    return 0;
}
