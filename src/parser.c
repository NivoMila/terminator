#define _POSIX_C_SOURCE 200809L

#include "parser.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
set_error(ParseError *error, size_t offset, const char *message)
{
    if (error == NULL)
        return;
    error->offset = offset;
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

static void
command_free(Command *command)
{
    size_t i;
    for (i = 0; i < command->argc; ++i)
        free(command->argv[i]);
    free(command->argv);
    for (i = 0; i < command->redirection_count; ++i)
        free(command->redirections[i].target);
    free(command->redirections);
    memset(command, 0, sizeof(*command));
}

void
parser_pipeline_free(Pipeline *pipeline)
{
    size_t i;
    if (pipeline == NULL)
        return;
    for (i = 0; i < pipeline->count; ++i)
        command_free(&pipeline->commands[i]);
    free(pipeline->commands);
    memset(pipeline, 0, sizeof(*pipeline));
}

static int
command_add_arg(Command *command, const char *arg)
{
    char **grown;
    char *copy = strdup(arg);
    if (copy == NULL)
        return -1;
    if (command->argc > SIZE_MAX / sizeof(*command->argv) - 2) {
        free(copy);
        return -1;
    }
    grown = realloc(command->argv, (command->argc + 2) * sizeof(*grown));
    if (grown == NULL) {
        free(copy);
        return -1;
    }
    command->argv = grown;
    command->argv[command->argc++] = copy;
    command->argv[command->argc] = NULL;
    return 0;
}

static int
pipeline_add_command(Pipeline *pipeline, Command *command)
{
    if (pipeline->count == pipeline->capacity) {
        size_t next = pipeline->capacity == 0 ? 4 : pipeline->capacity * 2;
        Command *grown;
        if (pipeline->capacity > SIZE_MAX / (2 * sizeof(*pipeline->commands)))
            return -1;
        grown = realloc(pipeline->commands, next * sizeof(*grown));
        if (grown == NULL)
            return -1;
        pipeline->commands = grown;
        pipeline->capacity = next;
    }
    pipeline->commands[pipeline->count++] = *command;
    memset(command, 0, sizeof(*command));
    return 0;
}

static int
finish_command(Pipeline *pipeline, Command *command,
               size_t offset, ParseError *error)
{
    if (command->argc == 0) {
        set_error(error, offset, "empty command around pipe");
        return -1;
    }
    if (pipeline_add_command(pipeline, command) < 0) {
        set_error(error, offset, "out of memory while parsing command");
        return -1;
    }
    return 0;
}

static int
add_redirection(Command *command, TokenType token_type,
                const char *target, size_t offset, ParseError *error)
{
    RedirectionType type;
    Redirection *grown;
    char *target_copy;
    size_t i;

    if (token_type == TOKEN_REDIR_INPUT)
        type = REDIRECTION_INPUT;
    else if (token_type == TOKEN_REDIR_APPEND)
        type = REDIRECTION_OUTPUT_APPEND;
    else
        type = REDIRECTION_OUTPUT_TRUNCATE;

    for (i = 0; i < command->redirection_count; ++i) {
        RedirectionType previous = command->redirections[i].type;
        if ((type == REDIRECTION_INPUT && previous == REDIRECTION_INPUT) ||
            (type != REDIRECTION_INPUT && previous != REDIRECTION_INPUT)) {
            set_error(error, offset, "duplicate redirection for command");
            return -1;
        }
    }
    if (command->redirection_count >= SIZE_MAX / sizeof(*command->redirections)) {
        set_error(error, offset, "out of memory while parsing redirection");
        return -1;
    }
    target_copy = strdup(target);
    if (target_copy == NULL) {
        set_error(error, offset, "out of memory while parsing redirection");
        return -1;
    }
    grown = realloc(command->redirections,
                    (command->redirection_count + 1) * sizeof(*grown));
    if (grown == NULL) {
        free(target_copy);
        set_error(error, offset, "out of memory while parsing redirection");
        return -1;
    }
    command->redirections = grown;
    command->redirections[command->redirection_count++] = (Redirection){
        .type = type,
        .target = target_copy
    };
    return 0;
}

int
parser_parse(const char *input, Pipeline *pipeline, ParseError *error)
{
    TokenList tokens = {0};
    Command command = {0};
    TokenType pending_redirection = TOKEN_WORD;
    size_t pending_offset = 0;
    size_t i;
    int after_pipe = 0;

    if (pipeline == NULL || input == NULL) {
        set_error(error, 0, "invalid parser argument");
        return -1;
    }
    memset(pipeline, 0, sizeof(*pipeline));
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (lexer_tokenize(input, &tokens, error) < 0)
        return -1;

    for (i = 0; i < tokens.count; ++i) {
        Token *token = &tokens.items[i];
        if (pending_redirection != TOKEN_WORD) {
            if (token->type != TOKEN_WORD) {
                set_error(error, token->offset, "redirection requires a target word");
                goto fail;
            }
            if (add_redirection(&command, pending_redirection, token->text,
                                pending_offset, error) < 0)
                goto fail;
            pending_redirection = TOKEN_WORD;
            after_pipe = 0;
            continue;
        }

        if (token->type == TOKEN_WORD) {
            if (command_add_arg(&command, token->text) < 0) {
                set_error(error, token->offset, "out of memory while parsing word");
                goto fail;
            }
            after_pipe = 0;
        } else if (token->type == TOKEN_PIPE) {
            if (finish_command(pipeline, &command, token->offset, error) < 0)
                goto fail;
            after_pipe = 1;
        } else if (token->type == TOKEN_BACKGROUND) {
            if (i + 1 != tokens.count || command.argc == 0) {
                set_error(error, token->offset,
                          "background operator must follow a complete command");
                goto fail;
            }
            pipeline->background = 1;
            after_pipe = 0;
        } else {
            pending_redirection = token->type;
            pending_offset = token->offset;
            after_pipe = 0;
        }
    }

    if (pending_redirection != TOKEN_WORD) {
        set_error(error, pending_offset, "redirection requires a target word");
        goto fail;
    }
    if (command.argc == 0 &&
        (after_pipe || command.redirection_count != 0)) {
        set_error(error, tokens.count == 0 ? 0 : tokens.items[tokens.count - 1].offset,
                  after_pipe ? "empty command after pipe" : "redirection has no command");
        goto fail;
    }
    if (command.argc != 0 && finish_command(pipeline, &command,
                                             strlen(input), error) < 0)
        goto fail;

    lexer_tokens_free(&tokens);
    command_free(&command);
    return 0;

fail:
    lexer_tokens_free(&tokens);
    command_free(&command);
    parser_pipeline_free(pipeline);
    return -1;
}

int
parser_classify_input(const char *input, ParseError *error)
{
    int has_substitution = 0;
    int state = lexer_inspect_input(input, &has_substitution, error);
    if (state != 0)
        return state;
    /* The interactive executor expands substitutions before parsing. Parsing
       their bodies here as outer pipeline syntax would misclassify operators
       inside $(...) as separators belonging to the submitted command. */
    if (has_substitution)
        return 0;
    Pipeline pipeline;
    if (parser_parse(input, &pipeline, error) < 0)
        return -1;
    parser_pipeline_free(&pipeline);
    return 0;
}
