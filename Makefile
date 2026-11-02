CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LDLIBS ?= -lX11 -pthread
THREAD_FLAGS ?= -pthread

TARGET := terminator
SOURCES := src/main.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/history.c src/key_config.c src/editor.c src/xclipboard.c
SOURCES += src/interactive_input.c
OBJECTS := $(SOURCES:.c=.o)
PARSER_TEST := tests/parser_test
SHELL_TEST := tests/shell_execution_test
JOB_TEST := tests/job_control_test
HISTORY_CONFIG_TEST := tests/history_config_test
MULTIWATCH_TEST := tests/multiwatch_test
SCRIPT_TEST := tests/script_test
EDITOR_TEST := tests/editor_test
CLIPBOARD_TEST := tests/clipboard_test
INTERACTIVE_INPUT_TEST := tests/interactive_input_test

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -c -o $@ $<

$(PARSER_TEST): tests/parser_test.c src/lexer.c src/parser.c src/parser.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/parser_test.c src/lexer.c src/parser.c

$(SHELL_TEST): tests/shell_execution_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/history.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/shell_execution_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/history.c

$(JOB_TEST): tests/job_control_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/history.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/job_control_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/history.c

$(HISTORY_CONFIG_TEST): tests/history_config_test.c src/history.c src/key_config.c src/history.h src/key_config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/history_config_test.c src/history.c src/key_config.c -lX11 -pthread

$(EDITOR_TEST): tests/editor_test.c src/editor.c src/editor.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/editor_test.c src/editor.c

$(CLIPBOARD_TEST): tests/clipboard_test.c src/xclipboard.c src/xclipboard.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/clipboard_test.c src/xclipboard.c -lX11

$(INTERACTIVE_INPUT_TEST): tests/interactive_input_test.c src/interactive_input.c src/lexer.c src/parser.c src/interactive_input.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/interactive_input_test.c src/interactive_input.c src/lexer.c src/parser.c

$(MULTIWATCH_TEST): tests/multiwatch_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/history.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/multiwatch_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/history.c

$(SCRIPT_TEST): tests/script_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/history.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/script_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/history.c

test: $(PARSER_TEST) $(SHELL_TEST) $(JOB_TEST) $(HISTORY_CONFIG_TEST) $(MULTIWATCH_TEST) $(SCRIPT_TEST) $(EDITOR_TEST) $(CLIPBOARD_TEST) $(INTERACTIVE_INPUT_TEST)
	./$(PARSER_TEST)
	./$(SHELL_TEST)
	./$(JOB_TEST)
	./$(HISTORY_CONFIG_TEST)
	./$(MULTIWATCH_TEST)
	./$(SCRIPT_TEST)
	./$(EDITOR_TEST)
	./$(CLIPBOARD_TEST)
	./$(INTERACTIVE_INPUT_TEST)

clean:
	rm -f $(OBJECTS) $(TARGET) $(PARSER_TEST) $(SHELL_TEST) $(JOB_TEST) $(HISTORY_CONFIG_TEST) $(MULTIWATCH_TEST) $(SCRIPT_TEST) $(EDITOR_TEST) $(CLIPBOARD_TEST) $(INTERACTIVE_INPUT_TEST)
