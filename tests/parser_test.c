#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
expect_word_args(const char *line, size_t argc, const char *const *expected)
{
    Pipeline pipeline;
    ParseError error;
    size_t i;

    assert(parser_parse(line, &pipeline, &error) == 0);
    assert(pipeline.count == 1);
    assert(pipeline.commands[0].argc == argc);
    for (i = 0; i < argc; ++i)
        assert(strcmp(pipeline.commands[0].argv[i], expected[i]) == 0);
    assert(pipeline.commands[0].argv[argc] == NULL);
    parser_pipeline_free(&pipeline);
    assert(pipeline.commands == NULL && pipeline.count == 0);
}

static void
expect_parse_error(const char *line)
{
    Pipeline pipeline;
    ParseError error;
    assert(parser_parse(line, &pipeline, &error) < 0);
    assert(error.message[0] != '\0');
    parser_pipeline_free(&pipeline);
}

static void
test_lex_words_and_quotes(void)
{
    const char *basic[] = {"echo", "hello", "world"};
    const char *double_quoted[] = {"echo", "hello world"};
    const char *single_quoted[] = {"echo", "hello world"};
    const char *mixed_quotes[] = {"echo", "hello 'world'"};
    const char *escaped_space[] = {"echo", "hello world"};
    const char *joined_segments[] = {"echo", "pre midpost"};
    const char *empty_quoted[] = {"echo", ""};
    const char *empty_single_quoted[] = {"echo", ""};
    const char *requested_join[] = {"echo", "abcdef"};
    const char *double_escapes[] = {"echo", "a\"b\\c"};
    const char *double_preserved_escape[] = {"echo", "a\\qb"};
    const char *single_backslash[] = {"echo", "a\\ b"};

    expect_word_args("echo hello world", 3, basic);
    expect_word_args("echo \"hello world\"", 2, double_quoted);
    expect_word_args("echo 'hello world'", 2, single_quoted);
    expect_word_args("echo \"hello 'world'\"", 2, mixed_quotes);
    expect_word_args("echo hello\\ world", 2, escaped_space);
    expect_word_args("echo pre\" mid\"'post'", 2, joined_segments);
    expect_word_args("echo ab\"cd\"'ef'", 2, requested_join);
    expect_word_args("echo \"\"", 2, empty_quoted);
    expect_word_args("echo ''", 2, empty_single_quoted);
    expect_word_args("echo \"a\\\"b\\\\c\"", 2, double_escapes);
    expect_word_args("echo \"a\\qb\"", 2, double_preserved_escape);
    expect_word_args("echo 'a\\ b'", 2, single_backslash);
}

static void
test_lexer_operators(void)
{
    TokenList tokens;
    ParseError error;
    const char *line = "echo \"a|b>c\" < in | cat >> out";
    const TokenType types[] = {
        TOKEN_WORD, TOKEN_WORD, TOKEN_REDIR_INPUT, TOKEN_WORD,
        TOKEN_PIPE, TOKEN_WORD, TOKEN_REDIR_APPEND, TOKEN_WORD
    };
    const char *words[] = {"echo", "a|b>c", NULL, "in", NULL, "cat", NULL, "out"};
    size_t i;

    assert(lexer_tokenize(line, &tokens, &error) == 0);
    assert(tokens.count == sizeof(types) / sizeof(types[0]));
    for (i = 0; i < tokens.count; ++i) {
        assert(tokens.items[i].type == types[i]);
        if (words[i] != NULL)
            assert(strcmp(tokens.items[i].text, words[i]) == 0);
    }
    lexer_tokens_free(&tokens);

    assert(lexer_tokenize("sleep 1 &", &tokens, &error) == 0);
    assert(tokens.count == 3 && tokens.items[2].type == TOKEN_BACKGROUND);
    lexer_tokens_free(&tokens);

    assert(lexer_tokenize("   \t\n", &tokens, &error) == 0);
    assert(tokens.count == 0);
    lexer_tokens_free(&tokens);
}

static void
test_pipeline_and_redirection_parse(void)
{
    Pipeline pipeline;
    ParseError error;

    assert(parser_parse("sort < source.txt | grep 'two words' >> result.txt",
                        &pipeline, &error) == 0);
    assert(pipeline.count == 2);
    assert(pipeline.commands[0].argc == 1);
    assert(strcmp(pipeline.commands[0].argv[0], "sort") == 0);
    assert(pipeline.commands[0].redirection_count == 1);
    assert(pipeline.commands[0].redirections[0].type == REDIRECTION_INPUT);
    assert(strcmp(pipeline.commands[0].redirections[0].target, "source.txt") == 0);
    assert(pipeline.commands[1].argc == 2);
    assert(strcmp(pipeline.commands[1].argv[0], "grep") == 0);
    assert(strcmp(pipeline.commands[1].argv[1], "two words") == 0);
    assert(pipeline.commands[1].redirection_count == 1);
    assert(pipeline.commands[1].redirections[0].type == REDIRECTION_OUTPUT_APPEND);
    assert(strcmp(pipeline.commands[1].redirections[0].target, "result.txt") == 0);
    parser_pipeline_free(&pipeline);

    assert(parser_parse("cat < in > out", &pipeline, &error) == 0);
    assert(pipeline.count == 1);
    assert(pipeline.commands[0].redirection_count == 2);
    assert(pipeline.commands[0].redirections[0].type == REDIRECTION_INPUT);
    assert(strcmp(pipeline.commands[0].redirections[0].target, "in") == 0);
    assert(pipeline.commands[0].redirections[1].type == REDIRECTION_OUTPUT_TRUNCATE);
    assert(strcmp(pipeline.commands[0].redirections[1].target, "out") == 0);
    parser_pipeline_free(&pipeline);

    assert(parser_parse("cat >> out < in", &pipeline, &error) == 0);
    assert(pipeline.commands[0].redirection_count == 2);
    assert(pipeline.commands[0].redirections[0].type == REDIRECTION_OUTPUT_APPEND);
    assert(strcmp(pipeline.commands[0].redirections[0].target, "out") == 0);
    assert(pipeline.commands[0].redirections[1].type == REDIRECTION_INPUT);
    assert(strcmp(pipeline.commands[0].redirections[1].target, "in") == 0);
    parser_pipeline_free(&pipeline);

    assert(parser_parse("printf hello | wc -c &", &pipeline, &error) == 0);
    assert(pipeline.background && pipeline.count == 2);
    parser_pipeline_free(&pipeline);
    assert(parser_parse("echo '&'", &pipeline, &error) == 0);
    assert(!pipeline.background && strcmp(pipeline.commands[0].argv[1], "&") == 0);
    parser_pipeline_free(&pipeline);

    assert(parser_parse(" \t  ", &pipeline, &error) == 0);
    assert(pipeline.count == 0);
    parser_pipeline_free(&pipeline);
}

static void
test_syntax_errors(void)
{
    const char *invalid[] = {
        "| cat",
        "cat |",
        "cat || wc",
        "cat >",
        "cat < | wc",
        "cat > | wc",
        "cat < in < other",
        "cat > out >> more",
        "> file",
        "cat | > out",
        "cat >>> out",
        "cat <<< input",
        "&",
        "echo & cat",
        "echo & | cat",
        "echo | &",
        "echo 'unclosed",
        "echo \"unclosed",
        "echo trailing\\"
    };
    size_t i;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        expect_parse_error(invalid[i]);
}

static void
test_memory_management_repetition(void)
{
    size_t i;
    for (i = 0; i < 3000; ++i) {
        Pipeline pipeline;
        ParseError error;
        TokenList tokens;
        assert(lexer_tokenize("echo 'a b' | cat > out", &tokens, &error) == 0);
        lexer_tokens_free(&tokens);
        assert(parser_parse("echo 'a b' | cat > out", &pipeline, &error) == 0);
        parser_pipeline_free(&pipeline);
        expect_parse_error("echo \"unfinished");
    }
}

int
main(void)
{
    test_lex_words_and_quotes();
    test_lexer_operators();
    test_pipeline_and_redirection_parse();
    test_syntax_errors();
    test_memory_management_repetition();
    puts("lexer/parser tests passed");
    return EXIT_SUCCESS;
}
