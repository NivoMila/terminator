#define _POSIX_C_SOURCE 200809L

#include "completion_selection.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char **
matches(const char *first, const char *second)
{
    char **items = calloc(2, sizeof(*items));
    assert(items != NULL);
    items[0] = strdup(first);
    items[1] = strdup(second);
    assert(items[0] != NULL && items[1] != NULL);
    return items;
}

static void
begin_selection(CompletionSelection *selection, char **items, size_t count,
                size_t start)
{
    char **caller_matches = items;
    size_t caller_count = count;
    assert(completion_selection_begin(selection, &caller_matches, &caller_count,
                                       start) == 0);
    assert(caller_matches == NULL);
    assert(caller_count == 0);
}

static void
test_select_and_escape(void)
{
    CompletionSelection selection;
    char *input = strdup("run dir/fi");
    size_t capacity = strlen(input) + 1;
    size_t length = strlen(input);
    size_t cursor = length;
    size_t chosen = 99;
    assert(input != NULL);
    completion_selection_init(&selection);
    begin_selection(&selection, matches("dir/file one", "dir/file two"), 2, 4);
    assert(completion_selection_key(&selection, '2', &chosen) ==
           COMPLETION_SELECTION_CHOSEN);
    assert(chosen == 1);
    assert(completion_selection_apply(&selection, chosen, &input, &capacity,
                                      &length, &cursor) == 0);
    assert(strcmp(input, "run dir/file\\ two") == 0);
    assert(cursor == strlen(input));
    completion_selection_clear(&selection);
    free(input);
}

static void
test_invalid_and_cancel(void)
{
    CompletionSelection selection;
    size_t chosen = 0;
    completion_selection_init(&selection);

    begin_selection(&selection, matches("a", "b"), 2, 0);
    assert(completion_selection_key(&selection, '3', &chosen) ==
           COMPLETION_SELECTION_INVALID);
    completion_selection_clear(&selection);

    begin_selection(&selection, matches("a", "b"), 2, 0);
    assert(completion_selection_key(&selection, '0', &chosen) ==
           COMPLETION_SELECTION_INVALID);
    completion_selection_clear(&selection);

    begin_selection(&selection, matches("a", "b"), 2, 0);
    assert(completion_selection_key(&selection, 'x', &chosen) ==
           COMPLETION_SELECTION_INVALID);
    completion_selection_clear(&selection);

    begin_selection(&selection, matches("a", "b"), 2, 0);
    assert(completion_selection_key(&selection, 27, &chosen) ==
           COMPLETION_SELECTION_CANCELLED);
    completion_selection_clear(&selection);
}

static void
test_rest_of_line_is_preserved(void)
{
    CompletionSelection selection;
    char *input = strdup("run abc-tail");
    size_t capacity = strlen(input) + 1;
    size_t length = strlen(input);
    size_t cursor = 7;
    size_t chosen = 0;
    assert(input != NULL);
    completion_selection_init(&selection);
    begin_selection(&selection, matches("abc file", "abc folder"), 2, 4);
    assert(completion_selection_key(&selection, '1', &chosen) ==
           COMPLETION_SELECTION_CHOSEN);
    assert(completion_selection_apply(&selection, chosen, &input, &capacity,
                                      &length, &cursor) == 0);
    assert(strcmp(input, "run abc\\ file-tail") == 0);
    assert(cursor == strlen("run abc\\ file"));
    completion_selection_clear(&selection);
    free(input);
}

static void
test_long_number_and_tab_isolation(void)
{
    CompletionSelection first, second;
    char **many = calloc(12, sizeof(*many));
    size_t i, chosen = 0;
    assert(many != NULL);
    for (i = 0; i < 12; ++i) {
        char number[16];
        (void)snprintf(number, sizeof(number), "candidate-%zu", i + 1);
        many[i] = strdup(number);
        assert(many[i] != NULL);
    }
    completion_selection_init(&first);
    completion_selection_init(&second);
    begin_selection(&first, many, 12, 0);
    begin_selection(&second, matches("left", "right"), 2, 0);
    assert(completion_selection_key(&first, '1', &chosen) ==
           COMPLETION_SELECTION_PENDING);
    assert(completion_selection_key(&second, '2', &chosen) ==
           COMPLETION_SELECTION_CHOSEN);
    assert(chosen == 1);
    assert(first.active && second.active == 0);
    assert(completion_selection_key(&first, '2', &chosen) ==
           COMPLETION_SELECTION_CHOSEN);
    assert(chosen == 11);
    completion_selection_clear(&first);
    completion_selection_clear(&second);
}

static char **
three_matches(const char *one, const char *two, const char *three)
{
    char **items = calloc(3, sizeof(*items));
    assert(items != NULL);
    items[0] = strdup(one);
    items[1] = strdup(two);
    items[2] = strdup(three);
    assert(items[0] != NULL && items[1] != NULL && items[2] != NULL);
    return items;
}

static void
test_repeated_tab_transfer_and_tab_close(void)
{
    CompletionSelection selection;
    char **caller_matches;
    size_t caller_count;
    size_t chosen = 0;
    completion_selection_init(&selection);

    /* Reproduces `cat app`, Tab to `appl`, then Tab with three matches. */
    caller_matches = three_matches("apple.txt", "application.txt", "apply.txt");
    caller_count = 3;
    assert(completion_selection_begin(&selection, &caller_matches, &caller_count,
                                      4) == 0);
    assert(caller_matches == NULL && caller_count == 0);
    assert(selection.count == 3 && selection.active);
    assert(strcmp(completion_selection_match(&selection, 0), "apple.txt") == 0);

    /* A repeated Tab replaces the pending list without leaking the old one. */
    caller_matches = three_matches("apple-new.txt", "application-new.txt",
                                   "apply-new.txt");
    caller_count = 3;
    assert(completion_selection_begin(&selection, &caller_matches, &caller_count,
                                      4) == 0);
    assert(caller_matches == NULL && caller_count == 0);
    assert(selection.count == 3 && selection.active);
    assert(strcmp(completion_selection_match(&selection, 1),
                  "application-new.txt") == 0);
    assert(completion_selection_key(&selection, '2', &chosen) ==
           COMPLETION_SELECTION_CHOSEN);
    assert(chosen == 1);
    completion_selection_clear(&selection);

    /* Tab destruction clears a pending list; a second clear is harmless. */
    caller_matches = three_matches("apple.txt", "application.txt", "apply.txt");
    caller_count = 3;
    assert(completion_selection_begin(&selection, &caller_matches, &caller_count,
                                      4) == 0);
    completion_selection_clear(&selection);
    assert(!selection.active && selection.count == 0 && selection.matches == NULL);
    completion_selection_clear(&selection);
}

int
main(void)
{
    test_select_and_escape();
    test_invalid_and_cancel();
    test_rest_of_line_is_preserved();
    test_long_number_and_tab_isolation();
    test_repeated_tab_transfer_and_tab_close();
    puts("completion selection tests passed");
    return 0;
}
