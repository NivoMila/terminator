#define _POSIX_C_SOURCE 200809L

#include "history.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    TEST_FORCE_RETIREMENT = 17,
    TEST_FORCE_RETIREMENT_UNLINK_FAILURE = 18
};

typedef struct {
    History *history;
    unsigned worker;
    unsigned count;
    int failed;
} Writer;

static void *thread_writer(void *argument)
{
    Writer *writer = argument;
    for (unsigned i = 0; i < writer->count; ++i) {
        char command[80];
        (void)snprintf(command, sizeof(command), "thread-%u-entry-%u",
                       writer->worker, i);
        if (history_add(writer->history, command) < 0) {
            writer->failed = errno == 0 ? EIO : errno;
            break;
        }
    }
    return NULL;
}

static pid_t start_process_writer(const char *path, unsigned worker)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        History *history = history_open_process_shared(path);
        if (history == NULL)
            _exit(10);
        for (unsigned i = 0; i < 25; ++i) {
            char command[80];
            (void)snprintf(command, sizeof(command), "process-%u-entry-%u",
                           worker, i);
            if (history_add(history, command) < 0)
                _exit(11);
        }
        history_close(history);
        _exit(0);
    }
    return child;
}

static pid_t start_first_attach_writer(const char *path, unsigned worker,
                                       int gate_fd)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        char token;
        if (read(gate_fd, &token, 1) != 1)
            _exit(12);
        History *history = history_open_process_shared(path);
        if (history == NULL)
            _exit(13);
        for (unsigned i = 0; i < 25; ++i) {
            char command[80];
            (void)snprintf(command, sizeof(command), "process-%u-entry-%u",
                           worker, i);
            if (history_add(history, command) < 0)
                _exit(14);
        }
        history_close(history);
        _exit(0);
    }
    return child;
}

static int contains_entry(History *history, const char *needle)
{
    char **items = NULL;
    size_t count = 0, first = 0;
    int found = 0;
    assert(history_recent(history, 10000, &items, &count, &first) == 0);
    (void)first;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(items[i], needle) == 0)
            found = 1;
        free(items[i]);
    }
    free(items);
    return found;
}

static void wait_success(pid_t child)
{
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void wait_success_timeout(pid_t child)
{
    int status;
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            return;
        }
        assert(result == 0 || (result < 0 && errno == EINTR));
        (void)poll(NULL, 0, 10);
    }
    assert(!"timed out waiting for child process");
}

static void wait_stopped_timeout(pid_t child)
{
    int status;
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        pid_t result = waitpid(child, &status, WNOHANG | WUNTRACED);
        if (result == child) {
            assert(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
            return;
        }
        assert(result == 0 || (result < 0 && errno == EINTR));
        (void)poll(NULL, 0, 10);
    }
    assert(!"timed out waiting for retirement failpoint");
}

static void crash_append(const char *path, int failpoint, const char *command,
                         int expected_exit)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        History *history = history_open_process_shared(path);
        if (history == NULL)
            _exit(20);
        (void)history_test_set_failpoint(history, failpoint);
        (void)history_add(history, command);
        _exit(21);
    }
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    assert(WIFEXITED(status) && WEXITSTATUS(status) == expected_exit);
}

static void crash_during_open_recovery(const char *path, int failpoint)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        history_test_set_next_open_failpoint(failpoint);
        History *history = history_open_process_shared(path);
        if (history == NULL)
            _exit(30);
        _exit(31);
    }
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 80 + failpoint);
}

static void cleanup_history_path(History *history, const char *path)
{
    assert(history_test_unlink_segment(history) == 0);
    history_close(history);
    char lock_path[512];
    (void)snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    assert(unlink(lock_path) == 0);
    assert(unlink(path) == 0);
}

static void test_successful_segment_retirement_remaps_old_handle(void)
{
    char path[] = "/tmp/terminator-history-retire-ok-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0);
    History *older_handle = history_open_process_shared(path);
    assert(older_handle != NULL);
    assert(history_add(older_handle, "before-retirement") == 0);

    pid_t retire = fork();
    assert(retire >= 0);
    if (retire == 0) {
        History *history = history_open_process_shared(path);
        if (history == NULL)
            _exit(20);
        (void)history_test_set_failpoint(history, TEST_FORCE_RETIREMENT);
        if (history_count(history) != 0)
            _exit(21);
        history_close(history);
        _exit(0);
    }
    wait_success(retire);

    /* This handle still maps the unlinked segment. Its next operation must
     * compare the named object's inode and rebuild from the durable file. */
    assert(history_add(older_handle, "after-retirement") == 0);
    assert(history_count(older_handle) == 2);
    assert(contains_entry(older_handle, "before-retirement"));
    assert(contains_entry(older_handle, "after-retirement"));
    cleanup_history_path(older_handle, path);
}

static void test_failed_retirement_fail_stops_and_recovers(void)
{
    char path[] = "/tmp/terminator-history-retire-fail-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0);
    History *older_handle = history_open_process_shared(path);
    assert(older_handle != NULL);
    assert(history_add(older_handle, "before-failed-retirement") == 0);

    pid_t retire = fork();
    assert(retire >= 0);
    if (retire == 0) {
        History *history = history_open_process_shared(path);
        if (history == NULL)
            _exit(20);
        (void)history_test_set_failpoint(
            history, TEST_FORCE_RETIREMENT_UNLINK_FAILURE);
        (void)history_count(history);
        _exit(21);
    }
    int status;
    wait_stopped_timeout(retire);

    /* The retiring process is stopped while owning flock and the robust
     * mutex. Confirm a second process reaches a contended flock before letting
     * the injected unlink failure fail-stop the owner. */
    int notify_pipe[2];
    assert(pipe(notify_pipe) == 0);
    pid_t contender = fork();
    assert(contender >= 0);
    if (contender == 0) {
        close(notify_pipe[0]);
        char fd_text[32];
        (void)snprintf(fd_text, sizeof(fd_text), "%d", notify_pipe[1]);
        if (setenv("TERMINATOR_HISTORY_TEST_LOCK_NOTIFY_FD", fd_text, 1) < 0)
            _exit(30);
        History *history = history_open_process_shared(path);
        if (history == NULL)
            _exit(31);
        (void)unsetenv("TERMINATOR_HISTORY_TEST_LOCK_NOTIFY_FD");
        close(notify_pipe[1]);
        if (history_add(history, "after-failed-retirement") < 0)
            _exit(32);
        history_close(history);
        _exit(0);
    }
    close(notify_pipe[1]);
    struct pollfd notification = {.fd = notify_pipe[0], .events = POLLIN};
    assert(poll(&notification, 1, 5000) == 1 &&
           (notification.revents & POLLIN) != 0);
    char byte;
    assert(read(notify_pipe[0], &byte, 1) == 1 && byte == 'b');
    close(notify_pipe[0]);

    assert(kill(retire, SIGCONT) == 0);
    while (waitpid(retire, &status, 0) < 0 && errno == EINTR) { }
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 125);
    wait_success_timeout(contender);

    /* The contender recovers the owner-dead mutex from durable history; the
     * older attached handle remains safe and sees the recovered segment. */
    assert(history_count(older_handle) == 2);
    assert(contains_entry(older_handle, "before-failed-retirement"));
    assert(contains_entry(older_handle, "after-failed-retirement"));
    cleanup_history_path(older_handle, path);
}

static void test_invalid_multiline_record(const char *bad_record)
{
    static const char valid_record[] = "\036M66697273740A7365636F6E64\n";
    char path[] = "/tmp/terminator-history-invalid-XXXXXX";
    char expected[sizeof(valid_record) + 32];
    char actual[sizeof(expected)];
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, valid_record, sizeof(valid_record) - 1) ==
           (ssize_t)(sizeof(valid_record) - 1));
    assert(close(fd) == 0);

    History *history = history_open_process_shared(path);
    assert(history != NULL);
    char *entry = history_get(history, 0);
    assert(entry != NULL && strcmp(entry, "first\nsecond") == 0);
    free(entry);

    fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    assert(fd >= 0);
    size_t bad_length = strlen(bad_record);
    assert(write(fd, bad_record, bad_length) == (ssize_t)bad_length);
    assert(close(fd) == 0);

    memcpy(expected, valid_record, sizeof(valid_record) - 1);
    memcpy(expected + sizeof(valid_record) - 1, bad_record, bad_length);
    size_t expected_length = sizeof(valid_record) - 1 + bad_length;
    errno = 0;
    assert(history_add(history, "must-not-rewrite-incomplete-history") < 0);
    assert(errno == EILSEQ);

    fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    ssize_t actual_length = read(fd, actual, sizeof(actual));
    assert(actual_length == (ssize_t)expected_length);
    assert(memcmp(actual, expected, expected_length) == 0);
    assert(close(fd) == 0);

    assert(history_test_unlink_segment(history) == 0);
    history_close(history);
    char lock_path[sizeof(path) + sizeof(".lock")];
    (void)snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    assert(unlink(lock_path) == 0);
    assert(unlink(path) == 0);
}

int main(void)
{
    test_invalid_multiline_record("\036M6G\n");
    test_invalid_multiline_record("\036M610062\n");
    test_invalid_multiline_record("unterminated-final-record");

    char path[] = "/tmp/terminator-history-shared-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    History *shared = history_open_process_shared(path);
    History *second = history_open_process_shared(path);
    History *thread_mode = history_open(path);
    char *entry;
    assert(shared != NULL && second != NULL && thread_mode != NULL);

    assert(history_add(shared, "alpha command") == 0);
    assert(history_add(shared, "first physical line\nsecond physical line") == 0);
    entry = history_get(second, 1);
    assert(entry != NULL &&
           strcmp(entry, "first physical line\nsecond physical line") == 0);
    free(entry);
    assert(history_add(thread_mode, "thread-mode-visible") == 0);
    assert(contains_entry(second, "thread-mode-visible"));

    HistoryCursor cursor = {0};
    entry = history_cursor_previous(second, &cursor, "draft text");
    assert(entry != NULL && strcmp(entry, "thread-mode-visible") == 0);
    free(entry);
    entry = history_cursor_next(second, &cursor);
    assert(entry != NULL && strcmp(entry, "draft text") == 0);
    free(entry);
    char **matches = NULL;
    size_t match_count = 0;
    assert(history_search(second, "alpha command", &matches, &match_count) == 0);
    assert(match_count == 1 && strcmp(matches[0], "alpha command") == 0);
    free(matches[0]); free(matches);

    /* Two independent shared-memory writers race with two thread-mode
     * application instances on the same durable file and stable flock. */
    pid_t process_a = start_process_writer(path, 1);
    pid_t process_b = start_process_writer(path, 2);
    pthread_t threads[2];
    Writer writers[2] = {
        {.history = shared, .worker = 3, .count = 20},
        {.history = thread_mode, .worker = 4, .count = 20}
    };
    for (size_t i = 0; i < 2; ++i)
        assert(pthread_create(&threads[i], NULL, thread_writer, &writers[i]) == 0);
    for (size_t i = 0; i < 2; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(writers[i].failed == 0);
    }
    wait_success(process_a);
    wait_success(process_b);
    for (unsigned i = 0; i < 25; ++i) {
        char command[80];
        (void)snprintf(command, sizeof(command), "process-1-entry-%u", i);
        assert(contains_entry(second, command));
        (void)snprintf(command, sizeof(command), "process-2-entry-%u", i);
        assert(contains_entry(second, command));
        if (i < 20) {
            (void)snprintf(command, sizeof(command), "thread-3-entry-%u", i);
            assert(contains_entry(second, command));
            (void)snprintf(command, sizeof(command), "thread-4-entry-%u", i);
            assert(contains_entry(second, command));
        }
    }
    size_t before_crash = history_count(second);
    assert(before_crash == 93);

    /* A dead owner leaves the robust mutex recoverable from the file. */
    pid_t owner = fork();
    assert(owner >= 0);
    if (owner == 0) {
        History *child_history = history_open_process_shared(path);
        if (child_history == NULL)
            _exit(30);
        (void)history_test_die_holding_lock(child_history);
        _exit(31);
    }
    int owner_status;
    while (waitpid(owner, &owner_status, 0) < 0 && errno == EINTR) { }
    assert(WIFEXITED(owner_status) && WEXITSTATUS(owner_status) == 79);
    assert(history_count(second) == before_crash);
    assert(history_add(second, "after-owner-death") == 0);

    /* Rebuild malformed offsets and an incompatible segment version solely
     * from the durable file; neither corruption can overwrite valid entries. */
    assert(history_test_corrupt_segment(second, 0) == 0);
    assert(contains_entry(shared, "after-owner-death"));
    assert(history_test_corrupt_segment(second, 1) == 0);
    assert(contains_entry(shared, "alpha command"));

    /* Failure before rename leaves the durable file unchanged. */
    size_t before_failed_persist = history_count(shared);
    const int failpoints[] = {HISTORY_TEST_FAIL_WRITE, HISTORY_TEST_FAIL_FSYNC,
                              HISTORY_TEST_FAIL_RENAME};
    for (size_t i = 0; i < sizeof(failpoints) / sizeof(failpoints[0]); ++i) {
        assert(history_test_set_failpoint(shared, failpoints[i]) == 0);
        errno = 0;
        assert(history_add(shared, "must-not-be-committed") < 0 && errno == EIO);
        assert(history_count(second) == before_failed_persist);
        assert(!contains_entry(second, "must-not-be-committed"));
    }
    assert(history_test_set_failpoint(shared, 0) == 0);
    assert(history_test_set_failpoint(shared, HISTORY_TEST_FAIL_DIR_FSYNC) == 0);
    errno = 0;
    assert(history_add(shared, "directory-sync-uncertain") < 0 && errno == EIO);
    assert(contains_entry(second, "directory-sync-uncertain"));
    assert(history_test_set_failpoint(shared, 0) == 0);
    before_failed_persist = history_count(second);

    crash_append(path, HISTORY_TEST_CRASH_AFTER_STAGE_WRITE,
                 "crash-before-fsync", 84);
    assert(history_count(second) == before_failed_persist);
    assert(!contains_entry(second, "crash-before-fsync"));
    crash_append(path, HISTORY_TEST_CRASH_AFTER_STAGE_FSYNC,
                 "crash-before-rename", 85);
    assert(history_count(second) == before_failed_persist);
    assert(!contains_entry(second, "crash-before-rename"));
    crash_append(path, HISTORY_TEST_CRASH_AFTER_RENAME,
                 "crash-after-rename", 86);
    assert(history_count(second) == before_failed_persist + 1);
    assert(contains_entry(second, "crash-after-rename"));
    crash_append(path, HISTORY_TEST_CRASH_AFTER_DIR_FSYNC,
                 "crash-after-dir-sync", 88);
    assert(contains_entry(second, "crash-after-dir-sync"));
    crash_append(path, HISTORY_TEST_CRASH_DURING_SHARED_SYNC,
                 "crash-during-shared-sync", 87);
    assert(contains_entry(second, "crash-during-shared-sync"));

    /* Each failure leaves a robust-mutex owner death. The next operation
     * repairs the semaphore token and reloads the durable file. */
    crash_append(path, HISTORY_TEST_CRASH_AFTER_MUTEX_BEFORE_SEM,
                 "crash-before-semaphore", 90);
    assert(history_count(second) == history_count(shared));
    crash_append(path, HISTORY_TEST_CRASH_AFTER_SEM_ACQUIRE,
                 "crash-after-semaphore", 91);
    assert(!contains_entry(second, "crash-after-semaphore"));
    assert(history_add(second, "recovered-after-semaphore-crash") == 0);
    assert(contains_entry(shared, "recovered-after-semaphore-crash"));
    crash_append(path, HISTORY_TEST_CRASH_DURING_MUTATION,
                 "crash-during-mutation", 92);
    assert(!contains_entry(second, "crash-during-mutation"));
    assert(history_add(second, "recovered-after-mutation-crash") == 0);
    crash_append(path, HISTORY_TEST_CRASH_AFTER_SEM_RELEASE,
                 "crash-after-semaphore-release", 93);
    assert(contains_entry(second, "crash-after-semaphore-release"));

    /* Recovery itself may die after token normalization, during index
     * reconstruction, or after consistency but before acquiring the token. */
    crash_append(path, HISTORY_TEST_CRASH_AFTER_SEM_ACQUIRE,
                 "owner-for-recovery-crash", 91);
    crash_during_open_recovery(path, HISTORY_TEST_CRASH_DURING_SEM_RECOVERY);
    crash_during_open_recovery(path, HISTORY_TEST_CRASH_DURING_SEM_RESTORE);
    crash_during_open_recovery(path, HISTORY_TEST_CRASH_DURING_INDEX_RECOVERY);
    crash_during_open_recovery(path, HISTORY_TEST_CRASH_AFTER_MUTEX_BEFORE_SEM);
    assert(history_add(second, "recovered-after-recovery-crashes") == 0);
    assert(contains_entry(shared, "recovered-after-recovery-crashes"));

    /* The test build uses a 64 KiB fixed arena. Capacity failure must leave
     * the durable history and its shared index unchanged. */
    size_t before_capacity = history_count(second);
    char too_large_for_arena[70001];
    memset(too_large_for_arena, 'x', sizeof(too_large_for_arena) - 1);
    too_large_for_arena[sizeof(too_large_for_arena) - 1] = '\0';
    errno = 0;
    assert(history_add(second, too_large_for_arena) < 0 && errno == ENOSPC);
    assert(history_count(shared) == before_capacity);

    /* The shared backend keeps the latest 10,000 records, including after a
     * new append evicts the oldest record. */
    char bulk_path[] = "/tmp/terminator-history-shared-limit-XXXXXX";
    int bulk_fd = mkstemp(bulk_path);
    assert(bulk_fd >= 0);
    for (unsigned i = 0; i < 10005; ++i) {
        char row[16];
        int length = snprintf(row, sizeof(row), "a%04u\n", i);
        assert(length > 0 && write(bulk_fd, row, (size_t)length) == length);
    }
    close(bulk_fd);
    History *bulk = history_open_process_shared(bulk_path);
    assert(bulk != NULL && history_count(bulk) == 10000);
    entry = history_get(bulk, 0);
    assert(entry != NULL && strcmp(entry, "a0005") == 0);
    free(entry);
    entry = history_get(bulk, 9999);
    assert(entry != NULL && strcmp(entry, "a10004") == 0);
    free(entry);
    assert(history_add(bulk, "newest-shared-entry") == 0);
    assert(history_count(bulk) == 10000);
    entry = history_get(bulk, 0);
    assert(entry != NULL && strcmp(entry, "a0006") == 0);
    free(entry);
    entry = history_get(bulk, 9999);
    assert(entry != NULL && strcmp(entry, "newest-shared-entry") == 0);
    free(entry);
    assert(history_test_unlink_segment(bulk) == 0);
    history_close(bulk);
    char bulk_lock[512];
    (void)snprintf(bulk_lock, sizeof(bulk_lock), "%s.lock", bulk_path);
    assert(unlink(bulk_lock) == 0);
    assert(unlink(bulk_path) == 0);

    history_close(thread_mode);
    history_close(second);
    history_close(shared);

    /* Reopening is a new application session; file contents remain durable. */
    shared = history_open_process_shared(path);
    assert(shared != NULL && contains_entry(shared, "crash-during-shared-sync"));
    entry = history_get(shared, 1);
    assert(entry != NULL &&
           strcmp(entry, "first physical line\nsecond physical line") == 0);
    free(entry);
    assert(history_test_unlink_segment(shared) == 0);
    history_close(shared);
    char lock_path[512];
    (void)snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    assert(unlink(lock_path) == 0);
    assert(unlink(path) == 0);

    /* Both child processes race to create and initialize a previously absent
     * segment. flock() must serialize one-time mutex/semaphore initialization. */
    char attach_path[] = "/tmp/terminator-history-attach-XXXXXX";
    int attach_fd = mkstemp(attach_path);
    assert(attach_fd >= 0 && close(attach_fd) == 0);
    int attach_gate[2];
    assert(pipe(attach_gate) == 0);
    pid_t attach_a = start_first_attach_writer(attach_path, 8,
                                                attach_gate[0]);
    pid_t attach_b = start_first_attach_writer(attach_path, 9,
                                                attach_gate[0]);
    assert(write(attach_gate[1], "go", 2) == 2);
    close(attach_gate[0]);
    close(attach_gate[1]);
    wait_success(attach_a);
    wait_success(attach_b);
    History *attached = history_open_process_shared(attach_path);
    assert(attached != NULL && history_count(attached) == 50);
    assert(history_test_unlink_segment(attached) == 0);
    history_close(attached);
    char attach_lock[512];
    (void)snprintf(attach_lock, sizeof(attach_lock), "%s.lock", attach_path);
    assert(unlink(attach_lock) == 0);
    assert(unlink(attach_path) == 0);

    test_successful_segment_retirement_remaps_old_handle();
    test_failed_retirement_fail_stops_and_recovers();
    puts("process-shared history tests passed");
    return 0;
}
