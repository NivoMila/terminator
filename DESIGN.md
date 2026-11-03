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
workers, command queues, event queues, lifecycle and cancellation. The thread
worker is a runtime adapter around `ShellState`, declared in
`src/shell_internal.h`. `shell_state_init()` creates reusable per-tab state
with an output callback; `shell_state_execute()` runs one interactive command
synchronously; `shell_state_reap_jobs()` services background jobs;
`shell_state_complete_path()` performs state-relative completion;
`shell_state_signal_foreground()` handles runtime signal requests; and
`shell_state_shutdown()` / `shell_state_destroy()` cancel jobs and release
state. The thread adapter preserves the public `Shell` API and routes emitted
events into its existing synchronized queue. The executor and script
interpreter receive `ShellState *` directly, so they do not depend on thread
command/event queues. The output callback is the runtime boundary and keeps
X11 outside shell execution. `lexer.c`
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
command separators in this grammar. Interactive input uses the same lexer and
parser to distinguish incomplete quoting/substitutions and trailing-backslash
continuations from invalid syntax. Each tab retains the accumulated command
until complete; Ctrl+C cancels an unfinished continuation. Completed commands
use the script interpreter's existing expansion path before ordinary parsing,
including nested `$(...)` and backtick substitutions. A trailing backslash
continues without inserting a newline; open quotes and substitutions preserve
the newline between physical input lines. Multiline history records are stored
as escaped records so the history file remains line-delimited.

## multiWatch

`multiWatch ["command", ...]` is recognized before the ordinary pipeline
parser. Its outer list accepts comma-separated double-quoted command strings;
inside each string, backslash escapes a double quote or backslash, while other
backslashes are preserved for the command parser. Every entry is parsed using
the existing command grammar, and an entry may contain a pipeline. An inner
background operator is rejected. All entries launch concurrently in separate
process groups. Their captured stdout and stderr are emitted in labeled blocks
containing the command text, a Unix timestamp, 52-hyphen divider lines, and output (or an
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

The reusable shell-state API is exercised directly by `shell_state_test`, in
addition to the thread-mode integration tests. Shell state still uses a
per-state pthread mutex for foreground process, cancellation, job, and cwd
coordination; it does not start a worker thread itself. The process backend
uses the same state API in a separate child process.

The per-tab thread worker output queue is bounded to about 1 MiB of payload. If the
GUI falls behind, the worker waits for queue space and stops draining the
child's capture pipe; pipe backpressure then slows the child. The GUI processes
a bounded number of events per turn and re-notifies itself when queue entries
remain, allowing X events to be handled between output batches.

`./terminator` selects this thread backend. `./terminator useProcess=True`
selects the process backend while leaving Xlib and the GUI event loop in the
parent. Each tab owns a child with two pipes. The child starts through a
fork-and-exec trampoline: before `execve()` it closes every inherited file
descriptor except its command-read and response-write ends. This also makes
later tab creation safe while other parent-side monitor threads exist. The
fresh child image initializes one `ShellState` and runs its request loop.
Parent-side monitor threads move framed IPC to and from the child and publish
responses into the same per-tab event queue used by the GUI; command submission
therefore remains asynchronous to the GUI. The shell facade dispatches to the
selected backend, while command execution remains the same synchronous
`shell_state_execute()` path in the child.

The process runtime supports command requests, output chunks, command status,
shell exit, graceful shutdown, structured errors, asynchronous filename
completion requests, child readiness, and job-state updates. Completion uses the same state-relative `openat()` logic
and the same common-prefix/numbered-selection edit logic as thread mode. A
request ID associates the candidate response with its tab's saved input/cursor
context; stale responses are discarded. The parent validates the tab ID and
translates responses into the existing shell events. It detects EOF/crash and
reaps each child independently. Child startup uses `/proc/self/exe` and
descriptor isolation currently targets Linux. Process-mode command children
use Linux `PR_SET_PDEATHSIG` as a final safeguard if their tab supervisor is
killed unexpectedly; orderly shutdown still terminates and reaps known process
groups.

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
   before the fork. This intentionally differs from the assignment's literal
   `execvp()` wording: PATH lookup and environment preparation happen before
   fork, avoiding post-fork library work in a multithreaded GUI process.
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

The GUI owns one `History` handle shared by tabs. Commands are stored in
`$HOME/.terminator_history`, with empty and whitespace-only lines excluded and
the newest 10,000 entries retained. Thread mode retains its process-local
history representation. Process mode uses the same API over a POSIX shared
memory segment so the GUI and each tab child see the same records. A local
pthread mutex protects each handle; a stable path-specific lock file serializes
thread-mode and process-mode application instances. Up/Down navigation uses an
independent cursor and draft per tab.

The GUI remains the sole append authority: after a command is accepted for
submission, the GUI calls `history_add()` once. A syntactically invalid line
continues to be recorded according to existing behavior. Process children
attach to the same history for reads and the `history` builtin; they do not
append submitted commands, preventing a second copy of each entry.

Filename completion is requested through the active tab's `Shell` interface.
The API holds the shell mutex while resolving the prefix from that tab's cwd FD
and enumerating the target directory. The GUI decodes the unfinished shell
word, requests matches, inserts a unique match or its common prefix with shell
escaping, and lists multiple matches with one-based numbers when the prefix
cannot be extended. Completion replaces text from the current word's start to
the cursor and preserves the rest of the line as typed. Pressing a listed
number replaces that unfinished prefix with its candidate; candidates whose
number is also a prefix of a longer number remain pending until another digit
or Enter disambiguates the choice. Escape
cancels selection, and invalid choices leave the command line intact. Selection
state belongs to each tab. Directory matches end in `/`, allowing the user to
continue entering a path. Candidate insertion uses the same shell escaping as
ordinary completion, including filenames with spaces and nested paths.

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

The thread and process backends both lock the stable `<history-path>.lock`
file before reading or changing the durable history. Updates use a staged file
in the same directory, `fsync()` the staged file, atomically rename it over the
history file, then `fsync()` the containing directory. This keeps the lock
identity stable across rename and prevents thread-mode applications from
overwriting process-mode updates.

## Process-mode history and crash handling

The process-shared segment name is derived from the user ID, a stable FNV-1a
hash of the history path, and the layout version. Version 1 has a fixed
mapping size: a header, 10,000 `{offset, length}` entries, a 16 MiB byte arena,
and one process-shared robust pthread mutex. The header carries magic, version,
mapping and arena sizes, entry count, bytes used, generation, and the durable
file identity (device, inode, size, and timestamps). Records are NUL-terminated
in the arena; offsets are relative to the arena and no process-local pointers
are stored. A single record is limited to 1 MiB. The mapping is never resized.
If the entry count, record limit, or total arena capacity would be exceeded,
`history_add()` fails with `ENOSPC` before changing the durable file or shared
state.

All participants use this lock order: per-handle pthread mutex, stable
path-specific `flock(LOCK_EX)`, then the robust process-shared mutex. There is
no process-shared semaphore. The stable file lock excludes participants while
bootstrapping, validating, or replacing an invalid shared segment, and avoids
waiting processes being stranded behind recovery. The same outer lock is used
by thread mode, so mixed application instances cannot lose updates. Reads
refresh the shared index when the durable file identity changes. Writes first
refresh from the durable file, prepare the updated records, atomically persist
them, then publish the new offset index and file identity in shared memory.

The history file is authoritative. On `EOWNERDEAD`, the next process parses
the durable file, validates capacity, rebuilds the shared entries, and calls
`pthread_mutex_consistent()` only after rebuilding succeeds. If recovery fails,
the mutex is left inconsistent and the operation returns an error; a later
participant can replace an `ENOTRECOVERABLE` segment under the stable file
lock. Invalid headers or incompatible layouts are likewise replaced only
after successfully loading the durable file. Shared memory is never used to
rewrite the history file during recovery.

If a process dies before rename, the old history file remains intact and an
orphaned staged file is removed on a later open. Death after rename but before
shared-memory publication leaves the complete new file authoritative; owner
death recovery rebuilds the index from it. Death during shared index updates
also rebuilds from the file. `history_close()` detaches and closes local
resources but does not destroy shared mutexes or unlink the segment because
other application instances may still be attached.

Lock and segment identities currently use the history path spelling. Relative
and absolute spellings, or symlink aliases, can therefore create separate
locks and shared segments for one underlying file. This is not expected for the
normal `$HOME/.terminator_history` path when all instances inherit the same
absolute `HOME`, but aliases are not safe. A future identity fix should resolve
an existing final symlink to its target, or canonicalize the parent directory
and append the basename when the file does not yet exist, then use that one
resolved path for both I/O and lock/segment derivation. Resolving only the
parent and renaming over a final symlink would replace the symlink itself;
canonicalization must preserve the intended target semantics.

## Event loop and process IPC

`src/ipc.c` and `src/ipc.h` implement the version-1 framing layer independently
of X11 and shell execution. Each frame has a 24-byte, network-byte-order header:
32-bit magic (`TERM`), 16-bit protocol version, 16-bit message type, 32-bit tab
ID, 64-bit request/sequence ID, and 32-bit payload length. Payloads are limited
to 1 MiB. The current message types are command submission, completion request,
graceful shutdown, output chunk, command status, shell exit, completion response,
job-state update, structured error, and child readiness.

Command submissions, completion requests, and output chunks carry raw bytes.
Shutdown has no payload. Command status and shell exit carry a signed 32-bit
status. Completion responses carry a 32-bit candidate count followed by
32-bit-length-prefixed candidate byte strings. Job updates have a 16-byte
payload: 32-bit job ID, signed 64-bit process-group ID, 8-bit state (running,
stopped, exited, or complete), 8-bit foreground flag, and two zero reserved
bytes. Structured errors carry a 32-bit error code followed by message bytes.
Readiness has no payload and request ID zero.
All payload integers use network byte order. Header and type-specific payload
sizes, job-state fields, completion-list bounds, magic, version, and known
message types are validated.

`IpcReader` retains partial header/payload state between calls and distinguishes
an incomplete nonblocking read, clean EOF, and a truncated frame. `IpcWriter`
owns a copied, encoded frame until incremental flushing completes; callers can
queue one frame at a time. Reader-returned payloads belong to the caller and
must be released with `ipc_message_dispose()`. Both helpers retry `EINTR` and
preserve progress across `EAGAIN`/`EWOULDBLOCK`. The process runtime currently
uses command, completion-request/response, shutdown, output, command-status,
shell-exit, structured-error, readiness, and job-state-update messages.

After initializing its `ShellState`, a process child sends a zero-payload
readiness message. The parent queues interrupt, suspend, and shutdown controls
until readiness arrives, avoiding signals during the fork/exec descriptor
cleanup window. The parent then signals only that tab's direct child with
`SIGUSR1` (interrupt), `SIGUSR2` (suspend), or `SIGTERM` (shutdown). The child
handlers write one-byte codes to a nonblocking self-pipe; they never call shell
code or signal a process group. The child's request loop and foreground/job
wait loops poll that pipe and invoke the existing `ShellState` control logic in
normal execution context. The child therefore resolves the active, owned job
group and applies Ctrl+C/Ctrl+Z. Shutdown marks the state stopping and kills
its known process groups before the child exits. The parent escalates a child
that does not exit and the monitor thread reaps it independently.

Job-state updates use the existing 16-byte payload and request ID zero. The
child reports per-job running, stopped, and exited transitions, plus
foreground/background changes from `fg` and `bg`. The parent validates and
publishes these as `SHELL_EVENT_JOB_STATE` events; the child's job table
remains authoritative for built-in behavior.

The GUI event loop currently continues to poll X and the existing wake pipe;
per-tab parent monitor threads consume child IPC and place resulting events on
the same event queues as thread mode. A later event-loop integration can poll
the IPC descriptors directly. Both stdout and stderr are routed to the
originating tab. Output is plain text; there is no PTY or terminal
escape-sequence emulator.

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
accept `[ ... ]` or `[[ ... ]]`, with string `=`, numeric `-eq`/`-gt`, and
file/directory `-f`/`-d`. Variables,
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
