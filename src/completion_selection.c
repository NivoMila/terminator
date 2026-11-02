#include "completion_selection.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void
completion_selection_init(CompletionSelection *selection)
{
    if (selection != NULL) {
        selection->matches = NULL;
        selection->count = 0;
        selection->start = 0;
        selection->digit_count = 0;
        selection->digits[0] = '\0';
        selection->active = 0;
    }
}

void
completion_selection_clear(CompletionSelection *selection)
{
    size_t i;
    if (selection == NULL)
        return;
    for (i = 0; i < selection->count; ++i)
        free(selection->matches[i]);
    free(selection->matches);
    completion_selection_init(selection);
}

int
completion_selection_begin(CompletionSelection *selection, char ***matches,
                           size_t *count, size_t start)
{
    if (selection == NULL || matches == NULL || *matches == NULL ||
        count == NULL || *count == 0)
        return -1;
    completion_selection_clear(selection);
    selection->matches = *matches;
    selection->count = *count;
    selection->start = start;
    selection->active = 1;
    *matches = NULL;
    *count = 0;
    return 0;
}

static int
parse_number(const CompletionSelection *selection, size_t *number)
{
    size_t value = 0;
    size_t i;
    if (selection->digit_count == 0)
        return 0;
    for (i = 0; i < selection->digit_count; ++i) {
        unsigned digit = (unsigned)(selection->digits[i] - '0');
        if (value > (SIZE_MAX - digit) / 10)
            return -1;
        value = value * 10 + digit;
    }
    *number = value;
    return 1;
}

static int
has_larger_index_with_prefix(const CompletionSelection *selection, size_t number)
{
    size_t candidate;
    size_t divisor = 1;
    size_t copy = number;
    while (copy > 0) {
        if (divisor > SIZE_MAX / 10)
            return 0;
        divisor *= 10;
        copy /= 10;
    }
    for (candidate = number + 1; candidate <= selection->count; ++candidate) {
        size_t d = divisor;
        while (d > 1 && candidate % d != number % d)
            d /= 10;
        if (d == divisor && candidate / divisor == number)
            return 1;
    }
    return 0;
}

CompletionSelectionResult
completion_selection_key(CompletionSelection *selection, int key,
                         size_t *chosen_index)
{
    size_t number;
    int parsed;
    if (selection == NULL || !selection->active || chosen_index == NULL)
        return COMPLETION_SELECTION_CANCELLED;
    if (key == 27) {
        selection->active = 0;
        return COMPLETION_SELECTION_CANCELLED;
    }
    if (key == '\b' || key == 127) {
        if (selection->digit_count != 0)
            selection->digits[--selection->digit_count] = '\0';
        return COMPLETION_SELECTION_PENDING;
    }
    if (key == '\n' || key == '\r') {
        parsed = parse_number(selection, &number);
        if (parsed == 1 && number > 0 && number <= selection->count) {
            *chosen_index = number - 1;
            selection->active = 0;
            return COMPLETION_SELECTION_CHOSEN;
        }
        selection->active = 0;
        return COMPLETION_SELECTION_INVALID;
    }
    if (key < '0' || key > '9' || selection->digit_count + 1 >= sizeof(selection->digits)) {
        selection->active = 0;
        return COMPLETION_SELECTION_INVALID;
    }
    if (selection->digit_count == 0 && key == '0') {
        selection->active = 0;
        return COMPLETION_SELECTION_INVALID;
    }
    selection->digits[selection->digit_count++] = (char)key;
    selection->digits[selection->digit_count] = '\0';
    parsed = parse_number(selection, &number);
    if (parsed <= 0 || number == 0 || number > selection->count) {
        selection->active = 0;
        return COMPLETION_SELECTION_INVALID;
    }
    if (!has_larger_index_with_prefix(selection, number)) {
        *chosen_index = number - 1;
        selection->active = 0;
        return COMPLETION_SELECTION_CHOSEN;
    }
    return COMPLETION_SELECTION_PENDING;
}

const char *
completion_selection_match(const CompletionSelection *selection, size_t index)
{
    if (selection == NULL || index >= selection->count)
        return NULL;
    return selection->matches[index];
}

int
completion_selection_apply(const CompletionSelection *selection, size_t index,
                           char **input, size_t *capacity, size_t *length,
                           size_t *cursor)
{
    const char *match = completion_selection_match(selection, index);
    size_t start;
    size_t old_length;
    size_t match_length;
    size_t encoded_length = 0;
    size_t i;
    char *encoded;
    char *grown;

    if (match == NULL || input == NULL || *input == NULL || capacity == NULL ||
        length == NULL || cursor == NULL || *cursor > *length ||
        selection->start > *cursor)
        return -1;
    start = selection->start;
    old_length = *cursor - start;
    match_length = strlen(match);
    for (i = 0; i < match_length; ++i) {
        char c = match[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\\' ||
            c == '\'' || c == '"' || c == '|' || c == '<' || c == '>' || c == '&')
            ++encoded_length;
        ++encoded_length;
    }
    encoded = malloc(encoded_length + 1);
    if (encoded == NULL)
        return -1;
    encoded_length = 0;
    for (i = 0; i < match_length; ++i) {
        char c = match[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\\' ||
            c == '\'' || c == '"' || c == '|' || c == '<' || c == '>' || c == '&')
            encoded[encoded_length++] = '\\';
        encoded[encoded_length++] = c;
    }
    encoded[encoded_length] = '\0';
    if (*length - old_length > SIZE_MAX - encoded_length - 1) {
        free(encoded);
        return -1;
    }
    if (*length - old_length + encoded_length + 1 > *capacity) {
        size_t next = *capacity == 0 ? 128 : *capacity;
        while (next < *length - old_length + encoded_length + 1) {
            if (next > SIZE_MAX / 2) {
                next = *length - old_length + encoded_length + 1;
                break;
            }
            next *= 2;
        }
        grown = realloc(*input, next);
        if (grown == NULL) {
            free(encoded);
            return -1;
        }
        *input = grown;
        *capacity = next;
    }
    memmove(*input + start + encoded_length, *input + *cursor,
            *length - *cursor + 1);
    memcpy(*input + start, encoded, encoded_length);
    *length = *length - old_length + encoded_length;
    *cursor = start + encoded_length;
    free(encoded);
    return 0;
}
