#define _POSIX_C_SOURCE 200809L

#include "history.h"
#include "key_config.h"

#include <X11/keysym.h>
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    History *history;
    unsigned worker;
} Writer;

static void *
write_history(void *argument)
{
    Writer *writer = argument;
    char command[64];
    unsigned i;
    for (i = 0; i < 50; ++i) {
        (void)snprintf(command, sizeof(command), "worker-%u-command-%u",
                       writer->worker, i);
        assert(history_add(writer->history, command) == 0);
    }
    return NULL;
}

static void
write_file(const char *path, const char *contents)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    size_t length = strlen(contents);
    size_t offset = 0;
    assert(fd >= 0);
    while (offset < length) {
        ssize_t amount = write(fd, contents + offset, length - offset);
        assert(amount > 0);
        offset += (size_t)amount;
    }
    close(fd);
}

int
main(void)
{
    char history_path[] = "/tmp/terminator-history-XXXXXX";
    char bulk_history_path[] = "/tmp/terminator-history-bulk-XXXXXX";
    char config_path[] = "/tmp/terminator-config-XXXXXX";
    int history_fd = mkstemp(history_path);
    int config_fd = mkstemp(config_path);
    History *history;
    HistoryCursor cursor = {0};
    char *entry;
    pthread_t threads[4];
    Writer writers[4];
    unsigned i;
    KeyConfig config;

    assert(history_fd >= 0 && config_fd >= 0);
    close(history_fd);
    close(config_fd);
    history = history_open(history_path);
    assert(history != NULL && history_count(history) == 0);
    assert(history_add(history, "") == 0);
    assert(history_add(history, " \t  ") == 0);
    assert(history_count(history) == 0);
    assert(history_add(history, "echo one") == 0);
    assert(history_add(history, "echo two") == 0);
    assert(history_add(history, "echo two\ncontinued") == 0);
    {
        char **matches = NULL;
        size_t count = 0;
        size_t first_index = 0;
        assert(history_add(history, "compile alpha target") == 0);
        assert(history_add(history, "copy alpha source") == 0);
        assert(history_add(history, "compile beta target") == 0);
        assert(history_search(history, "echo two", &matches, &count) == 0);
        assert(count == 1 && strcmp(matches[0], "echo two") == 0);
        free(matches[0]);
        free(matches);
        assert(history_search(history, "prefix alpha ending", &matches, &count) == 0);
        assert(count == 2);
        assert(strcmp(matches[0], "compile alpha target") == 0);
        assert(strcmp(matches[1], "copy alpha source") == 0);
        free(matches[0]); free(matches[1]); free(matches);
        assert(history_search(history, "ec", &matches, &count) == 0);
        assert(count == 0 && matches == NULL);
        assert(history_recent(history, 2, &matches, &count, &first_index) == 0);
        assert(count == 2 && first_index == 4);
        assert(strcmp(matches[0], "copy alpha source") == 0);
        assert(strcmp(matches[1], "compile beta target") == 0);
        free(matches[0]); free(matches[1]); free(matches);
    }
    entry = history_cursor_previous(history, &cursor, "draft");
    assert(entry != NULL && strcmp(entry, "compile beta target") == 0);
    free(entry);
    entry = history_cursor_previous(history, &cursor, "ignored");
    assert(entry != NULL && strcmp(entry, "copy alpha source") == 0);
    free(entry);
    entry = history_cursor_next(history, &cursor);
    assert(entry != NULL && strcmp(entry, "compile beta target") == 0);
    free(entry);
    entry = history_cursor_next(history, &cursor);
    assert(entry != NULL && strcmp(entry, "draft") == 0);
    free(entry);
    assert(!cursor.active);

    History *second = history_open(history_path);
    assert(second != NULL);
    for (i = 0; i < 4; ++i) {
        writers[i] = (Writer){.history = (i & 1U) ? second : history,
                              .worker = i};
        assert(pthread_create(&threads[i], NULL, write_history, &writers[i]) == 0);
    }
    for (i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    history_close(history);
    history_close(second);
    history = history_open(history_path);
    assert(history != NULL && history_count(history) == 206);
    entry = history_get(history, 0);
    assert(entry != NULL && strcmp(entry, "echo one") == 0);
    free(entry);
    entry = history_get(history, 2);
    assert(entry != NULL && strcmp(entry, "echo two\ncontinued") == 0);
    free(entry);
    history_close(history);

    int bulk_fd = mkstemp(bulk_history_path);
    assert(bulk_fd >= 0);
    for (i = 0; i < 1005; ++i) {
        char row[32];
        int length = snprintf(row, sizeof(row), "entry-%04u\n", i);
        assert(length > 0 && write(bulk_fd, row, (size_t)length) == length);
    }
    close(bulk_fd);
    History *bulk = history_open(bulk_history_path);
    assert(bulk != NULL);
    char **recent = NULL;
    size_t recent_count = 0, recent_first = 0;
    assert(history_recent(bulk, 1000, &recent, &recent_count, &recent_first) == 0);
    assert(recent_count == 1000 && recent_first == 5);
    assert(strcmp(recent[0], "entry-0005") == 0);
    assert(strcmp(recent[999], "entry-1004") == 0);
    for (i = 0; i < recent_count; ++i)
        free(recent[i]);
    free(recent);
    history_close(bulk);
    assert(unlink(bulk_history_path) == 0);

    write_file(config_path,
               "# valid override\n"
               "new_tab = Alt+N\n"
               "close_tab = Alt+N\n" /* Conflicts; ignored. */
               "close_tab = Ctrl+W\n" /* Reserved for word deletion. */
               "interrupt = Ctrl+Shift+C\n" /* Reserved for clipboard copy. */
               "history_next = Ctrl+D\n" /* Reserved for EOF/tab close. */
               "next_tab = Ctrl+NotAKey\n" /* Invalid; default retained. */
               "complete = Ctrl+Shift+Tab\n" /* Conflicts; ignored. */
               "previous_tab = Alt+P\n"
               "history_previous = Ctrl+Up\n");
    int accepted = key_config_load(&config, config_path);
    assert(accepted == 3);
    assert(key_config_match(&config, XK_n, Mod1Mask) == SHORTCUT_NEW_TAB);
    assert(key_config_match(&config, XK_w, ControlMask) == SHORTCUT_NONE);
    assert(key_config_match(&config, XK_w, Mod1Mask) == SHORTCUT_CLOSE_TAB);
    assert(key_config_match(&config, XK_Tab, ControlMask) == SHORTCUT_NEXT_TAB);
    assert(key_config_match(&config, XK_p, Mod1Mask) == SHORTCUT_PREVIOUS_TAB);
    assert(key_config_match(&config, XK_Tab, ControlMask | ShiftMask) ==
           SHORTCUT_NONE);
    assert(key_config_match(&config, XK_Up, ControlMask) == SHORTCUT_HISTORY_PREVIOUS);
    assert(key_config_match(&config, XK_Tab, 0) == SHORTCUT_COMPLETE);
    assert(key_config_load(&config, "/tmp/terminator-missing-key-config") == 0);
    assert(key_config_match(&config, XK_t, ControlMask) == SHORTCUT_NEW_TAB);

    char lock_path[512];
    assert(snprintf(lock_path, sizeof(lock_path), "%s.lock", history_path) > 0);
    assert(unlink(lock_path) == 0);
    assert(snprintf(lock_path, sizeof(lock_path), "%s.lock", bulk_history_path) > 0);
    assert(unlink(lock_path) == 0);
    assert(unlink(history_path) == 0);
    assert(unlink(config_path) == 0);
    puts("history/config tests passed");
    return 0;
}
