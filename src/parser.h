#ifndef TERMINATOR_PARSER_H
#define TERMINATOR_PARSER_H

#include <stddef.h>

typedef enum {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_REDIR_INPUT,
    TOKEN_REDIR_OUTPUT,
    TOKEN_REDIR_APPEND,
    TOKEN_BACKGROUND
} TokenType;

typedef struct {
    TokenType type;
    char *text;
    size_t offset;
} Token;

typedef struct {
    Token *items;
    size_t count;
    size_t capacity;
} TokenList;

typedef struct {
    size_t offset;
    char message[160];
    int incomplete;
    int line_continuation;
} ParseError;

typedef enum {
    REDIRECTION_INPUT,
    REDIRECTION_OUTPUT_TRUNCATE,
    REDIRECTION_OUTPUT_APPEND
} RedirectionType;

typedef struct {
    RedirectionType type;
    char *target;
} Redirection;

typedef struct {
    char **argv;
    size_t argc;
    Redirection *redirections;
    size_t redirection_count;
} Command;

typedef struct {
    Command *commands;
    size_t count;
    size_t capacity;
    int background;
} Pipeline;

/* Returns zero on success, including for empty/whitespace-only input. */
int lexer_tokenize(const char *input, TokenList *tokens, ParseError *error);
void lexer_tokens_free(TokenList *tokens);
/* Returns 1 for lexically incomplete input, 0 when complete, and -1 on error. */
int lexer_inspect_input(const char *input, int *has_substitution, ParseError *error);

/* Parses words, pipes, redirections, and an optional trailing background '&'. */
int parser_parse(const char *input, Pipeline *pipeline, ParseError *error);
/* Returns 1 for incomplete, 0 for complete, and -1 for invalid input. */
int parser_classify_input(const char *input, ParseError *error);
void parser_pipeline_free(Pipeline *pipeline);

#endif
