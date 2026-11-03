# Terminator

Terminator is a Linux/X11 tabbed command runner implemented in C. Each tab has
independent shell state and runs commands either in a worker thread or in an
isolated child process. Output is captured and shown in the tab; this is not a
PTY terminal emulator and does not interpret terminal escape sequences.

## Build and run

Requirements: a C11 compiler, pthreads, Xlib development headers, and an X11
display.

```sh
make
./terminator
./terminator useProcess=True
```

The default selects thread mode. `useProcess=True` selects one child process
per tab while the GUI remains in the parent. The implementation uses Linux
facilities including `/proc/self/exe`, `openat()`/directory FDs,
`PR_SET_PDEATHSIG`, POSIX shared memory, and robust process-shared pthread
mutexes. On Debian or Ubuntu, install a compiler and Xlib headers with
`build-essential` and `libx11-dev`.

Run the automated suite with:

```sh
make test
```

## Implemented behavior

- Multiple tabs with independent cwd, variables, jobs, input, and history
  navigation state.
- Quoting, escaping, multiline input, pipelines, input/output redirection,
  background jobs, and `jobs`, `fg`, and `bg`.
- `cd`, `pwd`, `exit`, `history`, `multiWatch`, and the internal script
  interpreter (`source`, `bash`, or an executable script path).
- Script positional arguments, `$?`, `$()`, backtick substitutions, and
  `if`/`elif`/`else` tests for string equality, numeric comparisons, regular
  files, and directories.
- Filename completion, editable line shortcuts, Up/Down history, Ctrl+R
  search, and X11 clipboard copy/paste.
- Thread mode and process mode share the shell-state execution code. Process
  mode communicates with the parent over a bounded, framed pipe protocol.
- History retains the newest 10,000 entries in a durable file. Process mode
  shares a fixed-size POSIX shared-memory index protected by a robust
  process-shared mutex and a stable path-specific `flock()` file. Updates stage
  and `fsync()` a file in the same directory, rename atomically, and sync the
  containing directory.

`DESIGN.md` describes the architecture, IPC wire format, history recovery,
synchronization order, and implementation limitations. Shortcuts may be
configured in `~/.terminatorrc`; invalid or conflicting entries are ignored
with a diagnostic and the default binding remains in effect.

## Limitations

- This is a command runner, not a terminal emulator: no PTY, terminal control
  sequences, or interactive full-screen programs.
- The process backend and several recovery mechanisms depend on Linux.
- Process startup currently uses `/proc/self/exe`; non-Linux systems are not
  supported by the current implementation.
- External commands are launched with `execve()` after PATH resolution before
  `fork()`, instead of the assignment's literal `execvp()` call.
- Script syntax is intentionally limited to the documented internal grammar;
  it does not invoke Bash and does not implement loops or functions.
- GUI interaction requires a working X11 display. Clipboard behavior depends
  on an X11 selection owner.
