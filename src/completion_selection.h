#ifndef TERMINATOR_COMPLETION_SELECTION_H
#define TERMINATOR_COMPLETION_SELECTION_H

#include <stddef.h>

typedef struct {
    char **matches;
    size_t count;
    size_t start;
    char digits[32];
    size_t digit_count;
    int active;
} CompletionSelection;

typedef enum {
    COMPLETION_SELECTION_PENDING,
    COMPLETION_SELECTION_CHOSEN,
    COMPLETION_SELECTION_INVALID,
    COMPLETION_SELECTION_CANCELLED
} CompletionSelectionResult;

void completion_selection_init(CompletionSelection *selection);
void completion_selection_clear(CompletionSelection *selection);
/* On success, atomically takes matches/count and resets the caller's values. */
int completion_selection_begin(CompletionSelection *selection, char ***matches,
                               size_t *count, size_t start);
CompletionSelectionResult completion_selection_key(CompletionSelection *selection,
                                                    int key, size_t *chosen_index);
const char *completion_selection_match(const CompletionSelection *selection,
                                       size_t index);
int completion_selection_apply(const CompletionSelection *selection, size_t index,
                               char **input, size_t *capacity, size_t *length,
                               size_t *cursor);

#endif
