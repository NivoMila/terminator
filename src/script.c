#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "script.h"
#include "shell_internal.h"
#include "parser.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum { ST_COMMAND, ST_ASSIGN, ST_IF, ST_EXIT } StatementType;
typedef struct Statement Statement;
typedef struct { Statement **items; size_t count, capacity; } StatementList;
typedef struct { char *test; StatementList body; size_t line; } IfBranch;

struct Statement {
    StatementType type;
    size_t line;
    union {
        char *text;
        struct { char *name; char *value; } assignment;
        struct { IfBranch *branches; size_t count, capacity; StatementList otherwise; } conditional;
        int exit_status;
    } value;
};

struct ScriptProgram { StatementList body; };

typedef struct { char *text; size_t line; } SourceUnit;
typedef struct { SourceUnit *items; size_t count, capacity; } UnitList;

static void statement_free(Statement *statement);

static int
error_set(ScriptError *error, size_t line, const char *message)
{
    if (error != NULL) {
        error->line = line;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
    errno = EINVAL;
    return -1;
}

static char *
trim_copy(const char *text, size_t length)
{
    size_t begin = 0;
    while (begin < length && isspace((unsigned char)text[begin])) ++begin;
    while (length > begin && isspace((unsigned char)text[length - 1])) --length;
    return strndup(text + begin, length - begin);
}

static size_t
line_for_nested_error(size_t base_line, const char *text, size_t offset,
                      size_t nested_line)
{
    size_t line = base_line;
    for (size_t i = 0; i < offset && text[i] != '\0'; ++i)
        if (text[i] == '\n') ++line;
    if (nested_line > 1) line += nested_line - 1;
    return line;
}

static int
unit_add(UnitList *units, const char *text, size_t length, size_t line)
{
    char *copy = trim_copy(text, length);
    if (copy == NULL) return -1;
    if (*copy == '\0' || copy[0] == '#') { free(copy); return 0; }
    if (units->count == units->capacity) {
        size_t cap = units->capacity == 0 ? 16 : units->capacity * 2;
        SourceUnit *items = realloc(units->items, cap * sizeof(*items));
        if (items == NULL) { free(copy); return -1; }
        units->items = items; units->capacity = cap;
    }
    units->items[units->count++] = (SourceUnit){copy, line};
    return 0;
}

static void
units_free(UnitList *units)
{
    for (size_t i = 0; i < units->count; ++i) free(units->items[i].text);
    free(units->items);
    memset(units, 0, sizeof(*units));
}

/* Split only at unquoted top-level separators. Quotes and substitutions may
 * span lines; escaped newlines are continuations. */
static int
split_units(const char *text, UnitList *units, ScriptError *error)
{
    size_t capacity = 128, used = 0, line = 1, start_line = 1;
    char *buffer = malloc(capacity);
    char quote = 0;
    unsigned substitution_depth = 0;
    int in_backticks = 0, escaped = 0;
    if (buffer == NULL) return error_set(error, 1, "out of memory");
    for (size_t i = 0;; ++i) {
        char c = text[i];
        if (c == '\0') {
            if (quote != 0) { free(buffer); return error_set(error, start_line, "unclosed quote"); }
            if (in_backticks || substitution_depth != 0) {
                free(buffer); return error_set(error, start_line, "unclosed command substitution");
            }
            if (unit_add(units, buffer, used, start_line) < 0) { free(buffer); return error_set(error, line, "out of memory"); }
            break;
        }
        if (c == '#' && quote == 0 && substitution_depth == 0 && !in_backticks &&
            (used == 0 || isspace((unsigned char)buffer[used - 1]))) {
            while (text[i] != '\0' && text[i] != '\n') ++i;
            c = text[i];
            if (c == '\0') {
                if (unit_add(units, buffer, used, start_line) < 0) { free(buffer); return error_set(error, line, "out of memory"); }
                break;
            }
        }
        if (escaped) {
            escaped = 0;
            if (c == '\n' && quote != '\'') { ++line; continue; }
        } else if (c == '\\' && quote != '\'') {
            escaped = 1;
        } else if (in_backticks) {
            if (c == '`') in_backticks = 0;
        } else if (quote == '\'') {
            if (c == '\'') quote = 0;
        } else {
            if (c == '"' || c == '\'') quote = quote == 0 ? c : (quote == c ? 0 : quote);
            else if (c == '`') in_backticks = 1;
            else if (quote != '\'' && c == '$' && text[i + 1] == '(') ++substitution_depth;
            else if (substitution_depth != 0 && c == '(' && (i == 0 || text[i - 1] != '$')) ++substitution_depth;
            else if (substitution_depth != 0 && c == ')') --substitution_depth;
        }
        if (c == '\n') {
            if (quote == 0 && substitution_depth == 0 && !in_backticks) {
                if (unit_add(units, buffer, used, start_line) < 0) { free(buffer); return error_set(error, line, "out of memory"); }
                used = 0; start_line = line + 1;
            } else {
                if (used + 1 >= capacity) { capacity *= 2; char *p = realloc(buffer, capacity); if (!p) { free(buffer); return error_set(error, line, "out of memory"); } buffer = p; }
                buffer[used++] = c;
            }
            ++line;
            continue;
        }
        if (c == ';' && quote == 0 && substitution_depth == 0 && !in_backticks) {
            if (unit_add(units, buffer, used, start_line) < 0) { free(buffer); return error_set(error, line, "out of memory"); }
            used = 0; start_line = line; continue;
        }
        if (used + 1 >= capacity) {
            if (capacity > (size_t)-1 / 2) { free(buffer); return error_set(error, line, "script is too large"); }
            capacity *= 2; char *p = realloc(buffer, capacity);
            if (p == NULL) { free(buffer); return error_set(error, line, "out of memory"); }
            buffer = p;
        }
        buffer[used++] = c;
    }
    free(buffer);
    return 0;
}

static int
starts_keyword(const char *text, const char *keyword)
{
    size_t n = strlen(keyword);
    return strncmp(text, keyword, n) == 0 &&
           (text[n] == '\0' || isspace((unsigned char)text[n]));
}

static int
assignment_is_one_word(const char *text)
{
    size_t words = 0;
    char quote = 0;
    int escaped = 0, backticks = 0, in_word = 0;
    unsigned substitutions = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        char c = text[i];
        if (escaped) { escaped=0; in_word=1; continue; }
        if (c=='\\' && quote!='\'') { escaped=1; in_word=1; continue; }
        if (backticks) { if(c=='`')backticks=0; continue; }
        if (quote=='\'') { if(c=='\'')quote=0; continue; }
        if (c=='\'' || c=='"') { if(quote==0)quote=c;else if(quote==c)quote=0;in_word=1;continue; }
        if (c=='`' && quote!='\'') { backticks=1; in_word=1; continue; }
        if (c=='$' && text[i+1]=='(' && quote!='\'') { ++substitutions; ++i; in_word=1; continue; }
        if (substitutions && c=='(' && (i==0 || text[i-1]!='$')) { ++substitutions; in_word=1; continue; }
        if (substitutions && c==')') { --substitutions; in_word=1; continue; }
        if (substitutions) { in_word=1; continue; }
        if (quote == '"') { in_word=1; continue; }
        if (isspace((unsigned char)c)) { if(in_word){++words;in_word=0;} continue; }
        if (c=='|' || c=='<' || c=='>' || c=='&' || c==';') return 0;
        in_word=1;
    }
    if (in_word) ++words;
    return words <= 1;
}

static int
list_add(StatementList *list, Statement *statement)
{
    if (list->count == list->capacity) {
        size_t cap = list->capacity == 0 ? 8 : list->capacity * 2;
        Statement **items = realloc(list->items, cap * sizeof(*items));
        if (items == NULL) return -1;
        list->items = items; list->capacity = cap;
    }
    list->items[list->count++] = statement;
    return 0;
}

static void
list_free(StatementList *list)
{
    for (size_t i = 0; i < list->count; ++i) statement_free(list->items[i]);
    free(list->items); memset(list, 0, sizeof(*list));
}

static void
statement_free(Statement *s)
{
    if (s == NULL) return;
    if (s->type == ST_COMMAND) free(s->value.text);
    else if (s->type == ST_ASSIGN) { free(s->value.assignment.name); free(s->value.assignment.value); }
    else if (s->type == ST_IF) {
        for (size_t i = 0; i < s->value.conditional.count; ++i) {
            free(s->value.conditional.branches[i].test);
            list_free(&s->value.conditional.branches[i].body);
        }
        free(s->value.conditional.branches);
        list_free(&s->value.conditional.otherwise);
    }
    free(s);
}

static int parse_list(UnitList *, size_t *, StatementList *, int, ScriptError *);
static int validate_test(const char *, size_t, ScriptError *);
static int validate_command(const char *, size_t, ScriptError *, unsigned);
static int script_parse_depth(const char *, ScriptProgram **, ScriptError *, unsigned);

static int
branch_add(Statement *s, char *test, size_t line)
{
    if (s->value.conditional.count == s->value.conditional.capacity) {
        size_t cap = s->value.conditional.capacity == 0 ? 2 : s->value.conditional.capacity * 2;
        IfBranch *items = realloc(s->value.conditional.branches, cap * sizeof(*items));
        if (items == NULL) return -1;
        s->value.conditional.branches = items; s->value.conditional.capacity = cap;
    }
    s->value.conditional.branches[s->value.conditional.count++] = (IfBranch){.test=test, .line=line};
    return 0;
}

static int
consume_then(UnitList *units, size_t *index, size_t line, ScriptError *error)
{
    if (*index >= units->count || !starts_keyword(units->items[*index].text, "then"))
        return error_set(error, line, "expected 'then'");
    SourceUnit *unit = &units->items[*index];
    if (strcmp(unit->text, "then") == 0) {
        ++*index;
        return 0;
    }
    char *remainder = trim_copy(unit->text + 4, strlen(unit->text + 4));
    if (remainder == NULL) return error_set(error, unit->line, "out of memory");
    free(unit->text);
    unit->text = remainder;
    return 0;
}

static int
parse_if(UnitList *units, size_t *index, Statement **out, ScriptError *error)
{
    SourceUnit *first = &units->items[*index];
    Statement *s = calloc(1, sizeof(*s));
    char *test = trim_copy(first->text + 2, strlen(first->text + 2));
    if (s == NULL || test == NULL) { free(s); free(test); return error_set(error, first->line, "out of memory"); }
    s->type = ST_IF; s->line = first->line;
    if (*test == '\0') { free(test); statement_free(s); return error_set(error, first->line, "if requires a test"); }
    char *then_suffix = strstr(test, " then");
    if (then_suffix != NULL && then_suffix[5] == '\0') *then_suffix = '\0';
    if (validate_test(test, first->line, error) < 0 || branch_add(s, test, first->line) < 0) {
        if (s->value.conditional.count == 0) free(test);
        statement_free(s); return -1;
    }
    ++*index;
    if (then_suffix == NULL) {
        if (consume_then(units, index, first->line, error) < 0) { statement_free(s); return -1; }
    }
    for (;;) {
        IfBranch *branch = &s->value.conditional.branches[s->value.conditional.count - 1];
        if (parse_list(units, index, &branch->body, 1, error) < 0) { statement_free(s); return -1; }
        if (*index >= units->count) { statement_free(s); return error_set(error, first->line, "missing 'fi'"); }
        SourceUnit *unit = &units->items[*index];
        if (starts_keyword(unit->text, "elif")) {
            char *next_test = trim_copy(unit->text + 4, strlen(unit->text + 4));
            if (next_test == NULL) { statement_free(s); return error_set(error, unit->line, "out of memory"); }
            char *suffix = strstr(next_test, " then");
            if (suffix != NULL && suffix[5] == '\0') *suffix = '\0';
            if (*next_test == '\0' || validate_test(next_test, unit->line, error) < 0 || branch_add(s, next_test, unit->line) < 0) {
                int has_error = error != NULL && error->message[0] != '\0';
                free(next_test); statement_free(s); return has_error ? -1 : error_set(error, unit->line, "invalid elif test");
            }
            ++*index;
            if (suffix == NULL) {
                if (consume_then(units, index, unit->line, error) < 0) { statement_free(s); return -1; }
            }
            continue;
        }
        if (starts_keyword(unit->text, "else")) {
            if (strcmp(unit->text, "else") == 0) ++*index;
            else {
                char *remainder=trim_copy(unit->text+4,strlen(unit->text+4));
                if(!remainder){statement_free(s);return error_set(error,unit->line,"out of memory");}
                free(unit->text);unit->text=remainder;
            }
            if (parse_list(units, index, &s->value.conditional.otherwise, 1, error) < 0) { statement_free(s); return -1; }
            if (*index >= units->count || strcmp(units->items[*index].text, "fi") != 0) { statement_free(s); return error_set(error, unit->line, "missing 'fi'"); }
            ++*index; break;
        }
        if (strcmp(unit->text, "fi") == 0) { ++*index; break; }
        statement_free(s); return error_set(error, unit->line, "expected 'elif', 'else', or 'fi'");
    }
    *out = s;
    return 0;
}

static int
parse_list(UnitList *units, size_t *index, StatementList *list, int in_if, ScriptError *error)
{
    while (*index < units->count) {
        SourceUnit *unit = &units->items[*index];
        Statement *s = NULL;
        if (strcmp(unit->text, "fi") == 0 || starts_keyword(unit->text, "else") || starts_keyword(unit->text, "elif"))
            return in_if ? 0 : error_set(error, unit->line, "unexpected conditional keyword");
        if (strcmp(unit->text, "then") == 0) return error_set(error, unit->line, "unexpected 'then'");
        if (starts_keyword(unit->text, "if")) {
            if (parse_if(units, index, &s, error) < 0) return -1;
        } else {
            s = calloc(1, sizeof(*s));
            if (s == NULL) return error_set(error, unit->line, "out of memory");
            s->line = unit->line;
            if (starts_keyword(unit->text, "exit")) {
                char *arg = unit->text + 4;
                while (*arg == ' ' || *arg == '\t') ++arg;
                char *end = NULL;
                long value = 0;
                if (*arg != '\0') {
                    errno = 0; value = strtol(arg, &end, 10);
                    if (errno != 0 || end == arg || *end != '\0' || value < 0 || value > 255) {
                        free(s); return error_set(error, unit->line, "exit expects an integer from 0 to 255");
                    }
                }
                s->type = ST_EXIT; s->value.exit_status = (int)value;
                ++*index;
            } else {
                char *equal = strchr(unit->text, '=');
                if (equal != NULL && equal != unit->text &&
                    (isalpha((unsigned char)unit->text[0]) || unit->text[0] == '_')) {
                    int valid = 1;
                    for (char *p = unit->text; p < equal; ++p)
                        if (!(isalnum((unsigned char)*p) || *p == '_') || (p == unit->text && isdigit((unsigned char)*p))) valid = 0;
                    if (valid) {
                        s->type = ST_ASSIGN;
                        s->value.assignment.name = strndup(unit->text, (size_t)(equal - unit->text));
                        s->value.assignment.value = strdup(equal + 1);
                        if (!s->value.assignment.name || !s->value.assignment.value) { statement_free(s); return error_set(error, unit->line, "out of memory"); }
                        size_t command_len = strlen(s->value.assignment.value) + 6;
                        char *validation = malloc(command_len);
                        if (validation == NULL) { statement_free(s); return error_set(error, unit->line, "out of memory"); }
                        (void)snprintf(validation, command_len, "echo %s", s->value.assignment.value);
                        int valid = validate_command(validation, unit->line, error, 0);
                        free(validation);
                        if (valid < 0) { statement_free(s); return -1; }
                        if (!assignment_is_one_word(s->value.assignment.value)) {
                            statement_free(s);
                            return error_set(error, unit->line, "assignment value must be one word");
                        }
                        ++*index;
                    } else { free(s); s = NULL; }
                } else { free(s); s = NULL; }
                if (s == NULL) {
                    s = calloc(1, sizeof(*s));
                    if (s == NULL) return error_set(error, unit->line, "out of memory");
                    s->type = ST_COMMAND; s->line = unit->line;
                    s->value.text = strdup(unit->text);
                    if (s->value.text == NULL || validate_command(unit->text, unit->line, error, 0) < 0) { statement_free(s); return -1; }
                    ++*index;
                }
            }
        }
        if (list_add(list, s) < 0) { statement_free(s); return error_set(error, unit->line, "out of memory"); }
    }
    if (in_if) return 0;
    return 0;
}

static int
script_parse_depth(const char *text, ScriptProgram **program, ScriptError *error, unsigned depth)
{
    UnitList units = {0};
    ScriptProgram *result;
    size_t index = 0;
    if (depth > 64) return error_set(error, 1, "command substitution nesting is too deep");
    result = calloc(1, sizeof(*result));
    if (result == NULL) return error_set(error, 1, "out of memory");
    if (split_units(text, &units, error) < 0 || parse_list(&units, &index, &result->body, 0, error) < 0) {
        units_free(&units); script_program_free(result); return -1;
    }
    units_free(&units); *program = result; return 0;
}

int
script_parse(const char *text, ScriptProgram **program, ScriptError *error)
{
    if (text == NULL || program == NULL) return error_set(error, 1, "invalid parser input");
    *program = NULL;
    if (error != NULL) memset(error, 0, sizeof(*error));
    return script_parse_depth(text, program, error, 0);
}

void
script_program_free(ScriptProgram *program)
{
    if (program != NULL) { list_free(&program->body); free(program); }
}

const char *
script_variable_get(Shell *shell, const char *name)
{
    for (ShellVariable *v = shell->variables; v != NULL; v = v->next)
        if (strcmp(v->name, name) == 0) return v->value;
    const char *environment_value = getenv(name);
    return environment_value == NULL ? "" : environment_value;
}

static int
variable_set(Shell *shell, const char *name, const char *value)
{
    ShellVariable *v;
    for (v = shell->variables; v != NULL; v = v->next)
        if (strcmp(v->name, name) == 0) break;
    if (v == NULL) {
        v = calloc(1, sizeof(*v)); if (v == NULL) return -1;
        v->name = strdup(name); if (v->name == NULL) { free(v); return -1; }
        v->next = shell->variables; shell->variables = v;
    }
    char *copy = strdup(value); if (copy == NULL) return -1;
    free(v->value); v->value = copy; return 0;
}

static void
variables_free(ShellVariable *v)
{
    while (v != NULL) { ShellVariable *next=v->next; free(v->name); free(v->value); free(v); v=next; }
}

static ShellVariable *
variables_clone(ShellVariable *source)
{
    ShellVariable *copy = NULL, **tail = &copy;
    for (; source != NULL; source = source->next) {
        ShellVariable *v = calloc(1, sizeof(*v));
        if (v == NULL || (v->name = strdup(source->name)) == NULL || (v->value = strdup(source->value)) == NULL) {
            if (v != NULL) { free(v->name); free(v->value); free(v); }
            variables_free(copy); return NULL;
        }
        *tail = v; tail = &v->next;
    }
    return copy;
}

/* Append an expansion in a form the existing lexer treats as data rather
 * than as shell syntax. Whitespace remains split only when unquoted. */
static int
append_escaped(char **out, size_t *length, size_t *capacity, const char *value,
               int quoted, int preserve_spaces)
{
    for (const char *p = value; *p != '\0'; ++p) {
        int escape = quoted ? (*p == '\\' || *p == '"' || *p == '$' || *p == '`') :
                     (*p == '\\' || *p == '"' || *p == '\'' || *p == '|' || *p == '<' || *p == '>' || *p == '&' || *p == ';' ||
                      (preserve_spaces && isspace((unsigned char)*p)));
        if (*length + (size_t)escape + 2 > *capacity) {
            size_t cap = *capacity ? *capacity : 64;
            while (cap < *length + (size_t)escape + 2) cap *= 2;
            char *grown = realloc(*out, cap); if (!grown) return -1; *out = grown; *capacity = cap;
        }
        if (escape) (*out)[(*length)++] = '\\';
        (*out)[(*length)++] = *p;
    }
    return 0;
}

static int
expand_text(Shell *, const char *, char **, int *, int, unsigned);

static int
run_substitution(Shell *shell, const char *text, size_t line, char **result, unsigned depth)
{
    ScriptProgram *program = NULL;
    ScriptError error;
    char *saved_capture = shell->capture_buffer;
    size_t saved_length = shell->capture_length, saved_capacity = shell->capture_capacity;
    int saved_capturing = shell->capturing_output, saved_failed = shell->capture_failed;
    int saved_cwd = fcntl(shell->cwd_fd, F_DUPFD_CLOEXEC, 3);
    char *saved_path = strdup(shell->cwd_path);
    ShellVariable *saved_variables = variables_clone(shell->variables);
    int had_variables = shell->variables != NULL;
    int status = 0;
    if (saved_cwd < 0 || saved_path == NULL || (had_variables && saved_variables == NULL)) {
        if (saved_cwd >= 0) close(saved_cwd);
        free(saved_path); variables_free(saved_variables); return -1;
    }
    shell->capture_buffer = NULL; shell->capture_length = shell->capture_capacity = 0;
    shell->capturing_output = 1; shell->capture_failed = 0;
    if (script_parse_depth(text, &program, &error, depth + 1) < 0) {
        shell->capture_buffer = saved_capture; shell->capture_length = saved_length; shell->capture_capacity = saved_capacity;
        shell->capturing_output = saved_capturing; shell->capture_failed = saved_failed;
        close(shell->cwd_fd); shell->cwd_fd=saved_cwd; free(shell->cwd_path); shell->cwd_path=saved_path;
        variables_free(shell->variables); shell->variables=saved_variables;
        char message[256];
        (void)snprintf(message, sizeof(message), "command substitution syntax error at line %zu: %s", error.line, error.message);
        (void)shell_emit(shell, SHELL_EVENT_OUTPUT, message, strlen(message), 0);
        (void)line; *result = strdup(""); return *result == NULL ? -1 : 0;
    }
    (void)script_execute(shell, program, (int)shell->script_argc, shell->script_args, &status);
    script_program_free(program);
    if (shell->capture_buffer == NULL) shell->capture_buffer = strdup("");
    if (shell->capture_buffer == NULL) {
        shell->capture_buffer=saved_capture; shell->capture_length=saved_length; shell->capture_capacity=saved_capacity;
        shell->capturing_output=saved_capturing; shell->capture_failed=saved_failed;
        close(shell->cwd_fd); shell->cwd_fd=saved_cwd; free(shell->cwd_path); shell->cwd_path=saved_path;
        variables_free(shell->variables); shell->variables=saved_variables; return -1;
    }
    if (shell->capture_failed) {
        free(shell->capture_buffer); shell->capture_buffer = saved_capture; shell->capture_length = saved_length; shell->capture_capacity = saved_capacity;
        shell->capturing_output=saved_capturing; shell->capture_failed=saved_failed;
        close(shell->cwd_fd); shell->cwd_fd=saved_cwd; free(shell->cwd_path); shell->cwd_path=saved_path;
        variables_free(shell->variables); shell->variables=saved_variables; errno=ENOMEM; return -1;
    }
    while (shell->capture_length > 0 && shell->capture_buffer[shell->capture_length - 1] == '\n')
        shell->capture_buffer[--shell->capture_length] = '\0';
    *result = shell->capture_buffer;
    shell->capture_buffer = saved_capture; shell->capture_length = saved_length; shell->capture_capacity = saved_capacity;
    shell->capturing_output = saved_capturing; shell->capture_failed = saved_failed;
    close(shell->cwd_fd); shell->cwd_fd=saved_cwd; free(shell->cwd_path); shell->cwd_path=saved_path;
    variables_free(shell->variables); shell->variables=saved_variables;
    return 0;
}

static char *
translate_nested_backticks(char *text)
{
    for (;;) {
        char *first = NULL, *second = NULL;
        size_t best = (size_t)-1;
        for (char *p = text; (p = strstr(p, "\\`")) != NULL; ++p) {
            char *q = strstr(p + 2, "\\`");
            if (q != NULL && (size_t)(q - p) < best) { first = p; second = q; best = (size_t)(q - p); }
        }
        if (first == NULL) return text;
        size_t prefix = (size_t)(first - text), middle = (size_t)(second - first) - 2;
        size_t suffix = strlen(second + 2);
        char *next = malloc(prefix + 2 + middle + 1 + suffix + 1);
        if (next == NULL) { free(text); return NULL; }
        memcpy(next, text, prefix);
        memcpy(next + prefix, "$(", 2);
        memcpy(next + prefix + 2, first + 2, middle);
        next[prefix + 2 + middle] = ')';
        memcpy(next + prefix + 3 + middle, second + 2, suffix + 1);
        free(text); text = next;
    }
}

static int
expand_text(Shell *shell, const char *source, char **result, int *quoted_result,
            int preserve_expansion_spaces, unsigned depth)
{
    size_t capacity = 0, length = 0;
    char *out = NULL;
    int quote = 0, escaped = 0, quoted_empty = 0;
    if (depth > 64) { errno = ELOOP; return -1; }
    for (size_t i = 0; source[i] != '\0';) {
        char c = source[i];
        if (!escaped && c == '\'' && quote != '"') { quote = quote == '\'' ? 0 : '\''; if (length + 2 >= capacity) { capacity = capacity ? capacity * 2 : 64; out = realloc(out, capacity); if (!out) return -1; } out[length++] = c; ++i; continue; }
        if (!escaped && c == '"' && quote != '\'') { quote = quote == '"' ? 0 : '"'; if (length + 2 >= capacity) { capacity = capacity ? capacity * 2 : 64; out = realloc(out, capacity); if (!out) return -1; } out[length++] = c; ++i; continue; }
        if (quote != '\'' && !escaped && c == '$') {
            char value[64]; const char *replacement = NULL; char *inner_command = NULL;
            char *substitution_output = NULL; size_t consumed = 1;
            if (source[i + 1] == '(') {
                size_t j = i + 2; unsigned nest = 1; int q = 0, esc = 0;
                for (; source[j] != '\0'; ++j) {
                    char d = source[j];
                    if (esc) { esc = 0; continue; }
                    if (d == '\\' && q != '\'') { esc = 1; continue; }
                    if (q == '\'') { if (d == '\'') q = 0; continue; }
                    if (d == '\'' || d == '"') { if (q == 0) q = d; else if (q == d) q = 0; continue; }
                    if (d == '$' && source[j + 1] == '(') { ++nest; ++j; continue; }
                    if (d == ')' && --nest == 0) break;
                }
                if (source[j] != ')') { free(out); errno = EINVAL; return -1; }
                inner_command = strndup(source + i + 2, j - i - 2);
                if (!inner_command || run_substitution(shell, inner_command, 1, &substitution_output, depth) < 0) { free(inner_command); free(out); return -1; }
                free(inner_command); replacement = substitution_output; consumed = j - i + 1;
            } else {
                size_t j = i + 1;
                if (source[j] == '?') { (void)snprintf(value, sizeof(value), "%d", shell->script_status); replacement = value; consumed = 2; }
                else if (source[j] == '#') { (void)snprintf(value, sizeof(value), "%zu", shell->script_argc); replacement = value; consumed = 2; }
                else if (isdigit((unsigned char)source[j])) {
                    unsigned long index = 0;
                    while (isdigit((unsigned char)source[j])) { index = index * 10 + (unsigned)(source[j] - '0'); ++j; }
                    replacement = index > 0 && index <= shell->script_argc ? shell->script_args[index - 1] : "";
                    consumed = j - i;
                } else if (source[j] == '@') {
                    for (size_t a = 0; a < shell->script_argc; ++a) {
                        if (a && quote == '"') {
                            if (length + 4 > capacity) { size_t cap=capacity?capacity*2:64;while(cap<length+4)cap*=2;char*p=realloc(out,cap);if(!p){free(out);return -1;}out=p;capacity=cap; }
                            out[length++]='"';out[length++]=' ';out[length++]='"';
                        } else if (a && append_escaped(&out, &length, &capacity, " ", 0, preserve_expansion_spaces) < 0) { free(out); return -1; }
                        if (append_escaped(&out, &length, &capacity, shell->script_args[a], quote == '"', preserve_expansion_spaces) < 0) { free(out); return -1; }
                    }
                    consumed = 2; i += consumed; continue;
                } else if (isalpha((unsigned char)source[j]) || source[j] == '_') {
                    size_t k = j + 1; while (isalnum((unsigned char)source[k]) || source[k] == '_') ++k;
                    char *name = strndup(source + j, k - j); if (!name) { free(out); return -1; }
                    replacement = script_variable_get(shell, name); free(name); consumed = k - i;
                } else replacement = "$";
            }
            if (replacement != NULL && append_escaped(&out, &length, &capacity, replacement, quote == '"', preserve_expansion_spaces) < 0) { free(substitution_output); free(out); return -1; }
            free(substitution_output);
            i += consumed; escaped = 0; continue;
        }
        if (c == '`' && quote != '\'' && !escaped) {
            size_t j = i + 1; int esc = 0;
            while (source[j] != '\0') { if (!esc && source[j] == '`') break; if (!esc && source[j] == '\\') esc = 1; else esc = 0; ++j; }
            if (source[j] != '`') { free(out); errno = EINVAL; return -1; }
            char *inner = strndup(source + i + 1, j - i - 1), *replacement = NULL;
            if (inner != NULL) inner = translate_nested_backticks(inner);
            if (!inner || run_substitution(shell, inner, 1, &replacement, depth) < 0) { free(inner); free(out); return -1; }
            free(inner);
            if (append_escaped(&out, &length, &capacity, replacement, quote == '"', preserve_expansion_spaces) < 0) { free(replacement); free(out); return -1; }
            free(replacement); i = j + 1; continue;
        }
        if (!escaped && c == '\\' && quote != '\'') escaped = 1;
        else escaped = 0;
        if (quote == '"' && c == '"') quoted_empty = 1;
        if (length + 2 >= capacity) { size_t cap = capacity ? capacity * 2 : 64; char *p = realloc(out, cap); if (!p) { free(out); return -1; } out = p; capacity = cap; }
        out[length++] = c; ++i;
    }
    if (quote != 0) { free(out); errno = EINVAL; return -1; }
    if (out == NULL) { out = strdup(""); if (!out) return -1; }
    out[length] = '\0'; *result = out; if (quoted_result) *quoted_result = quoted_empty;
    return 0;
}

int
script_expand_command(Shell *shell, const char *command, char **expanded)
{
    return expand_text(shell, command, expanded, NULL, 0, 0);
}

static int
validate_test(const char *text, size_t line, ScriptError *error)
{
    const char *begin = text, *end = text + strlen(text);
    if (strncmp(begin, "[[", 2) == 0) { begin += 2; while (isspace((unsigned char)*begin)) ++begin; }
    else if (*begin == '[') { ++begin; while (isspace((unsigned char)*begin)) ++begin; }
    else return error_set(error, line, "if/elif condition must use [ ... ] or [[ ... ]]");
    while (end > begin && isspace((unsigned char)end[-1])) --end;
    if (end - begin >= 2 && end[-2] == ']' && end[-1] == ']') end -= 2;
    else if (end > begin && end[-1] == ']') --end;
    else return error_set(error, line, "missing closing test bracket");
    char *inside = strndup(begin, (size_t)(end - begin));
    if (!inside) return error_set(error, line, "out of memory");
    char quote = 0;
    int escaped = 0;
    for (size_t i = 0; inside[i] != '\0'; ++i) {
        if (escaped) { escaped = 0; continue; }
        if (inside[i] == '\\' && quote != '\'') { escaped = 1; continue; }
        if (quote == '\'') { if (inside[i] == '\'') quote = 0; continue; }
        if (inside[i] == '\'' || inside[i] == '"') { if (quote == 0) quote = inside[i]; else if (quote == inside[i]) quote = 0; continue; }
        size_t start = i, finish = i;
        if (inside[i] == '$' && inside[i + 1] == '(') {
            unsigned nest = 1; char q = 0; int esc = 0;
            for (finish = i + 2; inside[finish] != '\0'; ++finish) {
                char c = inside[finish];
                if (esc) { esc=0; continue; }
                if (c=='\\' && q!='\'') { esc=1; continue; }
                if (q=='\'') { if(c=='\'') q=0; continue; }
                if(c=='\''||c=='"'){if(q==0)q=c;else if(q==c)q=0;continue;}
                if(c=='$'&&inside[finish+1]=='('){++nest;++finish;continue;}
                if(c==')'&&--nest==0)break;
            }
            if (inside[finish] != ')') { free(inside); return error_set(error,line,"unclosed command substitution in test"); }
            char *nested_text=strndup(inside+i+2,finish-i-2);ScriptProgram *nested=NULL;
            if(!nested_text||script_parse_depth(nested_text,&nested,error,1)<0){
                if (error != NULL) error->line = line_for_nested_error(line, inside, i, error->line);
                free(nested_text);free(inside);return -1;
            }
            script_program_free(nested);free(nested_text);
        } else if (inside[i] == '`') {
            for (finish=i+1;inside[finish];++finish){if(inside[finish]=='\\'&&inside[finish+1]){++finish;continue;}if(inside[finish]=='`')break;}
            if(!inside[finish]){free(inside);return error_set(error,line,"unclosed backtick substitution in test");}
            char *nested_text=strndup(inside+i+1,finish-i-1);ScriptProgram*nested=NULL;
            if(!nested_text||script_parse_depth(nested_text,&nested,error,1)<0){
                if (error != NULL) error->line = line_for_nested_error(line, inside, i, error->line);
                free(nested_text);free(inside);return -1;
            }
            script_program_free(nested);free(nested_text);
        }
        if (finish > start) {
            for (size_t k = start; k <= finish; ++k)
                if (inside[k] != '\n') inside[k] = 'x';
            i = finish;
        }
    }
    TokenList tokens; ParseError parse_error;
    if (lexer_tokenize(inside, &tokens, &parse_error) < 0) { free(inside); return error_set(error, line, "invalid test expression"); }
    free(inside);
    int valid = tokens.count == 2 && (strcmp(tokens.items[0].text, "-f") == 0 || strcmp(tokens.items[0].text, "-d") == 0) && tokens.items[1].type == TOKEN_WORD;
    valid = valid || (tokens.count == 3 && tokens.items[0].type == TOKEN_WORD && tokens.items[2].type == TOKEN_WORD &&
                      (strcmp(tokens.items[1].text, "-eq") == 0 || strcmp(tokens.items[1].text, "-gt") == 0));
    lexer_tokens_free(&tokens);
    return valid ? 0 : error_set(error, line, "tests support only -eq, -gt, -f, and -d");
}

static int
validate_command(const char *text, size_t line, ScriptError *error, unsigned depth)
{
    char *masked = strdup(text);
    char quote = 0;
    int escaped = 0;
    if (masked == NULL) return error_set(error, line, "out of memory");
    /* Substitution bodies are validated recursively and hidden from the
     * ordinary command lexer so their internal pipes remain nested. */
    for (size_t i = 0; masked[i] != '\0'; ++i) {
        size_t start, end;
        if (escaped) { escaped = 0; continue; }
        if (masked[i] == '\\' && quote != '\'') { escaped = 1; continue; }
        if (quote == '\'') { if (masked[i] == '\'') quote = 0; continue; }
        if (masked[i] == '\'' || masked[i] == '"') { if (quote == 0) quote = masked[i]; else if (quote == masked[i]) quote = 0; continue; }
        if (quote == '\'') continue;
        if (masked[i] == '$' && masked[i + 1] == '(') {
            start = i + 2; unsigned nest = 1; char q = 0; int esc = 0;
            for (end = start; masked[end] != '\0'; ++end) {
                char c=masked[end]; if (esc) { esc=0; continue; } if (c=='\\'&&q!='\'') { esc=1; continue; }
                if (q=='\'') { if(c=='\'')q=0; continue; } if(c=='\''||c=='"'){ if(q==0)q=c;else if(q==c)q=0;continue; }
                if(c=='$'&&masked[end+1]=='('){++nest;++end;continue;} if(c==')'&&--nest==0)break;
            }
            if (masked[end] != ')') { free(masked); return error_set(error, line, "unclosed command substitution"); }
            char *inner = strndup(masked + start, end - start); ScriptProgram *nested = NULL;
            if (!inner || script_parse_depth(inner, &nested, error, depth + 1) < 0) {
                if (error != NULL) error->line = line_for_nested_error(line, masked, i, error->line);
                free(inner); free(masked); return -1;
            }
            script_program_free(nested); free(inner);
            for (size_t k=i; k<=end; ++k)
                if (masked[k] != '\n') masked[k] = 'x';
            i = end; continue;
        }
        if (masked[i] == '`') {
            start=i+1; for(end=start; masked[end]; ++end) {
                if (masked[end] == '\\' && masked[end+1]) { ++end; continue; }
                if (masked[end] == '`') break;
            }
            if (!masked[end]) { free(masked); return error_set(error, line, "unclosed backtick substitution"); }
            char *inner=strndup(masked+start,end-start); ScriptProgram *nested=NULL;
            if(inner!=NULL)inner=translate_nested_backticks(inner);
            if(!inner||script_parse_depth(inner,&nested,error,depth+1)<0){
                if (error != NULL) error->line = line_for_nested_error(line, masked, i, error->line);
                free(inner);free(masked);return -1;
            }
            script_program_free(nested);free(inner);
            for(size_t k=i;k<=end;++k)if(masked[k]!='\n')masked[k]='x';
            i=end;
        }
    }
    Pipeline pipeline; ParseError parse_error;
    int result = parser_parse(masked, &pipeline, &parse_error);
    free(masked);
    if (result < 0) return error_set(error, line, "invalid command syntax");
    if (pipeline.background) { parser_pipeline_free(&pipeline); return error_set(error, line, "background commands are not part of the script grammar"); }
    parser_pipeline_free(&pipeline); return 0;
}

static int
evaluate_test(Shell *shell, const char *expression, size_t line)
{
    char *expanded = NULL;
    int ignored;
    if (expand_text(shell, expression, &expanded, &ignored, 0, 0) < 0) return 2;
    ScriptError error;
    if (validate_test(expanded, line, &error) < 0) { free(expanded); return 2; }
    const char *p = expanded, *end = expanded + strlen(expanded);
    if (strncmp(p, "[[", 2) == 0) { p += 2; while (isspace((unsigned char)*p)) ++p; end -= 2; }
    else { ++p; while (isspace((unsigned char)*p)) ++p; --end; }
    char *inner = strndup(p, (size_t)(end-p));
    if (!inner) { free(expanded); return 2; }
    TokenList tokens; ParseError pe;
    if (lexer_tokenize(inner, &tokens, &pe) < 0) { free(inner); free(expanded); return 2; }
    free(inner); int result = 1;
    if (tokens.count == 2) {
        struct stat st;
        int rc = fstatat(shell->cwd_fd, tokens.items[1].text, &st, 0);
        result = strcmp(tokens.items[0].text, "-f") == 0 ? (rc == 0 && S_ISREG(st.st_mode)) : (rc == 0 && S_ISDIR(st.st_mode));
    } else {
        char *a_end, *b_end; errno=0; long a=strtol(tokens.items[0].text,&a_end,10); long b=strtol(tokens.items[2].text,&b_end,10);
        if(errno||*a_end||*b_end) result=0;
        else result=strcmp(tokens.items[1].text,"-eq")==0 ? a==b : a>b;
    }
    lexer_tokens_free(&tokens); free(expanded); return result ? 0 : 1;
}

static int
execute_list(Shell *shell, StatementList *list, int *status, int *exited, int *exit_status)
{
    for (size_t i=0; i<list->count && !*exited; ++i) {
        (void)pthread_mutex_lock(&shell->mutex);
        int cancelled = shell->script_cancelled || shell->stopping;
        (void)pthread_mutex_unlock(&shell->mutex);
        if (cancelled) { *status = 128 + 2; return 0; }
        Statement *s=list->items[i]; int current=0;
        if (s->type == ST_ASSIGN) {
            char *raw=NULL; int ignored;
            if (expand_text(shell,s->value.assignment.value,&raw,&ignored,1,0)<0) current=2;
            else {
                size_t need = strlen(raw) + 3;
                char *synthetic = malloc(need);
                if (synthetic != NULL) (void)snprintf(synthetic, need, "x %s", raw);
                Pipeline p; ParseError pe;
                if (raw[0] == '\0') {
                    if (variable_set(shell, s->value.assignment.name, "") < 0) current = 1;
                } else if (!synthetic || parser_parse(synthetic,&p,&pe)<0) current=2;
                else if (p.count!=1 || p.commands[0].argc!=2) { current=2; parser_pipeline_free(&p); }
                else { if(variable_set(shell,s->value.assignment.name,p.commands[0].argv[1])<0)current=1; parser_pipeline_free(&p); }
                free(synthetic);free(raw);
            }
        } else if (s->type == ST_EXIT) { *exited=1; *exit_status=s->value.exit_status; current=*exit_status; }
        else if (s->type == ST_COMMAND) {
            char *expanded=NULL; int ignored, should_exit=0;
            if(expand_text(shell,s->value.text,&expanded,&ignored,0,0)<0) current=2;
            else { current=execute_line(shell,expanded,&should_exit); free(expanded); if(should_exit){*exited=1;*exit_status=current;} }
        } else {
            int matched=0;
            for(size_t b=0;b<s->value.conditional.count;++b) {
                IfBranch *branch=&s->value.conditional.branches[b];
                int test=evaluate_test(shell,branch->test,branch->line);
                if(test==0){ matched=1; if(execute_list(shell,&branch->body,&current,exited,exit_status)<0)return -1; break; }
                if(test>1) current=test;
            }
            if(!matched && !*exited && execute_list(shell,&s->value.conditional.otherwise,&current,exited,exit_status)<0)return -1;
        }
        *status=current; shell->script_status=current;
    }
    return 0;
}

int
script_execute(Shell *shell, ScriptProgram *program, int argc, char **argv, int *exit_status)
{
    char **old_args=shell->script_args; size_t old_argc=shell->script_argc; int old_status=shell->script_status;
    int old_active, old_cancelled;
    int status=0, exited=0, exit_value=0;
    (void)pthread_mutex_lock(&shell->mutex);
    old_active = shell->script_active; old_cancelled = shell->script_cancelled;
    shell->script_active = 1;
    if (!old_active) shell->script_cancelled = 0;
    (void)pthread_mutex_unlock(&shell->mutex);
    shell->script_args=argv; shell->script_argc=(size_t)argc; shell->script_status=0;
    (void)execute_list(shell,&program->body,&status,&exited,&exit_value);
    if(exited) status=exit_value;
    shell->script_args=old_args; shell->script_argc=old_argc; shell->script_status=old_status;
    (void)pthread_mutex_lock(&shell->mutex);
    int was_cancelled = shell->script_cancelled;
    shell->script_active = old_active;
    shell->script_cancelled = old_active ? (old_cancelled || was_cancelled) : 0;
    (void)pthread_mutex_unlock(&shell->mutex);
    if(exit_status)*exit_status=status;
    return status;
}

int
script_invoke(Shell *shell, const char *path, int argc, char **argv,
              int source_mode, int *exit_status)
{
    int fd=openat(shell->cwd_fd,path,O_RDONLY|O_CLOEXEC);
    if(fd<0){char msg[512];int n=snprintf(msg,sizeof(msg),"terminator: %s: %s\n",path,strerror(errno));if(n>0)(void)shell_emit(shell,SHELL_EVENT_OUTPUT,msg,(size_t)n,0);return 1;}
    size_t cap=4096,len=0;char *text=malloc(cap);if(!text){close(fd);return 1;}
    for(;;){if(len+2048>cap){cap*=2;char *p=realloc(text,cap);if(!p){free(text);close(fd);return 1;}text=p;}ssize_t n=read(fd,text+len,cap-len-1);if(n>0){len+=(size_t)n;continue;}if(n==0)break;if(errno==EINTR)continue;int e=errno;char msg[512];int count=snprintf(msg,sizeof(msg),"terminator: %s: %s\n",path,strerror(e));if(count>0)(void)shell_emit(shell,SHELL_EVENT_OUTPUT,msg,(size_t)count,0);free(text);close(fd);errno=e;return 1;}
    close(fd);text[len]='\0';
    /* Keep the shebang in the source buffer: the script lexer treats it as a
     * comment, preserving physical line numbers while still using our parser. */
    ScriptProgram *program=NULL;ScriptError error;
    if(script_parse(text,&program,&error)<0){char msg[512];(void)snprintf(msg,sizeof(msg),"%s:%zu: syntax error: %s\n",path,error.line,error.message);(void)shell_emit(shell,SHELL_EVENT_OUTPUT,msg,strlen(msg),0);free(text);return 2;}
    free(text);
    int saved_cwd=-1;char *saved_path=NULL;ShellVariable *saved_vars=NULL;
    if(!source_mode){
        saved_cwd=fcntl(shell->cwd_fd,F_DUPFD_CLOEXEC,3);saved_path=strdup(shell->cwd_path);saved_vars=variables_clone(shell->variables);
        if(saved_cwd<0||saved_path==NULL||(shell->variables!=NULL&&saved_vars==NULL)){
            if(saved_cwd>=0) close(saved_cwd);
            free(saved_path);
            variables_free(saved_vars);
            script_program_free(program);
            return 1;
        }
    }
    int status=0;(void)script_execute(shell,program,argc,argv,&status);script_program_free(program);
    if(!source_mode){close(shell->cwd_fd);shell->cwd_fd=saved_cwd;free(shell->cwd_path);shell->cwd_path=saved_path;variables_free(shell->variables);shell->variables=saved_vars;}
    if(exit_status)*exit_status=status;
    return status;
}

