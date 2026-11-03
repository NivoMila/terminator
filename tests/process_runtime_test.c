#define _POSIX_C_SOURCE 200809L

#include "process_runtime.h"
#include "shell.h"
#include "shell_internal.h"
#include "completion_selection.h"
#include "history.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint64_t id;
    char **matches;
    size_t count;
    int status;
} CompletionResult;

typedef struct {
    char *bytes;
    size_t length;
    int done;
    int status;
    int exited;
    size_t job_updates;
    int last_job_state;
    int last_job_foreground;
    CompletionResult *completions;
    size_t completion_count;
} Result;

static void free_completion_result(CompletionResult *completion)
{
    shell_completion_free(completion->matches, completion->count);
    memset(completion, 0, sizeof(*completion));
}

static void collect_event(Shell *shell, Result *result)
{
    ShellEvent event;
    while (shell_pop_event(shell, &event)) {
        if (event.type == SHELL_EVENT_OUTPUT && event.length != 0) {
            char *grown = realloc(result->bytes, result->length + event.length + 1);
            assert(grown != NULL);
            result->bytes = grown;
            memcpy(result->bytes + result->length, event.text, event.length);
            result->length += event.length;
            result->bytes[result->length] = '\0';
        } else if (event.type == SHELL_EVENT_COMMAND_DONE) {
            result->done = 1;
            result->status = event.status;
        } else if (event.type == SHELL_EVENT_EXIT) {
            result->exited = 1;
            result->status = event.status;
        } else if (event.type == SHELL_EVENT_JOB_STATE) {
            ++result->job_updates;
            result->last_job_state = event.status;
            if (event.length == 16)
                result->last_job_foreground = (unsigned char)event.text[13];
        } else if (event.type == SHELL_EVENT_COMPLETION) {
            CompletionResult *grown = realloc(result->completions,
                (result->completion_count + 1) * sizeof(*grown));
            assert(grown != NULL);
            result->completions = grown;
            CompletionResult *completion =
                &result->completions[result->completion_count++];
            memset(completion, 0, sizeof(*completion));
            completion->id = event.request_id;
            completion->status = event.status;
            completion->count = event.match_count;
            if (event.match_count != 0) {
                completion->matches = calloc(event.match_count,
                                             sizeof(*completion->matches));
                assert(completion->matches != NULL);
                for (size_t i = 0; i < event.match_count; ++i) {
                    completion->matches[i] = strdup(event.matches[i]);
                    assert(completion->matches[i] != NULL);
                }
            }
        }
        shell_event_dispose(&event);
    }
}

static void wait_events(Shell *shell, int wake_fd, Result *result,
                        int wait_done, int wait_exit)
{
    for (int attempt = 0; attempt < 2000; ++attempt) {
        struct pollfd fd = { .fd = wake_fd, .events = POLLIN };
        collect_event(shell, result);
        if ((!wait_done || result->done) && (!wait_exit || result->exited))
            return;
        (void)poll(&fd, 1, 10);
        if (fd.revents & POLLIN) {
            unsigned char bytes[256];
            while (read(wake_fd, bytes, sizeof(bytes)) > 0) { }
        }
    }
    assert(!"timed out waiting for process runtime event");
}

static Result run_command(Shell *shell, int wake_fd, const char *command)
{
    Result result = {0};
    assert(shell_submit(shell, command) == 0);
    wait_events(shell, wake_fd, &result, 1, 0);
    return result;
}

static void delay_ms(long milliseconds)
{
    struct timespec delay = { milliseconds / 1000,
                              (milliseconds % 1000) * 1000000L };
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) { }
}

static void submit_and_allow_start(Shell *shell, const char *command)
{
    assert(shell_submit(shell, command) == 0);
    delay_ms(150);
}

static int output_contains(const Result *result, const char *needle)
{
    return result->bytes != NULL && strstr(result->bytes, needle) != NULL;
}

static void dispose_result(Result *result)
{
    free(result->bytes);
    for (size_t i = 0; i < result->completion_count; ++i)
        free_completion_result(&result->completions[i]);
    free(result->completions);
    memset(result, 0, sizeof(*result));
}

static int has_match(const CompletionResult *completion, const char *candidate)
{
    for (size_t i = 0; i < completion->count; ++i)
        if (strcmp(completion->matches[i], candidate) == 0)
            return 1;
    return 0;
}

static CompletionResult request_completion(Shell *shell, int wake_fd,
                                           const char *prefix)
{
    Result events = {0};
    uint64_t request_id;
    CompletionResult result = {0};
    assert(shell_request_completion(shell, prefix, &request_id) == 0);
    for (int attempt = 0; attempt < 2000; ++attempt) {
        struct pollfd fd = { .fd = wake_fd, .events = POLLIN };
        collect_event(shell, &events);
        for (size_t i = 0; i < events.completion_count; ++i) {
            if (events.completions[i].id == request_id) {
                result = events.completions[i];
                memset(&events.completions[i], 0, sizeof(events.completions[i]));
                dispose_result(&events);
                return result;
            }
        }
        (void)poll(&fd, 1, 10);
        if (fd.revents & POLLIN) {
            unsigned char bytes[256];
            while (read(wake_fd, bytes, sizeof(bytes)) > 0) { }
        }
    }
    dispose_result(&events);
    assert(!"timed out waiting for process completion response");
    return result;
}

static void create_file(const char *directory, const char *name)
{
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);
}

static void create_nonexec_fixture(const char *path)
{
    static const char contents[] = "plain non-executable fixture\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    size_t offset = 0;
    assert(fd >= 0);
    while (offset < sizeof(contents) - 1) {
        ssize_t amount = write(fd, contents + offset,
                              sizeof(contents) - 1 - offset);
        assert(amount > 0);
        offset += (size_t)amount;
    }
    assert(fchmod(fd, 0600) == 0);
    assert(close(fd) == 0);
}

static void read_small_file(const char *path, char *buffer, size_t capacity)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0 && capacity > 0);
    ssize_t amount = read(fd, buffer, capacity - 1);
    assert(amount >= 0);
    buffer[amount] = '\0';
    assert(close(fd) == 0);
}

static void remove_file(const char *directory, const char *name)
{
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    assert(unlink(path) == 0);
}

static pid_t first_child_pid(pid_t parent)
{
    char path[128], text[256];
    long child;
    int n = snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children",
                     (long)parent, (long)parent);
    assert(n > 0 && (size_t)n < sizeof(path));
    for (int attempt = 0; attempt < 300; ++attempt) {
        FILE *file = fopen(path, "r");
        assert(file != NULL);
        if (fgets(text, sizeof(text), file) == NULL) text[0] = '\0';
        fclose(file);
        if (sscanf(text, "%ld", &child) == 1)
            return (pid_t)child;
        delay_ms(10);
    }
    assert(!"timed out waiting for command child");
    return -1;
}

static int process_is_executing(pid_t pid)
{
    char path[64], line[1024];
    int n = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *file = fopen(path, "r");
    if (file == NULL) return 0;
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 0;
    }
    fclose(file);
    char *close_paren = strrchr(line, ')');
    return close_paren != NULL && close_paren[1] == ' ' &&
           close_paren[2] != 'Z' && close_paren[2] != 'X';
}

static size_t open_fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    size_t count = 0;
    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL)
        if (entry->d_name[0] != '.') ++count;
    closedir(directory);
    return count;
}

static void test_repeated_completion_requests(Shell *shell, int wake_fd)
{
    uint64_t first_id, second_id;
    Result events = {0};
    int first_seen = 0, second_seen = 0;
    assert(shell_request_completion(shell, "uni", &first_id) == 0);
    assert(shell_request_completion(shell, "uni", &second_id) == 0);
    assert(first_id != second_id);
    for (int attempt = 0; attempt < 2000 && (!first_seen || !second_seen); ++attempt) {
        struct pollfd fd = { .fd = wake_fd, .events = POLLIN };
        collect_event(shell, &events);
        for (size_t i = 0; i < events.completion_count; ++i) {
            CompletionResult *completion = &events.completions[i];
            if (completion->id == first_id) {
                first_seen = 1;
                assert(completion->status == 0 &&
                       has_match(completion, "unique-file.txt"));
            } else if (completion->id == second_id) {
                second_seen = 1;
                assert(completion->status == 0 &&
                       has_match(completion, "unique-file.txt"));
            }
        }
        if (!first_seen || !second_seen) {
            (void)poll(&fd, 1, 10);
            if (fd.revents & POLLIN) {
                unsigned char bytes[256];
                while (read(wake_fd, bytes, sizeof(bytes)) > 0) { }
            }
        }
    }
    assert(first_seen && second_seen);
    dispose_result(&events);
}

static void test_argument_selection(void)
{
    char *default_argv[] = { (char *)"terminator", NULL };
    char *process_argv[] = { (char *)"terminator", (char *)"useProcess=True", NULL };
    char *unsupported_argv[] = { (char *)"terminator", (char *)"useProcess=false", NULL };
    int process = -1;
    assert(process_runtime_parse_args(1, default_argv, &process) == 0 && process == 0);
    assert(process_runtime_parse_args(2, process_argv, &process) == 0 && process == 1);
    assert(process_runtime_parse_args(2, unsupported_argv, &process) < 0);
    assert(process_runtime_parse_args(3, process_argv, &process) < 0);
}

int main(int argc, char **argv)
{
    int wake_pipe[2], sentinel_pipe[2], unrelated_pipe[2];
    int x_fd, unrelated_fd;
    char dir_a[] = "/tmp/terminator-process-a-XXXXXX";
    char dir_b[] = "/tmp/terminator-process-b-XXXXXX";
    char history_home[] = "/tmp/terminator-process-history-XXXXXX";
    char nested_dir[1024];
    char command[256];
    char exec_fixture[1024], nonexec_fixture[1024], exec_output[1024];
    char process_path[4096];
    char *original_path = getenv("PATH") == NULL ? NULL : strdup(getenv("PATH"));
    Shell *first, *second, *crashed;
    Result result;
    (void)argc;
    if (process_runtime_is_child_invocation(argc, argv))
        return process_runtime_child_entry(argc, argv);
    test_argument_selection();
    assert(mkdtemp(history_home) != NULL);
    assert(setenv("HOME", history_home, 1) == 0);
    History *shared_history = history_open_process_shared(NULL);
    assert(shared_history != NULL);
    assert(history_add(shared_history, "shared-parent-entry") == 0);
    assert(history_add(shared_history, "shared first line\nshared second line") == 0);
    HistoryCursor shared_cursor = {0};
    char *shared_entry = history_cursor_previous(shared_history, &shared_cursor,
                                                  "draft");
    assert(shared_entry != NULL &&
           strcmp(shared_entry, "shared first line\nshared second line") == 0);
    free(shared_entry);
    shared_entry = history_cursor_next(shared_history, &shared_cursor);
    assert(shared_entry != NULL && strcmp(shared_entry, "draft") == 0);
    free(shared_entry);
    char **shared_matches = NULL;
    size_t shared_match_count = 0;
    assert(history_search(shared_history, "shared-parent-entry",
                          &shared_matches, &shared_match_count) == 0);
    assert(shared_match_count == 1 &&
           strcmp(shared_matches[0], "shared-parent-entry") == 0);
    free(shared_matches[0]); free(shared_matches);
    assert(pipe(wake_pipe) == 0);
    assert(fcntl(wake_pipe[0], F_SETFL,
                 fcntl(wake_pipe[0], F_GETFL) | O_NONBLOCK) == 0);
    assert(pipe(sentinel_pipe) == 0);
    assert(pipe(unrelated_pipe) == 0);
    x_fd = fcntl(sentinel_pipe[0], F_DUPFD_CLOEXEC, 400);
    unrelated_fd = fcntl(unrelated_pipe[0], F_DUPFD_CLOEXEC, 401);
    assert(x_fd >= 400 && unrelated_fd >= 401);
    assert(mkdtemp(dir_a) != NULL && mkdtemp(dir_b) != NULL);
    assert(snprintf(exec_fixture, sizeof(exec_fixture),
                    "%s/terminator-execvp-fixture", dir_a) > 0);
    assert(snprintf(nonexec_fixture, sizeof(nonexec_fixture),
                    "%s/terminator-nonexec-fixture", dir_a) > 0);
    assert(snprintf(exec_output, sizeof(exec_output), "%s/execvp-output.txt",
                    dir_a) > 0);
    assert(symlink("/bin/echo", exec_fixture) == 0);
    create_nonexec_fixture(nonexec_fixture);
    int process_path_length = snprintf(process_path, sizeof(process_path), "%s:%s",
        dir_a, original_path == NULL ? "/usr/bin:/bin" : original_path);
    assert(process_path_length > 0 &&
           (size_t)process_path_length < sizeof(process_path));
    assert(setenv("PATH", process_path, 1) == 0);
    create_file(dir_a, "unique-file.txt");
    create_file(dir_a, "apple-one.txt");
    create_file(dir_a, "apple-two.txt");
    create_file(dir_a, "bravo.txt");
    create_file(dir_a, "brick.txt");
    create_file(dir_a, "space name.txt");
    assert(snprintf(nested_dir, sizeof(nested_dir), "%s/sub dir", dir_a) > 0);
    assert(mkdir(nested_dir, 0700) == 0);
    create_file(nested_dir, "nested-alpha");
    create_file(nested_dir, "nested-beta");
    create_file(dir_b, "other-tab-only");

    first = shell_create_process(wake_pipe[1], x_fd, 11);
    assert(first != NULL && process_runtime_child_pid(first->process) > 0);
    result = run_command(first, wake_pipe[0], "printf process-started");
    assert(result.status == 0 && output_contains(&result, "process-started"));
    dispose_result(&result);
    size_t before_child_history = history_count(shared_history);
    result = run_command(first, wake_pipe[0], "history");
    assert(output_contains(&result, "shared-parent-entry") &&
           output_contains(&result, "shared second line"));
    assert(history_count(shared_history) == before_child_history);
    dispose_result(&result);

    result = run_command(first, wake_pipe[0], "printf first-command");
    assert(result.status == 0 && output_contains(&result, "first-command"));
    dispose_result(&result);
    result = run_command(first, wake_pipe[0], "printf second-command");
    assert(result.status == 0 && output_contains(&result, "second-command"));
    dispose_result(&result);

    assert(snprintf(command, sizeof(command), "cd %s", dir_a) > 0);
    result = run_command(first, wake_pipe[0], command);
    assert(result.status == 0);
    dispose_result(&result);

    result = run_command(first, wake_pipe[0],
                         "terminator-execvp-fixture execvp-fixture-ok");
    assert(result.status == 0 && output_contains(&result, "execvp-fixture-ok"));
    dispose_result(&result);
    result = run_command(first, wake_pipe[0],
                         "./terminator-execvp-fixture execvp-fixture-ok");
    assert(result.status == 0 && output_contains(&result, "execvp-fixture-ok"));
    dispose_result(&result);
    assert(snprintf(command, sizeof(command), "%s execvp-fixture-ok",
                    exec_fixture) > 0);
    result = run_command(first, wake_pipe[0], command);
    assert(result.status == 0 && output_contains(&result, "execvp-fixture-ok"));
    dispose_result(&result);
    result = run_command(first, wake_pipe[0], "./terminator-nonexec-fixture");
    assert(result.status == 126);
    dispose_result(&result);
    result = run_command(first, wake_pipe[0],
                         "terminator-execvp-fixture execvp-fixture-ok | cat > execvp-output.txt");
    assert(result.status == 0);
    dispose_result(&result);
    char exec_output_contents[128];
    read_small_file(exec_output, exec_output_contents, sizeof(exec_output_contents));
    assert(strcmp(exec_output_contents, "execvp-fixture-ok\n") == 0);

    char process_script[1024];
    assert(snprintf(process_script, sizeof(process_script),
                    "%s/process-script.sh", dir_a) > 0);
    int process_script_fd = open(process_script,
                                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                                 0700);
    assert(process_script_fd >= 0);
    static const char process_script_text[] =
        "if [ \"alpha\" = \"alpha\" ]; then echo process-script-equal; fi\n";
    assert(write(process_script_fd, process_script_text,
                 sizeof(process_script_text) - 1) ==
           (ssize_t)(sizeof(process_script_text) - 1));
    assert(close(process_script_fd) == 0);
    result = run_command(first, wake_pipe[0], "bash process-script.sh");
    assert(result.status == 0 && output_contains(&result, "process-script-equal"));
    dispose_result(&result);

    /* Unique and common-prefix results reuse the thread-mode edit semantics. */
    {
        CompletionResult completion = request_completion(first, wake_pipe[0], "uni");
        char *input = strdup("cat uni");
        size_t capacity = strlen(input) + 1, length = strlen(input), cursor = length;
        CompletionSelection selection;
        assert(completion.status == 0 && completion.count == 1 &&
               has_match(&completion, "unique-file.txt"));
        completion_selection_init(&selection);
        assert(completion_apply_matches(&selection, &completion.matches,
                    &completion.count, "uni", 4, &input, &capacity,
                    &length, &cursor) == COMPLETION_APPLY_INSERTED);
        assert(strcmp(input, "cat unique-file.txt") == 0);
        completion_selection_clear(&selection);
        free(input);
        free_completion_result(&completion);

        completion = request_completion(first, wake_pipe[0], "app");
        input = strdup("cat app");
        capacity = strlen(input) + 1; length = strlen(input); cursor = length;
        assert(completion.status == 0 && completion.count == 2);
        assert(completion_apply_matches(&selection, &completion.matches,
                    &completion.count, "app", 4, &input, &capacity,
                    &length, &cursor) == COMPLETION_APPLY_INSERTED);
        assert(strcmp(input, "cat apple-") == 0);
        free(input);
        free_completion_result(&completion);
    }
    test_repeated_completion_requests(first, wake_pipe[0]);
    {
        CompletionResult completion = request_completion(
            first, wake_pipe[0], "no-such-directory-terminator/x");
        assert(completion.status != 0 && completion.count == 0);
        free_completion_result(&completion);
    }
    result = run_command(first, wake_pipe[0], "terminator-command-does-not-exist");
    assert(result.status == 127 && result.bytes != NULL);
    dispose_result(&result);

    /* Numbered candidate choice, invalid input, Escape cancellation, and paths. */
    {
        CompletionSelection selection;
        size_t chosen = 0, capacity, length, cursor;
        char *input;
        CompletionResult completion = request_completion(first, wake_pipe[0], "br");
        assert(completion.status == 0 && completion.count == 2);
        size_t brick_index = 0;
        for (size_t i = 0; i < completion.count; ++i)
            if (strcmp(completion.matches[i], "brick.txt") == 0)
                brick_index = i;
        input = strdup("cat br"); capacity = strlen(input) + 1;
        length = strlen(input); cursor = length;
        completion_selection_init(&selection);
        assert(completion_apply_matches(&selection, &completion.matches,
                    &completion.count, "br", 4, &input, &capacity,
                    &length, &cursor) == COMPLETION_APPLY_SELECTING);
        assert(completion_selection_key(&selection, (int)('1' + brick_index), &chosen) ==
               COMPLETION_SELECTION_CHOSEN);
        assert(completion_selection_apply(&selection, chosen, &input,
                    &capacity, &length, &cursor) == 0);
        assert(strcmp(input, "cat brick.txt") == 0);
        completion_selection_clear(&selection);
        free(input); free_completion_result(&completion);

        completion = request_completion(first, wake_pipe[0], "br");
        input = strdup("cat br"); capacity = strlen(input) + 1;
        length = strlen(input); cursor = length;
        assert(completion_apply_matches(&selection, &completion.matches,
                    &completion.count, "br", 4, &input, &capacity,
                    &length, &cursor) == COMPLETION_APPLY_SELECTING);
        assert(completion_selection_key(&selection, '9', &chosen) ==
               COMPLETION_SELECTION_INVALID);
        completion_selection_clear(&selection);
        free(input); free_completion_result(&completion);

        completion = request_completion(first, wake_pipe[0], "br");
        input = strdup("cat br"); capacity = strlen(input) + 1;
        length = strlen(input); cursor = length;
        assert(completion_apply_matches(&selection, &completion.matches,
                    &completion.count, "br", 4, &input, &capacity,
                    &length, &cursor) == COMPLETION_APPLY_SELECTING);
        assert(completion_selection_key(&selection, 27, &chosen) ==
               COMPLETION_SELECTION_CANCELLED);
        completion_selection_clear(&selection);
        free(input); free_completion_result(&completion);

        completion = request_completion(first, wake_pipe[0], "sub dir/n");
        input = strdup("cat sub\\ dir/n"); capacity = strlen(input) + 1;
        length = strlen(input); cursor = length;
        assert(completion.status == 0 && completion.count == 2);
        assert(completion_apply_matches(&selection, &completion.matches,
                    &completion.count, "sub dir/n", 4, &input, &capacity,
                    &length, &cursor) == COMPLETION_APPLY_INSERTED);
        assert(strcmp(input, "cat sub\\ dir/nested-") == 0);
        free(input); free_completion_result(&completion);

        completion = request_completion(first, wake_pipe[0], "space");
        input = strdup("cat space"); capacity = strlen(input) + 1;
        length = strlen(input); cursor = length;
        assert(completion.status == 0 && completion.count == 1 &&
               has_match(&completion, "space name.txt"));
        assert(completion_apply_matches(&selection, &completion.matches,
                    &completion.count, "space", 4, &input, &capacity,
                    &length, &cursor) == COMPLETION_APPLY_INSERTED);
        assert(strcmp(input, "cat space\\ name.txt") == 0);
        free(input); free_completion_result(&completion);
        completion_selection_clear(&selection);
    }

    second = shell_create_process(wake_pipe[1], x_fd, 12);
    assert(second != NULL && process_runtime_child_pid(second->process) > 0);
    assert(history_add(shared_history, "added-after-second-tab") == 0);
    result = run_command(second, wake_pipe[0], "history");
    assert(output_contains(&result, "added-after-second-tab"));
    dispose_result(&result);
    result = run_command(first, wake_pipe[0], "pwd");
    assert(result.status == 0 && output_contains(&result, dir_a));
    dispose_result(&result);
    assert(snprintf(command, sizeof(command), "cd %s", dir_b) > 0);
    result = run_command(second, wake_pipe[0], command);
    assert(result.status == 0);
    dispose_result(&result);
    {
        CompletionResult completion = request_completion(first, wake_pipe[0], "oth");
        assert(completion.status == 0 && completion.count == 0);
        free_completion_result(&completion);
        completion = request_completion(second, wake_pipe[0], "oth");
        assert(completion.status == 0 && completion.count == 1 &&
               has_match(&completion, "other-tab-only"));
        free_completion_result(&completion);
    }
    result = run_command(first, wake_pipe[0], "pwd");
    assert(result.status == 0 && output_contains(&result, dir_a));
    dispose_result(&result);
    result = run_command(second, wake_pipe[0], "pwd");
    assert(result.status == 0 && output_contains(&result, dir_b));
    dispose_result(&result);

    /* multiWatch runs through the same process-mode shell state and signal
     * path as foreground and background jobs. */
    {
        Result watched = {0};
        assert(shell_submit(first, "multiWatch [\"printf process-watch\"]") == 0);
        delay_ms(250);
        assert(shell_signal_foreground(first, 0) == 0);
        wait_events(first, wake_pipe[0], &watched, 1, 0);
        assert(output_contains(&watched, "process-watch"));
        assert(output_contains(&watched, "----------------------------------------------------"));
        dispose_result(&watched);
    }

    /* The parent directs controls only at its child; the child selects its own PGID. */
    {
        Shell *controls = shell_create_process(wake_pipe[1], x_fd, 15);
        Result interrupted = {0};
        assert(controls != NULL);
        submit_and_allow_start(controls, "sleep 10");
        assert(shell_signal_foreground(controls, 0) == 0);
        wait_events(controls, wake_pipe[0], &interrupted, 1, 0);
        assert(interrupted.status == 128 + SIGINT);
        dispose_result(&interrupted);

        Result stopped = {0};
        submit_and_allow_start(controls, "sleep 10");
        assert(shell_signal_foreground(controls, 1) == 0);
        wait_events(controls, wake_pipe[0], &stopped, 1, 0);
        assert(stopped.status == 128 + SIGTSTP);
        assert(stopped.job_updates != 0 && stopped.last_job_state == 1 &&
               stopped.last_job_foreground == 0);
        dispose_result(&stopped);
        result = run_command(controls, wake_pipe[0], "jobs");
        assert(output_contains(&result, "Stopped sleep 10"));
        dispose_result(&result);
        result = run_command(controls, wake_pipe[0], "bg");
        assert(result.status == 0);
        assert(result.job_updates != 0 && result.last_job_state == 0);
        dispose_result(&result);
        result = run_command(controls, wake_pipe[0], "jobs");
        assert(output_contains(&result, "Running sleep 10"));
        dispose_result(&result);
        submit_and_allow_start(controls, "fg");
        delay_ms(100);
        assert(shell_signal_foreground(controls, 0) == 0);
        result = (Result){0};
        wait_events(controls, wake_pipe[0], &result, 1, 0);
        assert(result.status == 128 + SIGINT);
        dispose_result(&result);
        shell_destroy(controls);
    }

    /* A signal request never targets another tab's foreground group. */
    {
        Shell *left = shell_create_process(wake_pipe[1], x_fd, 16);
        Shell *right = shell_create_process(wake_pipe[1], x_fd, 17);
        Result left_result = {0}, right_result = {0};
        assert(left != NULL && right != NULL);
        submit_and_allow_start(left, "sleep 10");
        submit_and_allow_start(right, "sleep 10");
        assert(shell_signal_foreground(left, 0) == 0);
        wait_events(left, wake_pipe[0], &left_result, 1, 0);
        assert(left_result.status == 128 + SIGINT);
        assert(shell_signal_foreground(right, 0) == 0);
        wait_events(right, wake_pipe[0], &right_result, 1, 0);
        assert(right_result.status == 128 + SIGINT);
        dispose_result(&left_result);
        dispose_result(&right_result);
        shell_destroy(left);
        shell_destroy(right);
    }

    /* Closing with foreground or background work kills only that tab's jobs. */
    {
      size_t descriptors_before = open_fd_count();
      for (int cycle = 0; cycle < 12; ++cycle) {
        Shell *closing = shell_create_process(wake_pipe[1], x_fd,
                                              (uint32_t)(30 + cycle));
        pid_t child;
        pid_t command_child;
        assert(closing != NULL);
        child = process_runtime_child_pid(closing->process);
        if ((cycle & 1) == 0)
            submit_and_allow_start(closing, "sleep 30");
        else {
            result = run_command(closing, wake_pipe[0], "sleep 30 &");
            dispose_result(&result);
        }
        command_child = first_child_pid(child);
        shell_destroy(closing);
        errno = 0;
        assert(kill(child, 0) < 0 && errno == ESRCH);
        errno = 0;
        assert(kill(command_child, 0) < 0 && errno == ESRCH);
      }
      assert(open_fd_count() == descriptors_before);
    }

    /* Completion racing a signal and close must leave the supervisor reaped. */
    {
        Shell *racing = shell_create_process(wake_pipe[1], x_fd, 50);
        pid_t child;
        assert(racing != NULL);
        child = process_runtime_child_pid(racing->process);
        submit_and_allow_start(racing, "sleep 0.15");
        (void)shell_signal_foreground(racing, 0);
        shell_destroy(racing);
        errno = 0;
        assert(kill(child, 0) < 0 && errno == ESRCH);
    }

    /* A child that cannot service graceful shutdown is force-killed and reaped. */
    {
        Shell *forced = shell_create_process(wake_pipe[1], x_fd, 51);
        pid_t child;
        assert(forced != NULL);
        child = process_runtime_child_pid(forced->process);
        delay_ms(100);
        assert(kill(child, SIGSTOP) == 0);
        shell_destroy(forced);
        errno = 0;
        assert(kill(child, 0) < 0 && errno == ESRCH);
    }

    result = run_command(first, wake_pipe[0], "test ! -e /proc/self/fd/400");
    assert(result.status == 0);
    dispose_result(&result);
    result = run_command(first, wake_pipe[0], "test ! -e /proc/self/fd/401");
    assert(result.status == 0);
    dispose_result(&result);

    result = run_command(first, wake_pipe[0], "yes process | head -c 750000");
    assert(result.status == 0 && result.length >= 750000);
    for (size_t i = 0; i < 750000; ++i) {
        if (result.bytes[i] != 'p' && result.bytes[i] != 'r' &&
            result.bytes[i] != 'o' && result.bytes[i] != 'c' &&
            result.bytes[i] != 'e' && result.bytes[i] != 's' &&
            result.bytes[i] != '\n') {
            fprintf(stderr, "unexpected pipeline output byte at %zu: 0x%02x (%.*s)\n",
                    i, (unsigned char)result.bytes[i], 80,
                    result.bytes + (i > 40 ? i - 40 : 0));
            assert(!"unexpected large pipeline output");
        }
    }
    dispose_result(&result);

    result = run_command(first, wake_pipe[0], "exit");
    wait_events(first, wake_pipe[0], &result, 0, 1);
    assert(result.done && result.exited);
    dispose_result(&result);
    shell_destroy(first);

    crashed = shell_create_process(wake_pipe[1], x_fd, 13);
    assert(crashed != NULL);
    assert(kill(process_runtime_child_pid(crashed->process), SIGKILL) == 0);
    result = (Result){0};
    wait_events(crashed, wake_pipe[0], &result, 0, 1);
    assert(result.exited);
    dispose_result(&result);
    shell_destroy(crashed);

    /* A supervisor crash while a command is active must terminate its
     * process-mode command child as well. */
    crashed = shell_create_process(wake_pipe[1], x_fd, 52);
    assert(crashed != NULL);
    pid_t supervisor = process_runtime_child_pid(crashed->process);
    submit_and_allow_start(crashed, "sleep 30");
    pid_t command_child = first_child_pid(supervisor);
    assert(process_is_executing(command_child));
    assert(kill(supervisor, SIGKILL) == 0);
    result = (Result){0};
    wait_events(crashed, wake_pipe[0], &result, 0, 1);
    dispose_result(&result);
    for (int attempt = 0; attempt < 100 && process_is_executing(command_child); ++attempt)
        delay_ms(10);
    assert(!process_is_executing(command_child));
    shell_destroy(crashed);

    /* Closing a tab with a response outstanding frees the response safely. */
    {
        Shell *pending = shell_create_process(wake_pipe[1], x_fd, 14);
        uint64_t pending_id;
        assert(pending != NULL);
        assert(shell_request_completion(pending, "", &pending_id) == 0);
        assert(pending_id != 0);
        shell_destroy(pending);
    }

    result = run_command(second, wake_pipe[0], "printf surviving-tab");
    assert(result.status == 0 && output_contains(&result, "surviving-tab"));
    dispose_result(&result);
    {
        pid_t child_pid = process_runtime_child_pid(second->process);
        shell_destroy(second); /* idle graceful shutdown and independent reap */
        errno = 0;
        assert(kill(child_pid, 0) < 0 && errno == ESRCH);
    }

    close(wake_pipe[0]); close(wake_pipe[1]);
    close(sentinel_pipe[0]); close(sentinel_pipe[1]); close(x_fd);
    close(unrelated_pipe[0]); close(unrelated_pipe[1]); close(unrelated_fd);
    remove_file(dir_a, "unique-file.txt");
    remove_file(dir_a, "apple-one.txt");
    remove_file(dir_a, "apple-two.txt");
    remove_file(dir_a, "bravo.txt");
    remove_file(dir_a, "brick.txt");
    remove_file(dir_a, "space name.txt");
    assert(unlink(exec_fixture) == 0);
    assert(unlink(nonexec_fixture) == 0);
    assert(unlink(exec_output) == 0);
    assert(unlink(process_script) == 0);
    remove_file(nested_dir, "nested-alpha");
    remove_file(nested_dir, "nested-beta");
    remove_file(dir_b, "other-tab-only");
    assert(rmdir(nested_dir) == 0);
    assert(rmdir(dir_a) == 0 && rmdir(dir_b) == 0);
    char shared_history_path[1024], shared_lock_path[1030];
    assert(snprintf(shared_history_path, sizeof(shared_history_path),
                    "%s/.terminator_history", history_home) > 0);
    assert(snprintf(shared_lock_path, sizeof(shared_lock_path), "%s.lock",
                    shared_history_path) > 0);
    assert(history_test_unlink_segment(shared_history) == 0);
    history_close(shared_history);
    assert(unlink(shared_history_path) == 0);
    assert(unlink(shared_lock_path) == 0);
    assert(rmdir(history_home) == 0);
    if (original_path != NULL)
        assert(setenv("PATH", original_path, 1) == 0);
    else
        assert(unsetenv("PATH") == 0);
    free(original_path);
    puts("process runtime tests passed");
    return 0;
}
