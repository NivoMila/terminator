#ifndef TERMINATOR_PROCESS_RUNTIME_H
#define TERMINATOR_PROCESS_RUNTIME_H

#include "shell.h"

#include <stddef.h>
#include <stdint.h>

typedef struct ProcessRuntime ProcessRuntime;
typedef int (*ProcessRuntimeEvent)(void *context, ShellEventType type,
                                  const char *text, size_t length, int status,
                                  uint64_t request_id, char **matches,
                                  size_t match_count);

int process_runtime_parse_args(int argc, char **argv, int *use_process);
ProcessRuntime *process_runtime_create(uint32_t tab_id, int x11_fd,
                                       ProcessRuntimeEvent event,
                                       void *event_context);
int process_runtime_submit(ProcessRuntime *runtime, const char *command);
int process_runtime_signal(ProcessRuntime *runtime, int suspend);
int process_runtime_complete(ProcessRuntime *runtime, const char *prefix,
                             uint64_t *request_id);
int process_runtime_child_pid(const ProcessRuntime *runtime);
void process_runtime_shutdown(ProcessRuntime *runtime);
void process_runtime_destroy(ProcessRuntime *runtime);

/* Called only by the freshly exec'd helper image; returns its exit status. */
int process_runtime_child_entry(int argc, char **argv);
int process_runtime_is_child_invocation(int argc, char **argv);

#endif
