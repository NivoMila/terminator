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
    entry = history_cursor_previous(history, &cursor, "draft");
    assert(entry != NULL && strcmp(entry, "echo two") == 0);
    free(entry);
    entry = history_cursor_previous(history, &cursor, "ignored");
    assert(entry != NULL && strcmp(entry, "echo one") == 0);
    free(entry);
    assert(history_cursor_previous(history, &cursor, "ignored") == NULL);
    entry = history_cursor_next(history, &cursor);
    assert(entry != NULL && strcmp(entry, "echo two") == 0);
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
    assert(history != NULL && history_count(history) == 202);
    entry = history_get(history, 0);
    assert(entry != NULL && strcmp(entry, "echo one") == 0);
    free(entry);
    history_close(history);

    write_file(config_path,
               "# valid override\n"
               "new_tab = Alt+N\n"
               "close_tab = Alt+N\n" /* Conflicts; ignored. */
               "next_tab = Ctrl+NotAKey\n" /* Invalid; default retained. */
               "complete = Ctrl+Shift+Tab\n" /* Conflicts; ignored. */
               "previous_tab = Alt+P\n"
               "history_previous = Ctrl+Up\n");
    int accepted = key_config_load(&config, config_path);
    assert(accepted == 3);
    assert(key_config_match(&config, XK_n, Mod1Mask) == SHORTCUT_NEW_TAB);
    assert(key_config_match(&config, XK_w, ControlMask) == SHORTCUT_CLOSE_TAB);
    assert(key_config_match(&config, XK_Tab, ControlMask) == SHORTCUT_NEXT_TAB);
    assert(key_config_match(&config, XK_p, Mod1Mask) == SHORTCUT_PREVIOUS_TAB);
    assert(key_config_match(&config, XK_Tab, ControlMask | ShiftMask) ==
           SHORTCUT_NONE);
    assert(key_config_match(&config, XK_Up, ControlMask) == SHORTCUT_HISTORY_PREVIOUS);
    assert(key_config_match(&config, XK_Tab, 0) == SHORTCUT_COMPLETE);
    assert(key_config_load(&config, "/tmp/terminator-missing-key-config") == 0);
    assert(key_config_match(&config, XK_t, ControlMask) == SHORTCUT_NEW_TAB);

    assert(unlink(history_path) == 0);
    assert(unlink(config_path) == 0);
    puts("history/config tests passed");
    return 0;
}
