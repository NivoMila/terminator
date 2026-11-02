CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LDLIBS ?= -lX11 -pthread
THREAD_FLAGS ?= -pthread

TARGET := terminator
SOURCES := src/main.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/history.c src/key_config.c
OBJECTS := $(SOURCES:.c=.o)
PARSER_TEST := tests/parser_test
SHELL_TEST := tests/shell_execution_test
JOB_TEST := tests/job_control_test
HISTORY_CONFIG_TEST := tests/history_config_test
MULTIWATCH_TEST := tests/multiwatch_test
SCRIPT_TEST := tests/script_test

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -c -o $@ $<

$(PARSER_TEST): tests/parser_test.c src/lexer.c src/parser.c src/parser.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/parser_test.c src/lexer.c src/parser.c

$(SHELL_TEST): tests/shell_execution_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/shell_execution_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c

$(JOB_TEST): tests/job_control_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/job_control_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c

$(HISTORY_CONFIG_TEST): tests/history_config_test.c src/history.c src/key_config.c src/history.h src/key_config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/history_config_test.c src/history.c src/key_config.c -lX11 -pthread

$(MULTIWATCH_TEST): tests/multiwatch_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/multiwatch_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c

$(SCRIPT_TEST): tests/script_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc -o $@ tests/script_test.c src/shell.c src/execute.c src/lexer.c src/parser.c src/script.c

test: $(PARSER_TEST) $(SHELL_TEST) $(JOB_TEST) $(HISTORY_CONFIG_TEST) $(MULTIWATCH_TEST) $(SCRIPT_TEST)
	./$(PARSER_TEST)
	./$(SHELL_TEST)
	./$(JOB_TEST)
	./$(HISTORY_CONFIG_TEST)
	./$(MULTIWATCH_TEST)
	./$(SCRIPT_TEST)

clean:
	rm -f $(OBJECTS) $(TARGET) $(PARSER_TEST) $(SHELL_TEST) $(JOB_TEST) $(HISTORY_CONFIG_TEST) $(MULTIWATCH_TEST) $(SCRIPT_TEST)
