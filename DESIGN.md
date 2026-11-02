# Terminator design notes

## Thread-mode shell architecture

The GUI remains the sole Xlib owner. It holds stable, separately allocated tab
objects containing the editable input, scrollback, and an opaque `Shell *`.
Each tab has one worker thread. `shell_submit()` queues a copied command under
a mutex and wakes that worker with a condition variable. Workers publish
output, command-completion, and shell-exit events to a per-shell synchronized
queue, then write a byte to a shared nonblocking wake pipe. The GUI polls that
pipe together with the X connection, drains worker events, and is the only code
that modifies visible tab buffers. The wake pipe is only a notification; the
mutex-protected queues carry event data, so short writes and message framing
are not an issue. A full wake pipe already represents a pending notification.
The GUI marks both wake-pipe descriptors and the X connection close-on-exec;
directory, capture, and exec-status descriptors also use close-on-exec.

`src/shell.h` is the GUI-facing opaque shell interface. `shell.c` owns tab
workers, command queues, event queues, lifecycle and cancellation. `lexer.c`
and `parser.c` are independent of execution. The lexer returns words and
operator tokens; the parser produces a `Pipeline` containing ordered commands,
argv arrays, and an ordered list of typed redirections for each command.
`execute.c` runs plain single commands plus `cd`, `pwd`, `exit`, `jobs`, `fg`,
and `bg`, and runs external-command pipelines with ordered input/output
redirections and a trailing background `&`. Pipeline
stages share one process group and execute concurrently. Final-stage stdout and
stderr from every stage share a capture pipe routed to the originating tab.
The state-changing builtins (`cd` and `exit`) remain standalone shell commands
without redirections; pipeline stages and redirected commands are external
commands.
Each stage gets a close-on-exec setup-status pipe so the worker can report
child-side `fchdir()` and `execve()` failures as text without mixing binary
status data into command output. The worker drains capture output while the
children run, then waits for every stage; the pipeline status is the final
stage's status.

Each tab worker owns a job list with monotonically assigned IDs, one process
group per pipeline, per-child running/stopped/exited states, and a capture FD.
The worker uses `waitpid(WNOHANG | WUNTRACED | WCONTINUED)` and bounded capture
drains to reap background jobs without blocking the GUI or starving queued
commands. A SIGCHLD handler only records a signal-safe notification; periodic
worker wakeups provide a fallback, so child reaping does not depend on delivery
to a particular thread. `jobs` displays retained jobs; `fg` and `bg` accept an
optional numeric ID or `%ID`, defaulting to the newest non-completed job. The
GUI forwards Ctrl+C and Ctrl+Z to only the active tab's foreground process
group. With no foreground group, Ctrl+C has no effect. Closing a tab terminates
and reaps every process group in its table.

The parser grammar is one optional pipeline per submitted line:

```text
pipeline    := command ("|" command)* ["&"]
command     := (word | input-redirection | output-redirection)+
input-redirection  := "<" word
output-redirection := ">" word | ">>" word
```

Each command must have at least one argv word. At most one input and one
output redirection are accepted per command; their AST order matches source
order. Duplicate redirections, missing targets, unclosed quotes, dangling
escapes, and empty pipeline stages are syntax errors. Single quotes preserve their contents; double quotes remove
the delimiters and allow the supported escapes; adjacent quoted and unquoted
word fragments concatenate. Semicolon and other unlisted operators are not
command separators in this grammar. Variable expansion, command substitution,
multiline continuation, and script parsing remain for later stages.

## multiWatch

`multiWatch ["command", ...]` is recognized before the ordinary pipeline
parser. Its outer list accepts comma-separated double-quoted command strings;
inside each string, backslash escapes a double quote or backslash, while other
backslashes are preserved for the command parser. Every entry is parsed using
the existing command grammar, and an entry may contain a pipeline. An inner
background operator is rejected. All entries launch concurrently in separate
process groups. Their captured stdout and stderr are emitted in labeled blocks
containing the command text, a Unix timestamp, divider lines, and output (or an
empty-output/exit-status note). Each entry is restarted two seconds after that
run completes.

The tab worker owns the monitoring loop and uses `poll()` on watcher capture
pipes with a bounded timeout. It continues to reap ordinary background jobs
while monitoring. Ctrl+C marks only that tab's monitor stopped and signals its
watcher process groups; the worker kills/reaps remaining children, closes the
capture and status descriptors, removes watcher jobs, then returns to the
prompt. Closing a tab follows the existing worker shutdown cleanup, which
terminates all process groups and waits for direct children. MultiWatch state
and jobs are per `Shell`, so tabs monitor independently.

The per-tab worker output queue is bounded to about 1 MiB of payload. If the
GUI falls behind, the worker waits for queue space and stops draining the
child's capture pipe; pipe backpressure then slows the child. The GUI processes
a bounded number of events per turn and re-notifies itself when queue entries
remain, allowing X events to be handled between output batches.

## Per-tab filesystem and shell state

Linux current working directory and `environ` are process-wide. A tab must
therefore never call `chdir()` or mutate the process environment in a thread
worker. Each `ShellState` owns a logical cwd (preferably an open directory FD
plus its display path) and a private variable/environment map.

All path operations use that tab's cwd: `cd` resolves its target from the
current logical cwd and updates only that tab; redirection opens relative paths
with `openat(cwd_fd, path, ...)`; filename completion enumerates the cwd FD;
and script lookup/opening resolves relative paths against the same cwd. Child
processes inherit the tab's logical cwd by changing directory in the child
before exec. Parent and sibling-tab cwd state is never changed.

## Thread-mode child setup

Before `fork()`, the worker tokenizes the command line, allocates argv,
resolves the executable against the tab cwd and `PATH`, snapshots the
environment pointer, opens ordered redirections with `openat(cwd_fd, ...)`,
and creates pipeline, capture, and per-stage exec-status pipes above the
standard descriptor numbers. Redirections are opened in their source order;
relative paths therefore resolve from the tab's logical cwd. A failed open
prevents all children from starting, while earlier redirection side effects
(such as a file created before a later open fails) remain. The child must not
allocate memory, use stdio, acquire application locks, or call general shell
code. The current child setup sequence is limited to:

1. Establish/join the pipeline process group with `setpgid()`.
2. Close unused status-pipe ends and install pipeline stdin/stdout, redirection,
   and captured stderr descriptors using `dup2()` (final stdout is captured
   unless redirected).
3. Change directory using the pre-opened cwd directory FD (`fchdir()`).
4. Close original pipeline, capture, cwd and redirection descriptors not needed
   after setup; close-on-exec also protects the GUI wake pipe and other tabs'
   directory descriptors.
5. Call `execve()` with the pre-resolved path, argv, and environment prepared
   before the fork. `execvp()` performs PATH search after fork and is not
   guaranteed async-signal-safe, so PATH resolution is done by the worker.
6. If setup or exec fails, write the saved errno to that stage's close-on-exec
   status pipe using `write()` and call `_exit()`. The worker formats the
   diagnostic after `waitpid()` in the parent thread.

The implementation must preserve the parent’s cwd and environment. Linux cwd
operations use the tab’s directory FD (`openat()` for `cd`); the child alone
uses `fchdir()` before exec. `pwd` reports the path associated with that tab’s
directory FD. A separate worker owns each tab’s cwd state, so tabs do not share
or mutate it.

The child must not return through the multithreaded application's normal
cleanup path. Parent and child both call `setpgid()` where needed to handle
scheduling races. A pipeline is one job and one process group. On partial fork
failure or output-drain failure, the worker terminates the started group,
closes all descriptors, and waits for every started child before reporting
completion. Closing a tab kills the process groups retained in its job table
through the worker shutdown path.

## Stage 6 history, completion, and shortcuts

The GUI owns one `History` object shared by all tabs. Commands are stored in
`$HOME/.terminator_history`, with empty and whitespace-only lines excluded and
the newest 10,000 entries retained. A pthread mutex protects each in-process
history object; an advisory `flock()` on the history file serializes file
reloads and updates across processes. Future process-mode workers must use the
robust process-shared mutex and semaphore recovery protocol described below
for shared in-memory history, while retaining the advisory file lock around
cross-application file access. Up/Down navigation uses an independent cursor
and draft per tab.

Filename completion is requested through the active tab's `Shell` interface.
The API holds the shell mutex while resolving the prefix from that tab's cwd FD
and enumerating the target directory. The GUI decodes the unfinished shell
word, requests matches, inserts a unique match or its common prefix with shell
escaping, and lists multiple matches when the prefix cannot be extended.
Directory matches end in `/`, allowing the user to continue entering a path.

Shortcuts load once from `$HOME/.terminatorrc`; missing or invalid files leave
defaults active. The format is one `action = Modifier+Key` entry per line,
with `#` comments. Modifiers are `Ctrl`, `Shift`, and `Alt`. Supported actions
are `new_tab`, `close_tab`, `next_tab`, `previous_tab`, `interrupt`, `suspend`,
`complete`, `history_previous`, and `history_next`. Unknown actions, malformed
chords, and conflicts with another current binding are ignored without
disabling the default binding. Example:

```text
new_tab = Ctrl+T
close_tab = Alt+W
next_tab = Ctrl+Tab
previous_tab = Ctrl+Shift+Tab
interrupt = Ctrl+C
suspend = Ctrl+Z
complete = Tab
history_previous = Up
history_next = Down
```

The required line-editing chords (Ctrl+A/E/U/K/W/L, Ctrl+D, Ctrl+R, and
Ctrl+Shift+C/V) are reserved and cannot be rebound by `.terminatorrc`.
Ctrl+W deletes the previous whitespace-delimited word; close-tab defaults to
Alt+W. Ctrl+Shift+C owns the X11 `CLIPBOARD` selection with `TARGETS`,
`UTF8_STRING`, and `STRING` support and copies the editable command line.
Ctrl+Shift+V requests that selection and inserts the returned text at the
active tab's cursor. Ctrl+D closes the active tab, including the last tab.

This configuration and locking design is for thread mode. Process mode remains
unimplemented.

## Process-mode history and crash handling

History is persistent across restarts and shared by tabs. The durable history
file is loaded into the shared representation at startup; bounded updates
(10,000 entries) update the shared representation and backing file under one
critical section. Use the process-shared semaphore required by the spec, and
place a process-shared robust mutex around acquisition and recovery. All
participants take the robust mutex before the semaphore, so no second history
writer can be waiting on the semaphore during recovery.

If a tab dies while holding the robust mutex, the next participant receives
`EOWNERDEAD`, checks the shared history structure, and calls
`sem_getvalue()`. Since all access follows the same lock order, a zero value
means the dead owner consumed the semaphore without posting it; the recovery
path posts once, repairs/validates the ring metadata, then marks the robust
mutex consistent before releasing it. Normal paths post the semaphore before
unlocking the robust mutex. Critical sections must remain short and avoid
blocking I/O where possible; file persistence should use a staged write or
append protocol whose recovery can detect a partial record. This makes tab
process death recoverable instead of leaving all tabs blocked indefinitely.

## Event loop and process IPC

The GUI event loop polls the X connection FD, tab IPC/output FDs, child output
FDs, and timer deadlines. Process-mode messages are framed with a fixed header
containing message type, tab ID, and payload length. Readers and writers handle
partial transfers and EOF; tab process failure closes only that tab's channel
and is reaped by the GUI process.

Both stdout and stderr are routed to the originating tab. Output is plain text;
the first implementation has no PTY or terminal escape-sequence emulator.

## Internal script interpreter

`script.c`/`script.h` load script text through `openat()` on the tab's logical
cwd, split statements at unquoted newlines and semicolons, and build an owned
AST. The complete AST is validated before execution. Command statements are
validated and later executed through the existing lexer, pipeline parser, and
`execute_line()` path. Substitution bodies are parsed recursively; substitution
execution captures stdout while stderr remains visible in the originating tab.
Neither script invocation nor command substitution launches an external shell
as an interpreter.

The implemented statement forms are assignments, command pipelines with the
existing redirections, `if`/`elif`/`else`/`fi`, and `exit [n]`. Conditions
accept `[ ... ]` or `[[ ... ]]`, with `-eq`, `-gt`, `-f`, and `-d`. Variables,
`$?`, `$1`...`$@`, `$(...)`, and backticks expand in command words; single
quotes remain literal and double quotes allow expansion. Command substitution
supports nested and multiline bodies. Loops, functions, other test operators,
and other shell grammar extensions are outside this subset.

The per-tab variable map is owned by `Shell`. `source file` executes against
the current cwd and variable map. `./file.sh` and `bash file.sh` snapshot and
restore cwd and variables around the invocation, while still routing command
output and status through the tab worker. A shebang line is skipped as metadata;
its named interpreter is never invoked. `exit [n]` ends the current script
invocation only. Ordinary nonzero command statuses update `$?` and execution
continues. Ctrl+C marks the active script cancelled and signals its current
foreground process group; tab shutdown also cancels the worker and cleans up
its child jobs.
