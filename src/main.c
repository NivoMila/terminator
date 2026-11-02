#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "history.h"
#include "key_config.h"
#include "editor.h"
#include "xclipboard.h"
#include "interactive_input.h"
#include "completion_selection.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    WINDOW_WIDTH = 900,
    WINDOW_HEIGHT = 600,
    TAB_HEIGHT = 34,
    INPUT_HEIGHT = 34,
    BUFFER_LIMIT = 2000,
    INPUT_INITIAL_CAPACITY = 128,
    TAB_MIN_WIDTH = 120,
    TAB_MAX_WIDTH = 220,
    MAX_GUI_EVENTS_PER_TURN = 128
};

typedef struct {
    char **lines;
    size_t line_count;
    size_t line_capacity;
    char *input;
    size_t input_length;
    size_t input_capacity;
    size_t cursor;
    size_t scroll_rows;
    HistoryCursor history_cursor;
    InteractiveInput multiline_input;
    CompletionSelection completion_selection;
    int history_search_active;
    char *history_search_input;
    size_t history_search_length;
    size_t history_search_capacity;
    char *partial_line;
    size_t partial_length;
    size_t partial_capacity;
    Shell *shell;
    int busy;
} Tab;

typedef struct {
    Display *display;
    int screen;
    Window window;
    GC gc;
    XFontStruct *font;
    Atom wm_delete;
    unsigned int width;
    unsigned int height;
    Tab **tabs;
    size_t tab_count;
    size_t active_tab;
    int wake_read_fd;
    int wake_write_fd;
    int running;
    History *history;
    KeyConfig shortcuts;
    XClipboard clipboard;
} App;

static void redraw(App *app);
static void history_navigation_reset(Tab *tab);

static void
fatal(const char *message)
{
    perror(message);
    exit(EXIT_FAILURE);
}

static void
set_close_on_exec(int fd)
{
    int flags;
    int result;
    do {
        flags = fcntl(fd, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0)
        fatal("get descriptor flags");
    do {
        result = fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        fatal("set close-on-exec");
}

static void
set_wakeup_fd_flags(int fd)
{
    int flags;
    int result;
    do {
        flags = fcntl(fd, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0)
        fatal("set wake pipe nonblocking");
    do {
        result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        fatal("set wake pipe nonblocking");

    set_close_on_exec(fd);
}

static void
tab_init(Tab *tab, int wake_write_fd, History *history)
{
    memset(tab, 0, sizeof(*tab));
    tab->input_capacity = INPUT_INITIAL_CAPACITY;
    tab->input = calloc(tab->input_capacity, 1);
    if (tab->input == NULL)
        fatal("calloc input");
    tab->shell = shell_create(wake_write_fd);
    if (tab->shell == NULL)
        fatal("create shell worker");
    shell_set_history(tab->shell, history);
    completion_selection_init(&tab->completion_selection);
}

static void
tab_destroy(Tab *tab)
{
    size_t i;

    for (i = 0; i < tab->line_count; ++i)
        free(tab->lines[i]);
    free(tab->lines);
    free(tab->input);
    free(tab->partial_line);
    free(tab->history_search_input);
    history_cursor_reset(&tab->history_cursor);
    interactive_input_destroy(&tab->multiline_input);
    completion_selection_clear(&tab->completion_selection);
    shell_destroy(tab->shell);
    memset(tab, 0, sizeof(*tab));
}

static void
tab_append_line(Tab *tab, const char *text)
{
    char *copy;

    if (tab->line_count == tab->line_capacity) {
        size_t next = tab->line_capacity == 0 ? 64 : tab->line_capacity * 2;
        char **grown = realloc(tab->lines, next * sizeof(*grown));
        if (grown == NULL)
            fatal("realloc lines");
        tab->lines = grown;
        tab->line_capacity = next;
    }

    copy = strdup(text);
    if (copy == NULL)
        fatal("strdup line");
    tab->lines[tab->line_count++] = copy;

    if (tab->line_count > BUFFER_LIMIT) {
        free(tab->lines[0]);
        memmove(tab->lines, tab->lines + 1,
                (tab->line_count - 1) * sizeof(*tab->lines));
        --tab->line_count;
    }
    tab->scroll_rows = 0;
}

static void
tab_append_output(Tab *tab, const char *text, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        char c = text[i];
        if (c == '\n') {
            if (tab->partial_line == NULL) {
                tab_append_line(tab, "");
            } else {
                tab_append_line(tab, tab->partial_line);
            }
            tab->partial_length = 0;
            if (tab->partial_line != NULL)
                tab->partial_line[0] = '\0';
            continue;
        }
        if (c == '\r')
            continue;
        if (c == '\0')
            c = '?';
        if (tab->partial_length + 1 >= tab->partial_capacity) {
            size_t next = tab->partial_capacity == 0 ? 128 : tab->partial_capacity * 2;
            char *grown = realloc(tab->partial_line, next);
            if (grown == NULL)
                fatal("realloc partial output");
            tab->partial_line = grown;
            tab->partial_capacity = next;
        }
        tab->partial_line[tab->partial_length++] = c;
        tab->partial_line[tab->partial_length] = '\0';
    }
    tab->scroll_rows = 0;
}

static void
tab_flush_partial(Tab *tab)
{
    if (tab->partial_length != 0) {
        tab_append_line(tab, tab->partial_line);
        tab->partial_length = 0;
        tab->partial_line[0] = '\0';
    }
}

static void
app_add_tab(App *app)
{
    Tab *tab = calloc(1, sizeof(*tab));
    Tab **grown;
    if (tab == NULL)
        fatal("calloc tab");
    tab_init(tab, app->wake_write_fd, app->history);
    grown = realloc(app->tabs, (app->tab_count + 1) * sizeof(*grown));
    if (grown == NULL)
        fatal("realloc tabs");
    app->tabs = grown;
    app->tabs[app->tab_count] = tab;
    app->active_tab = app->tab_count;
    ++app->tab_count;
}

static void
app_close_active_tab(App *app)
{
    if (app->tab_count <= 1)
        return;

    tab_destroy(app->tabs[app->active_tab]);
    free(app->tabs[app->active_tab]);
    if (app->active_tab + 1 < app->tab_count) {
        memmove(&app->tabs[app->active_tab], &app->tabs[app->active_tab + 1],
                (app->tab_count - app->active_tab - 1) * sizeof(*app->tabs));
    }
    --app->tab_count;
    if (app->active_tab >= app->tab_count)
        app->active_tab = app->tab_count - 1;
}

static void
app_close_tab(App *app, Tab *tab)
{
    size_t index;
    for (index = 0; index < app->tab_count; ++index) {
        if (app->tabs[index] == tab)
            break;
    }
    if (index == app->tab_count)
        return;

    tab_destroy(tab);
    free(tab);
    if (index + 1 < app->tab_count)
        memmove(&app->tabs[index], &app->tabs[index + 1],
                (app->tab_count - index - 1) * sizeof(*app->tabs));
    --app->tab_count;
    if (app->tab_count == 0) {
        app->active_tab = 0;
        app->running = 0;
    } else if (app->active_tab > index) {
        --app->active_tab;
    } else if (app->active_tab >= app->tab_count) {
        app->active_tab = app->tab_count - 1;
    }
}

static void
drain_shell_events(App *app)
{
    size_t index = 0;
    size_t processed = 0;
    int budget_reached = 0;
    int reads;
    char notifications[256];

    /* Bound draining so sustained output cannot starve queued X events. */
    for (reads = 0; reads < 4; ++reads) {
        ssize_t amount = read(app->wake_read_fd, notifications, sizeof(notifications));
        if (amount <= 0)
            break;
    }

    while (index < app->tab_count) {
        Tab *tab = app->tabs[index];
        ShellEvent event;
        int close_tab = 0;

        while (shell_pop_event(tab->shell, &event)) {
            if (event.type == SHELL_EVENT_OUTPUT)
                tab_append_output(tab, event.text, event.length);
            else if (event.type == SHELL_EVENT_COMMAND_DONE) {
                tab_flush_partial(tab);
                tab->busy = 0;
            } else if (event.type == SHELL_EVENT_EXIT) {
                close_tab = 1;
            }
            shell_event_dispose(&event);
            ++processed;
            if (processed >= MAX_GUI_EVENTS_PER_TURN) {
                budget_reached = 1;
                break;
            }
        }
        if (close_tab)
            app_close_tab(app, tab);
        else
            ++index;
        if (budget_reached)
            break;
    }
    if (budget_reached) {
        unsigned char notification = 1;
        ssize_t amount;
        do {
            amount = write(app->wake_write_fd, &notification, 1);
        } while (amount < 0 && errno == EINTR);
    }
    if (app->running && app->tab_count != 0)
        redraw(app);
}

static int
font_height(const App *app)
{
    return app->font->ascent + app->font->descent;
}

static int
text_width(const App *app, const char *text)
{
    return XTextWidth(app->font, text, (int)strlen(text));
}

static void
draw_tab_strip(App *app)
{
    size_t i;
    unsigned int usable = app->width > 46 ? app->width - 46 : app->width;
    unsigned int tab_width = app->tab_count == 0 ? TAB_MIN_WIDTH : usable / (unsigned int)app->tab_count;

    if (tab_width > TAB_MAX_WIDTH)
        tab_width = TAB_MAX_WIDTH;
    if (tab_width < TAB_MIN_WIDTH && app->tab_count > 0)
        tab_width = TAB_MIN_WIDTH;

    XSetForeground(app->display, app->gc, 0x252a34);
    XFillRectangle(app->display, app->window, app->gc, 0, 0, app->width, TAB_HEIGHT);

    for (i = 0; i < app->tab_count; ++i) {
        int x = (int)(i * tab_width);
        char label[32];
        unsigned int tab_number = (unsigned int)(i % 999U) + 1U;
        (void)snprintf(label, sizeof(label), " %u: tab %u%s", tab_number,
                       tab_number, app->tabs[i]->busy ? " *" : "");
        XSetForeground(app->display, app->gc,
                       i == app->active_tab ? 0x3b4252 : 0x2e3440);
        XFillRectangle(app->display, app->window, app->gc, x + 2, 3,
                       tab_width > 4 ? tab_width - 4 : tab_width, TAB_HEIGHT - 5);
        XSetForeground(app->display, app->gc, 0xe5e9f0);
        XDrawString(app->display, app->window, app->gc, x + 8,
                    (TAB_HEIGHT + font_height(app)) / 2 - 2,
                    label, (int)strlen(label));
    }

    XSetForeground(app->display, app->gc, 0x3b4252);
    XFillRectangle(app->display, app->window, app->gc,
                   (int)app->width - 39, 3, 35, TAB_HEIGHT - 5);
    XSetForeground(app->display, app->gc, 0xa3be8c);
    XDrawString(app->display, app->window, app->gc,
                (int)app->width - 29, (TAB_HEIGHT + font_height(app)) / 2 - 2,
                "+", 1);
}

static void
draw_body(App *app)
{
    Tab *tab = app->tabs[app->active_tab];
    int line_height = font_height(app) + 3;
    int body_top = TAB_HEIGHT + 8;
    int body_bottom = (int)app->height - INPUT_HEIGHT - 7;
    int max_rows = (body_bottom - body_top) / line_height;
    size_t total, end, start, i;

    XSetForeground(app->display, app->gc, 0x1e222a);
    XFillRectangle(app->display, app->window, app->gc, 0, TAB_HEIGHT,
                   app->width, app->height - TAB_HEIGHT - INPUT_HEIGHT);
    XSetForeground(app->display, app->gc, 0xd8dee9);

    if (max_rows < 1)
        max_rows = 1;
    total = tab->line_count + (tab->partial_length != 0 ? 1U : 0U);
    end = total;
    if (tab->scroll_rows < end)
        end -= tab->scroll_rows;
    else
        end = 0;
    start = end > (size_t)max_rows ? end - (size_t)max_rows : 0;

    for (i = start; i < end; ++i) {
        int y = body_top + (int)(i - start + 1) * line_height - 3;
        const char *line = i < tab->line_count ? tab->lines[i] : tab->partial_line;
        XDrawString(app->display, app->window, app->gc, 10, y,
                    line, (int)strlen(line));
    }
}

static void
draw_input(App *app)
{
    Tab *tab = app->tabs[app->active_tab];
    const char *prompt = tab->history_search_active ? "Enter search term: " :
                         tab->multiline_input.active ? "> " : "user@terminator> ";
    char empty_search_input[1] = {0};
    char *input = tab->history_search_active ?
        (tab->history_search_input == NULL ? empty_search_input : tab->history_search_input) : tab->input;
    size_t input_length = tab->history_search_active ? tab->history_search_length : tab->input_length;
    size_t cursor = tab->history_search_active ? input_length : tab->cursor;
    int baseline = (int)app->height - (INPUT_HEIGHT - font_height(app)) / 2 - 3;
    int prompt_width = text_width(app, prompt);
    int available = (int)app->width - prompt_width - 18;
    size_t first = 0;
    char *visible;

    XSetForeground(app->display, app->gc, 0x252a34);
    XFillRectangle(app->display, app->window, app->gc, 0,
                   (int)app->height - INPUT_HEIGHT, app->width, INPUT_HEIGHT);
    XSetForeground(app->display, app->gc, 0xa3be8c);
    XDrawString(app->display, app->window, app->gc, 10, baseline,
                prompt, (int)strlen(prompt));

    if (available < 1)
        available = 1;
    if (cursor > 0) {
        while (first < cursor &&
               text_width(app, input + first) > available)
            ++first;
    }
    visible = input + first;
    XSetForeground(app->display, app->gc, 0xe5e9f0);
    XDrawString(app->display, app->window, app->gc, 10 + prompt_width, baseline,
                visible, (int)(input_length - first));

    if (cursor >= first) {
        char saved = input[cursor];
        input[cursor] = '\0';
        XSetForeground(app->display, app->gc, 0xebcb8b);
        XDrawLine(app->display, app->window, app->gc,
                  10 + prompt_width + text_width(app, input + first),
                  baseline - app->font->ascent,
                  10 + prompt_width + text_width(app, input + first),
                  baseline + app->font->descent);
        input[cursor] = saved;
    }
}

static void
redraw(App *app)
{
    draw_tab_strip(app);
    draw_body(app);
    draw_input(app);
    XFlush(app->display);
}

static void
input_reserve(Tab *tab, size_t needed)
{
    if (needed > tab->input_capacity) {
        size_t next = tab->input_capacity;
        char *grown;
        while (next < needed)
            next *= 2;
        grown = realloc(tab->input, next);
        if (grown == NULL)
            fatal("realloc input");
        tab->input = grown;
        tab->input_capacity = next;
    }
}

static void
input_insert(Tab *tab, const char *text, size_t length)
{
    history_navigation_reset(tab);
    input_reserve(tab, tab->input_length + length + 1);
    memmove(tab->input + tab->cursor + length,
            tab->input + tab->cursor,
            tab->input_length - tab->cursor + 1);
    memcpy(tab->input + tab->cursor, text, length);
    tab->input_length += length;
    tab->cursor += length;
    tab->scroll_rows = 0;
}

static void
history_navigation_reset(Tab *tab)
{
    history_cursor_reset(&tab->history_cursor);
}

static void
replace_input(Tab *tab, const char *text)
{
    size_t length = strlen(text);
    input_reserve(tab, length + 1);
    memcpy(tab->input, text, length + 1);
    tab->input_length = length;
    tab->cursor = length;
    tab->scroll_rows = 0;
}

static void
history_previous(App *app, Tab *tab)
{
    char *entry;
    if (app->history == NULL)
        return;
    entry = history_cursor_previous(app->history, &tab->history_cursor,
                                    tab->input);
    if (entry != NULL) {
        replace_input(tab, entry);
        free(entry);
    }
}

static void
history_next(App *app, Tab *tab)
{
    char *entry;
    if (app->history == NULL || !tab->history_cursor.active)
        return;
    entry = history_cursor_next(app->history, &tab->history_cursor);
    if (entry != NULL) {
        replace_input(tab, entry);
        free(entry);
    }
}

static void
history_search_finish(App *app, Tab *tab)
{
    char **matches = NULL;
    size_t count = 0;
    const char *term = tab->history_search_input == NULL ? "" : tab->history_search_input;
    char *header;
    size_t header_size = strlen("Enter search term: \n") + strlen(term) + 1;
    header = malloc(header_size);
    if (header != NULL) {
        (void)snprintf(header, header_size, "Enter search term: %s\n", term);
        tab_append_line(tab, header);
        free(header);
    }
    if (app->history == NULL || history_search(app->history, term, &matches, &count) < 0) {
        tab_append_line(tab, "No match for search term in history");
    } else if (count == 0) {
        tab_append_line(tab, "No match for search term in history");
    } else {
        for (size_t i = 0; i < count; ++i) {
            tab_append_line(tab, matches[i]);
            free(matches[i]);
        }
        free(matches);
    }
    tab->history_search_active = 0;
    if (tab->history_search_input != NULL)
        tab->history_search_input[0] = '\0';
    tab->history_search_length = 0;
}

static void
history_search_append(Tab *tab, const char *bytes, size_t length)
{
    size_t needed = tab->history_search_length + length + 1;
    if (needed > tab->history_search_capacity) {
        size_t capacity = tab->history_search_capacity == 0 ? 64 : tab->history_search_capacity;
        while (capacity < needed)
            capacity *= 2;
        char *grown = realloc(tab->history_search_input, capacity);
        if (grown == NULL)
            return;
        tab->history_search_input = grown;
        tab->history_search_capacity = capacity;
    }
    memcpy(tab->history_search_input + tab->history_search_length, bytes, length);
    tab->history_search_length += length;
    tab->history_search_input[tab->history_search_length] = '\0';
}

static int
is_word_boundary(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '|' || c == '<' || c == '>' || c == '&';
}

static size_t
current_word_start(const char *input, size_t cursor)
{
    enum { NO_QUOTE, SINGLE_QUOTE, DOUBLE_QUOTE } quote = NO_QUOTE;
    size_t start = 0;
    size_t i;
    for (i = 0; i < cursor; ++i) {
        char c = input[i];
        if (quote == SINGLE_QUOTE) {
            if (c == '\'')
                quote = NO_QUOTE;
            continue;
        }
        if (quote == DOUBLE_QUOTE) {
            if (c == '"')
                quote = NO_QUOTE;
            else if (c == '\\' && i + 1 < cursor)
                ++i;
            continue;
        }
        if (c == '\\' && i + 1 < cursor) {
            ++i;
            continue;
        }
        if (c == '\'')
            quote = SINGLE_QUOTE;
        else if (c == '"')
            quote = DOUBLE_QUOTE;
        else if (is_word_boundary(c))
            start = i + 1;
    }
    return start;
}

static char *
decode_word_prefix(const char *input, size_t start, size_t end)
{
    enum { NO_QUOTE, SINGLE_QUOTE, DOUBLE_QUOTE } quote = NO_QUOTE;
    char *decoded = malloc(end - start + 1);
    size_t used = 0;
    size_t i;
    if (decoded == NULL)
        return NULL;
    for (i = start; i < end; ++i) {
        char c = input[i];
        if (quote == SINGLE_QUOTE) {
            if (c == '\'')
                quote = NO_QUOTE;
            else
                decoded[used++] = c;
        } else if (quote == DOUBLE_QUOTE) {
            if (c == '"')
                quote = NO_QUOTE;
            else if (c == '\\' && i + 1 < end) {
                char escaped = input[++i];
                if (escaped == '"' || escaped == '\\' || escaped == '$' || escaped == '`')
                    decoded[used++] = escaped;
                else {
                    decoded[used++] = '\\';
                    decoded[used++] = escaped;
                }
            } else
                decoded[used++] = c;
        } else if (c == '\'') {
            quote = SINGLE_QUOTE;
        } else if (c == '"') {
            quote = DOUBLE_QUOTE;
        } else if (c == '\\' && i + 1 < end) {
            decoded[used++] = input[++i];
        } else {
            decoded[used++] = c;
        }
    }
    decoded[used] = '\0';
    return decoded;
}

static char *
escape_word(const char *word)
{
    size_t length = strlen(word);
    char *escaped = malloc(length * 2 + 1);
    size_t used = 0;
    size_t i;
    if (escaped == NULL)
        return NULL;
    for (i = 0; i < length; ++i) {
        char c = word[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\\' ||
            c == '\'' || c == '"' || c == '|' || c == '<' || c == '>' || c == '&')
            escaped[used++] = '\\';
        escaped[used++] = c;
    }
    escaped[used] = '\0';
    return escaped;
}

static void
complete_input(Tab *tab)
{
    size_t start;
    char *prefix;
    char **matches = NULL;
    size_t count = 0;
    size_t common;
    size_t i;
    const char *replacement;
    char *encoded;
    size_t old_length;
    size_t new_length;

    history_navigation_reset(tab);
    if (tab->cursor > tab->input_length)
        return;
    start = current_word_start(tab->input, tab->cursor);
    prefix = decode_word_prefix(tab->input, start, tab->cursor);
    if (prefix == NULL)
        return;
    if (shell_complete_path(tab->shell, prefix, &matches, &count) < 0 || count == 0) {
        free(prefix);
        shell_completion_free(matches, count);
        return;
    }
    common = strlen(matches[0]);
    for (i = 1; i < count; ++i) {
        size_t j = 0;
        while (j < common && matches[0][j] == matches[i][j])
            ++j;
        common = j;
    }
    if (count == 1 || common > strlen(prefix)) {
        if (count == 1)
            replacement = matches[0];
        else {
            matches[0][common] = '\0';
            replacement = matches[0];
        }
        encoded = escape_word(replacement);
        if (encoded != NULL) {
            old_length = tab->cursor - start;
            new_length = strlen(encoded);
            input_reserve(tab, tab->input_length - old_length + new_length + 1);
            memmove(tab->input + start + new_length, tab->input + tab->cursor,
                    tab->input_length - tab->cursor + 1);
            memcpy(tab->input + start, encoded, new_length);
            tab->input_length = tab->input_length - old_length + new_length;
            tab->cursor = start + new_length;
            free(encoded);
        }
    } else {
        if (completion_selection_begin(&tab->completion_selection, &matches,
                                       &count, start) == 0) {
            for (i = 0; i < tab->completion_selection.count; ++i) {
                char *line;
                int needed = snprintf(NULL, 0, "%zu. %s", i + 1,
                                      completion_selection_match(
                                          &tab->completion_selection, i));
                if (needed < 0)
                    continue;
                line = malloc((size_t)needed + 1);
                if (line == NULL)
                    fatal("malloc completion list line");
                (void)snprintf(line, (size_t)needed + 1, "%zu. %s", i + 1,
                               completion_selection_match(
                                   &tab->completion_selection, i));
                tab_append_line(tab, line);
                free(line);
            }
        }
    }
    free(prefix);
    shell_completion_free(matches, count);
}

static void
completion_apply_choice(Tab *tab, size_t index)
{
    (void)completion_selection_apply(&tab->completion_selection, index,
                                     &tab->input, &tab->input_capacity,
                                     &tab->input_length, &tab->cursor);
    completion_selection_clear(&tab->completion_selection);
}

static void
submit_input(App *app, Tab *tab)
{
    int was_continuing = tab->multiline_input.active;
    const char *prompt = was_continuing ? "> " : "user@terminator> ";
    size_t prompt_len = strlen(prompt);
    char *line = malloc(prompt_len + tab->input_length + 1);
    char *command = NULL;
    ParseError error;
    InteractiveInputResult result;
    if (line == NULL)
        fatal("malloc submitted line");
    result = interactive_input_enter(&tab->multiline_input, tab->input,
                                     &command, &error);
    memcpy(line, prompt, prompt_len);
    memcpy(line + prompt_len, tab->input, tab->input_length + 1);
    tab_append_line(tab, line);
    if (result == INTERACTIVE_INPUT_CONTINUE) {
        free(line);
        tab->input_length = 0;
        tab->cursor = 0;
        tab->input[0] = '\0';
        history_navigation_reset(tab);
        return;
    }
    if (result == INTERACTIVE_INPUT_INVALID) {
        if (command != NULL) {
            char message[256];
            int count = snprintf(message, sizeof(message),
                                 "terminator: syntax error at byte %zu: %s",
                                 error.offset, error.message);
            if (count > 0)
                tab_append_line(tab, message);
            if (app->history != NULL && command[0] != '\0')
                (void)history_add(app->history, command);
        } else {
            tab_append_line(tab, "terminator: unable to parse command");
        }
    } else if (shell_submit(tab->shell, command) == 0) {
        if (app->history != NULL && command[0] != '\0')
            (void)history_add(app->history, command);
        if (command[0] != '\0')
            tab->busy = 1;
    } else {
        tab_append_line(tab, "terminator: unable to submit command");
    }
    free(line);
    free(command);
    tab->input_length = 0;
    tab->cursor = 0;
    tab->input[0] = '\0';
    history_navigation_reset(tab);
}

static void
paste_into_active_input(void *context, const char *text, size_t length)
{
    App *app = context;
    if (app->tab_count != 0 && !app->tabs[app->active_tab]->history_search_active) {
        completion_selection_clear(&app->tabs[app->active_tab]->completion_selection);
        input_insert(app->tabs[app->active_tab], text, length);
    }
}

static void
handle_key(App *app, XKeyEvent *event)
{
    char bytes[64];
    KeySym symbol = NoSymbol;
    int count = XLookupString(event, bytes, (int)sizeof(bytes), &symbol, NULL);
    Tab *tab = app->tabs[app->active_tab];
    int control = (event->state & ControlMask) != 0;

    ShortcutAction action = key_config_match(&app->shortcuts, symbol, event->state);
    if (tab->history_search_active) {
        if (symbol == XK_Escape) {
            tab->history_search_active = 0;
            tab->history_search_length = 0;
            if (tab->history_search_input != NULL)
                tab->history_search_input[0] = '\0';
        } else if (symbol == XK_Return || symbol == XK_KP_Enter) {
            history_search_finish(app, tab);
        } else if (symbol == XK_BackSpace) {
            if (tab->history_search_length != 0)
                tab->history_search_input[--tab->history_search_length] = '\0';
        } else if (!control && count > 0 && (unsigned char)bytes[0] >= 32) {
            history_search_append(tab, bytes, (size_t)count);
        }
        return;
    }
    if (action == SHORTCUT_INTERRUPT && tab->multiline_input.active) {
        (void)interactive_input_cancel(&tab->multiline_input);
        completion_selection_clear(&tab->completion_selection);
        tab->input_length = 0;
        tab->cursor = 0;
        tab->input[0] = '\0';
        history_navigation_reset(tab);
        tab_append_line(tab, "^C");
        return;
    }
    if (tab->completion_selection.active) {
        int selection_key = -1;
        size_t chosen = 0;
        CompletionSelectionResult result;
        if ((event->state & (ControlMask | Mod1Mask)) == 0 &&
            symbol >= XK_0 && symbol <= XK_9)
            selection_key = (int)symbol;
        else if ((event->state & (ControlMask | Mod1Mask)) == 0 &&
                 symbol >= XK_KP_0 && symbol <= XK_KP_9)
            selection_key = (int)('0' + symbol - XK_KP_0);
        else if (symbol == XK_BackSpace)
            selection_key = '\b';
        else if (symbol == XK_Return || symbol == XK_KP_Enter)
            selection_key = '\n';
        else if (symbol == XK_Escape)
            selection_key = 27;
        if (selection_key >= 0) {
            result = completion_selection_key(&tab->completion_selection,
                                              selection_key, &chosen);
            if (result == COMPLETION_SELECTION_CHOSEN)
                completion_apply_choice(tab, chosen);
            else if (result == COMPLETION_SELECTION_INVALID) {
                tab_append_line(tab, "Invalid completion selection");
                completion_selection_clear(&tab->completion_selection);
            } else if (result == COMPLETION_SELECTION_CANCELLED) {
                completion_selection_clear(&tab->completion_selection);
            }
            return;
        }
        if (tab->completion_selection.digit_count != 0 && !control &&
            count > 0 && (unsigned char)bytes[0] >= 32) {
            tab_append_line(tab, "Invalid completion selection");
            completion_selection_clear(&tab->completion_selection);
            return;
        }
        /* Other shortcuts and ordinary editing cancel selection and retain
         * their normal meaning. */
        completion_selection_clear(&tab->completion_selection);
    }
    if (control && (symbol == XK_r || symbol == XK_R)) {
        tab->history_search_active = 1;
        tab->history_search_length = 0;
        if (tab->history_search_input != NULL)
            tab->history_search_input[0] = '\0';
        history_navigation_reset(tab);
        return;
    }
    if (control && (event->state & ShiftMask) != 0 &&
        (symbol == XK_c || symbol == XK_C)) {
        (void)xclipboard_copy(app->display, app->window, &app->clipboard,
                              tab->input, tab->input_length);
        return;
    }
    if (control && (event->state & ShiftMask) != 0 &&
        (symbol == XK_v || symbol == XK_V)) {
        xclipboard_paste(app->display, app->window, &app->clipboard);
        return;
    }
    if (control && (symbol == XK_a || symbol == XK_A)) {
        history_navigation_reset(tab);
        editor_move_start(&tab->cursor);
        return;
    }
    if (control && (symbol == XK_e || symbol == XK_E)) {
        history_navigation_reset(tab);
        editor_move_end(tab->input_length, &tab->cursor);
        return;
    }
    if (control && (symbol == XK_u || symbol == XK_U)) {
        history_navigation_reset(tab);
        editor_kill_to_start(tab->input, &tab->input_length, &tab->cursor);
        return;
    }
    if (control && (symbol == XK_k || symbol == XK_K)) {
        history_navigation_reset(tab);
        editor_kill_to_end(tab->input, &tab->input_length, &tab->cursor);
        return;
    }
    if (control && (symbol == XK_w || symbol == XK_W)) {
        history_navigation_reset(tab);
        editor_kill_previous_word(tab->input, &tab->input_length, &tab->cursor);
        return;
    }
    if (control && (symbol == XK_l || symbol == XK_L)) {
        for (size_t i = 0; i < tab->line_count; ++i)
            free(tab->lines[i]);
        tab->line_count = 0;
        tab->partial_length = 0;
        if (tab->partial_line != NULL)
            tab->partial_line[0] = '\0';
        tab->scroll_rows = 0;
        return;
    }
    if (control && (symbol == XK_d || symbol == XK_D)) {
        app_close_tab(app, tab);
        return;
    }
    if (action == SHORTCUT_NEW_TAB) {
        app_add_tab(app);
        return;
    }
    if (action == SHORTCUT_CLOSE_TAB) {
        app_close_active_tab(app);
        return;
    }
    if (action == SHORTCUT_NEXT_TAB || action == SHORTCUT_PREVIOUS_TAB) {
        if (app->tab_count > 1) {
            if (action == SHORTCUT_NEXT_TAB)
                app->active_tab = (app->active_tab + 1) % app->tab_count;
            else
                app->active_tab = (app->active_tab + app->tab_count - 1) % app->tab_count;
        }
        return;
    }
    if (action == SHORTCUT_INTERRUPT || action == SHORTCUT_SUSPEND) {
        (void)shell_signal_foreground(tab->shell, action == SHORTCUT_SUSPEND);
        return;
    }
    if (action == SHORTCUT_COMPLETE) {
        complete_input(tab);
        return;
    }
    if (action == SHORTCUT_HISTORY_PREVIOUS) {
        history_previous(app, tab);
        return;
    }
    if (action == SHORTCUT_HISTORY_NEXT) {
        history_next(app, tab);
        return;
    }

    switch (symbol) {
    case XK_Left:
        history_navigation_reset(tab);
        if (tab->cursor > 0)
            --tab->cursor;
        break;
    case XK_Right:
        history_navigation_reset(tab);
        if (tab->cursor < tab->input_length)
            ++tab->cursor;
        break;
    case XK_Home:
        history_navigation_reset(tab);
        tab->cursor = 0;
        break;
    case XK_End:
        history_navigation_reset(tab);
        tab->cursor = tab->input_length;
        break;
    case XK_BackSpace:
        history_navigation_reset(tab);
        if (tab->cursor > 0) {
            memmove(tab->input + tab->cursor - 1, tab->input + tab->cursor,
                    tab->input_length - tab->cursor + 1);
            --tab->cursor;
            --tab->input_length;
        }
        break;
    case XK_Delete:
        history_navigation_reset(tab);
        if (tab->cursor < tab->input_length) {
            memmove(tab->input + tab->cursor, tab->input + tab->cursor + 1,
                    tab->input_length - tab->cursor);
            --tab->input_length;
        }
        break;
    case XK_Return:
    case XK_KP_Enter:
        submit_input(app, tab);
        break;
    case XK_Page_Up:
        if (tab->scroll_rows + 1 < tab->line_count)
            tab->scroll_rows += 10;
        if (tab->scroll_rows >= tab->line_count)
            tab->scroll_rows = tab->line_count > 0 ? tab->line_count - 1 : 0;
        break;
    case XK_Page_Down:
        tab->scroll_rows = tab->scroll_rows > 10 ? tab->scroll_rows - 10 : 0;
        break;
    default:
        if (!control && count > 0 && (unsigned char)bytes[0] >= 32)
            input_insert(tab, bytes, (size_t)count);
        break;
    }
}

static void
handle_button(App *app, XButtonEvent *event)
{
    unsigned int usable = app->width > 46 ? app->width - 46 : app->width;
    unsigned int tab_width = app->tab_count == 0 ? TAB_MIN_WIDTH : usable / (unsigned int)app->tab_count;
    size_t index;

    if (tab_width > TAB_MAX_WIDTH)
        tab_width = TAB_MAX_WIDTH;
    if (tab_width < TAB_MIN_WIDTH && app->tab_count > 0)
        tab_width = TAB_MIN_WIDTH;

    if (event->y >= TAB_HEIGHT || event->button != Button1)
        return;
    if ((unsigned int)event->x >= app->width - 42) {
        app_add_tab(app);
        return;
    }
    if (tab_width == 0)
        return;
    index = (size_t)event->x / tab_width;
    if (index < app->tab_count)
        app->active_tab = index;
}

static void
handle_event(App *app, XEvent *event)
{
    if (xclipboard_handle_event(app->display, app->window, &app->clipboard,
                                event, paste_into_active_input, app)) {
        if (app->running && app->tab_count != 0)
            redraw(app);
        return;
    }
    switch (event->type) {
    case Expose:
        if (event->xexpose.count == 0)
            redraw(app);
        break;
    case ConfigureNotify:
        app->width = (unsigned int)event->xconfigure.width;
        app->height = (unsigned int)event->xconfigure.height;
        redraw(app);
        break;
    case KeyPress:
        handle_key(app, &event->xkey);
        if (app->running && app->tab_count != 0)
            redraw(app);
        break;
    case ButtonPress:
        if (event->xbutton.button == Button4 || event->xbutton.button == Button5) {
            Tab *tab = app->tabs[app->active_tab];
            if (event->xbutton.button == Button4) {
                tab->scroll_rows += 3;
                if (tab->scroll_rows >= tab->line_count)
                    tab->scroll_rows = tab->line_count > 0 ? tab->line_count - 1 : 0;
            } else {
                tab->scroll_rows = tab->scroll_rows > 3 ? tab->scroll_rows - 3 : 0;
            }
        } else {
            handle_button(app, &event->xbutton);
        }
        redraw(app);
        break;
    case ClientMessage:
        if ((Atom)event->xclient.data.l[0] == app->wm_delete)
            app->running = 0;
        break;
    default:
        break;
    }
}

static void
app_init(App *app)
{
    XSizeHints hints;
    int wake_pipe[2];

    memset(app, 0, sizeof(*app));
    app->wake_read_fd = -1;
    app->wake_write_fd = -1;
    if (pipe(wake_pipe) < 0)
        fatal("pipe wakeup");
    app->wake_read_fd = wake_pipe[0];
    app->wake_write_fd = wake_pipe[1];
    set_wakeup_fd_flags(app->wake_read_fd);
    set_wakeup_fd_flags(app->wake_write_fd);
    app->history = history_open(NULL);
    (void)key_config_load(&app->shortcuts, NULL);
    app->display = XOpenDisplay(NULL);
    if (app->display == NULL) {
        (void)fprintf(stderr, "terminator: cannot open X display\n");
        exit(EXIT_FAILURE);
    }
    set_close_on_exec(ConnectionNumber(app->display));
    app->screen = DefaultScreen(app->display);
    app->width = WINDOW_WIDTH;
    app->height = WINDOW_HEIGHT;
    app->window = XCreateSimpleWindow(app->display,
                                      RootWindow(app->display, app->screen),
                                      40, 40, app->width, app->height, 1,
                                      BlackPixel(app->display, app->screen),
                                      0x1e222a);
    xclipboard_init(app->display, &app->clipboard);
    XStoreName(app->display, app->window, "Terminator: A Custom Terminal");
    XSelectInput(app->display, app->window,
                 ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    app->wm_delete = XInternAtom(app->display, "WM_DELETE_WINDOW", False);
    (void)XSetWMProtocols(app->display, app->window, &app->wm_delete, 1);

    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize;
    hints.min_width = 360;
    hints.min_height = 180;
    XSetWMNormalHints(app->display, app->window, &hints);

    app->gc = XCreateGC(app->display, app->window, 0, NULL);
    app->font = XLoadQueryFont(app->display, "fixed");
    if (app->font != NULL)
        XSetFont(app->display, app->gc, app->font->fid);
    else {
        (void)fprintf(stderr, "terminator: unable to load an X11 font\n");
        exit(EXIT_FAILURE);
    }

    app_add_tab(app);
    tab_append_line(app->tabs[0], "Thread-mode shell ready.");
    tab_append_line(app->tabs[0],
                    "Commands: cd, pwd, jobs, fg, bg, &, and external programs.");
    XMapWindow(app->display, app->window);
    app->running = 1;
}

static void
app_run(App *app)
{
    while (app->running) {
        struct pollfd fds[2];
        int result;
        while (app->running && XPending(app->display) > 0) {
            XEvent event;
            XNextEvent(app->display, &event);
            handle_event(app, &event);
        }
        if (!app->running)
            break;

        fds[0].fd = ConnectionNumber(app->display);
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = app->wake_read_fd;
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        result = poll(fds, 2, -1);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            fatal("poll");
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            app->running = 0;
        if (fds[1].revents & POLLIN)
            drain_shell_events(app);
    }
}

static void
app_destroy(App *app)
{
    size_t i;

    for (i = 0; i < app->tab_count; ++i) {
        tab_destroy(app->tabs[i]);
        free(app->tabs[i]);
    }
    free(app->tabs);
    history_close(app->history);
    xclipboard_destroy(&app->clipboard);
    if (app->gc != 0)
        XFreeGC(app->display, app->gc);
    if (app->font != NULL)
        XFreeFont(app->display, app->font);
    if (app->window != 0)
        XDestroyWindow(app->display, app->window);
    if (app->display != NULL)
        XCloseDisplay(app->display);
    if (app->wake_read_fd >= 0)
        (void)close(app->wake_read_fd);
    if (app->wake_write_fd >= 0)
        (void)close(app->wake_write_fd);
}

int
main(void)
{
    App app;
    app_init(&app);
    app_run(&app);
    app_destroy(&app);
    return EXIT_SUCCESS;
}
