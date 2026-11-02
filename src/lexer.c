#define _POSIX_C_SOURCE 200809L

#include "parser.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} WordBuilder;

static void
set_error(ParseError *error, size_t offset, const char *message)
{
    if (error == NULL)
        return;
    error->offset = offset;
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

static int
builder_append(WordBuilder *builder, char c)
{
    if (builder->length > SIZE_MAX - 2)
        return -1;
    if (builder->length + 2 > builder->capacity) {
        size_t next = builder->capacity == 0 ? 32 : builder->capacity;
        char *grown;
        while (next < builder->length + 2) {
            if (next > SIZE_MAX / 2) {
                next = builder->length + 2;
                break;
            }
            next *= 2;
        }
        grown = realloc(builder->data, next);
        if (grown == NULL)
            return -1;
        builder->data = grown;
        builder->capacity = next;
    }
    builder->data[builder->length++] = c;
    builder->data[builder->length] = '\0';
    return 0;
}

static int
tokens_reserve(TokenList *tokens)
{
    if (tokens->count < tokens->capacity)
        return 0;
    if (tokens->capacity > SIZE_MAX / (2 * sizeof(*tokens->items)))
        return -1;
    size_t next = tokens->capacity == 0 ? 16 : tokens->capacity * 2;
    Token *grown = realloc(tokens->items, next * sizeof(*grown));
    if (grown == NULL)
        return -1;
    tokens->items = grown;
    tokens->capacity = next;
    return 0;
}

static int
push_operator(TokenList *tokens, TokenType type, size_t offset)
{
    if (tokens_reserve(tokens) < 0)
        return -1;
    tokens->items[tokens->count++] = (Token){
        .type = type,
        .text = NULL,
        .offset = offset
    };
    return 0;
}

static int
flush_word(TokenList *tokens, WordBuilder *builder, int *started, size_t offset)
{
    char *word;
    if (!*started)
        return 0;
    if (builder->data == NULL) {
        builder->data = calloc(1, 1);
        if (builder->data == NULL)
            return -1;
    }
    if (tokens_reserve(tokens) < 0)
        return -1;
    word = builder->data;
    tokens->items[tokens->count++] = (Token){
        .type = TOKEN_WORD,
        .text = word,
        .offset = offset
    };
    builder->data = NULL;
    builder->length = 0;
    builder->capacity = 0;
    *started = 0;
    return 0;
}

void
lexer_tokens_free(TokenList *tokens)
{
    size_t i;
    if (tokens == NULL)
        return;
    for (i = 0; i < tokens->count; ++i)
        free(tokens->items[i].text);
    free(tokens->items);
    memset(tokens, 0, sizeof(*tokens));
}

int
lexer_tokenize(const char *input, TokenList *tokens, ParseError *error)
{
    enum { QUOTE_NONE, QUOTE_SINGLE, QUOTE_DOUBLE } quote = QUOTE_NONE;
    WordBuilder builder = {0};
    size_t word_offset = 0;
    size_t quote_offset = 0;
    size_t i = 0;
    int word_started = 0;

    if (tokens == NULL || input == NULL) {
        set_error(error, 0, "invalid lexer argument");
        return -1;
    }
    memset(tokens, 0, sizeof(*tokens));
    if (error != NULL)
        memset(error, 0, sizeof(*error));

    while (input[i] != '\0') {
        unsigned char c = (unsigned char)input[i];
        if (quote == QUOTE_SINGLE) {
            if (c == '\'')
                quote = QUOTE_NONE;
            else if (builder_append(&builder, (char)c) < 0)
                goto out_of_memory;
            ++i;
            continue;
        }
        if (quote == QUOTE_DOUBLE) {
            if (c == '"') {
                quote = QUOTE_NONE;
                ++i;
                continue;
            }
            if (c == '\\') {
                unsigned char escaped = (unsigned char)input[i + 1];
                if (escaped == '\0') {
                    set_error(error, i, "dangling escape in double quote");
                    goto fail;
                }
                if (escaped == '"' || escaped == '\\' || escaped == '$' || escaped == '`') {
                    if (builder_append(&builder, (char)escaped) < 0)
                        goto out_of_memory;
                } else {
                    if (builder_append(&builder, '\\') < 0 ||
                        builder_append(&builder, (char)escaped) < 0)
                        goto out_of_memory;
                }
                i += 2;
                continue;
            }
            if (builder_append(&builder, (char)c) < 0)
                goto out_of_memory;
            ++i;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (flush_word(tokens, &builder, &word_started, word_offset) < 0)
                goto out_of_memory;
            ++i;
            continue;
        }
        if (c == '\'' || c == '"') {
            if (!word_started)
                word_offset = i;
            word_started = 1;
            quote = c == '\'' ? QUOTE_SINGLE : QUOTE_DOUBLE;
            quote_offset = i++;
            continue;
        }
        if (c == '\\') {
            if (input[i + 1] == '\0') {
                set_error(error, i, "dangling escape");
                goto fail;
            }
            if (!word_started)
                word_offset = i;
            word_started = 1;
            if (builder_append(&builder, input[i + 1]) < 0)
                goto out_of_memory;
            i += 2;
            continue;
        }
        if (c == '&') {
            if (flush_word(tokens, &builder, &word_started, word_offset) < 0 ||
                push_operator(tokens, TOKEN_BACKGROUND, i) < 0)
                goto out_of_memory;
            ++i;
            continue;
        }
        if (c == '|' || c == '<' || c == '>') {
            TokenType type;
            size_t operator_offset = i;
            if (flush_word(tokens, &builder, &word_started, word_offset) < 0)
                goto out_of_memory;
            if (c == '|') {
                type = TOKEN_PIPE;
                ++i;
            } else if (c == '<') {
                type = TOKEN_REDIR_INPUT;
                ++i;
            } else if (input[i + 1] == '>') {
                type = TOKEN_REDIR_APPEND;
                i += 2;
            } else {
                type = TOKEN_REDIR_OUTPUT;
                ++i;
            }
            if (push_operator(tokens, type, operator_offset) < 0)
                goto out_of_memory;
            continue;
        }
        if (!word_started)
            word_offset = i;
        word_started = 1;
        if (builder_append(&builder, (char)c) < 0)
            goto out_of_memory;
        ++i;
    }

    if (quote != QUOTE_NONE) {
        set_error(error, quote_offset,
                  quote == QUOTE_SINGLE ? "unclosed single quote" : "unclosed double quote");
        goto fail;
    }
    if (flush_word(tokens, &builder, &word_started, word_offset) < 0)
        goto out_of_memory;
    return 0;

out_of_memory:
    set_error(error, i, "out of memory while tokenizing command");
fail:
    free(builder.data);
    lexer_tokens_free(tokens);
    return -1;
}
