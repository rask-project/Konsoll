# Per-Session Threading Plan

> Status: proposed architecture; not implemented yet.
> Applies to Konsoll's Qt 6.12 / C++23 macOS implementation.

## 1. Purpose

This document records a plan for giving every terminal session its own I/O
worker thread. The goal is to keep PTY event handling independent between
sessions and prevent PTY reads and writes from blocking the GUI event loop.

This change alone does not move terminal parsing or rendering state off the
GUI thread. That is a separate possible optimization described in §9.

## 2. Current implementation

`TerminalManager` creates a `TerminalBackend` per session and starts it on the
GUI thread. `TerminalBackend::start()` creates a `PtyProcess` and calls
`forkpty()` synchronously. `PtyProcess` then creates a `QSocketNotifier` in
the same thread. Its read callback emits data to `TerminalBackend::onData()`,
which parses the bytes and updates the terminal screen model on the GUI
thread. Input, resize, and protocol replies also reach the PTY from that
thread.

The notifier uses a non-blocking master descriptor, but this does not move
work to another thread. In particular, reading and parsing a large amount of
output can still consume GUI event-loop time.

## 3. Proposed ownership model

Keep QML-facing objects and all state directly queried by QML on the GUI
thread. Add one worker object and one `QThread` per session:

```text
GUI thread                                      Session thread
QML                                             PtyWorker
  │                                                 │
TerminalManager                                    PtyProcess
  │                                                 │
TerminalBackend ── queued commands/signals ────────┤
  │                       output/errors/exited ◄────┤
Terminal + UI state                                 │
                                                    ▼
                                              PTY master / shell
```

### GUI-thread responsibilities

- Own `TerminalManager`, `TerminalBackend`, QML-facing properties, and the
  terminal model (`Terminal` / `Screen`).
- Handle keyboard and mouse input, selection, search, clipboard, scrollback,
  and snapshot queries.
- Apply PTY output to the terminal model and coalesce repaint notifications.
- Route all requests for PTY I/O to that session's worker.

### Session-thread responsibilities

- Own `PtyWorker` and `PtyProcess` for exactly one session.
- Create and operate the PTY master descriptor and `QSocketNotifier` in the
  session thread's event loop.
- Handle ordered start, write, resize, and shutdown commands.
- Emit output in byte blocks, plus startup failure and process-exit events.
- Never access QML objects, `TerminalBackend`, or the GUI-owned terminal
  model directly.

Create the `QSocketNotifier` only after the worker is running in its target
thread. Do not call `PtyProcess` methods directly across thread boundaries.
Use queued signals/slots (or an equivalent queued invocation mechanism) for
all cross-thread commands and results.

## 4. Thread-safe process launch is a prerequisite

The existing `PtyProcess::start()` calls `forkpty()` and performs work in
the child before `execvp()`, including `tcgetattr()`, `tcsetattr()`,
`chdir()`, `setenv()`, and `execvp()`. Once the application has multiple
threads, only async-signal-safe operations may safely run in the child after
`fork()` and before `exec()`. Other calls can deadlock on locks inherited
from threads that do not exist in the child.

Therefore, moving `forkpty()` into a `QThread` is not by itself a safe
solution. Before enabling per-session threads, choose and validate a launch
strategy that is safe when the parent process is multithreaded. Options to
investigate include:

1. A small helper process that owns PTY creation and shell launch.
2. A `posix_spawn`-based setup that correctly establishes the controlling
   terminal, session, process group, file descriptors, and required terminal
   settings on macOS.
3. Another platform-supported launch mechanism with equivalent guarantees.

The choice must preserve login-shell behavior, environment variables, shell
integration, working directory, window size, signal delivery, and child
reaping. Do not ship the threaded design until this launch path has been
validated on supported macOS versions.

## 5. Cross-thread protocol

Define explicit worker commands and results. Suggested operations are:

| Direction | Operation | Payload / effect |
|---|---|---|
| Backend → worker | Start | Shell path, dimensions, working directory; returns success or a concrete error |
| Backend → worker | Write | Byte block to send to the PTY master |
| Backend → worker | Resize | Column and row counts; apply `TIOCSWINSZ` |
| Backend → worker | Shutdown | Begin bounded child and descriptor cleanup |
| Worker → backend | Output | One byte block read from the PTY |
| Worker → backend | Start result | Success or launch error |
| Worker → backend | Process exited | Exit code / termination status |
| Worker → backend | Shutdown complete | Worker has released its PTY resources |

Use queued connections so each operation runs in the receiver's thread. Keep
commands for one session ordered. PTY output should be emitted in blocks,
not one signal per byte. Define a batching/backpressure policy so sustained
output cannot grow an unbounded queue of GUI work. The worker must continue
draining the PTY while output is being delivered to the backend.

## 6. Terminal protocol replies

`TerminalBackend::onData()` currently parses output and may send responses
back to the shell for terminal queries (keyboard protocol, device
attributes, color, and status). These replies must use the same queued write
path as user input. The backend must not access the PTY descriptor or call
`PtyProcess::write()` across threads.

Likewise, user input and mouse reports are encoded by the backend and sent
as byte blocks through the worker. Resizing updates the GUI-owned terminal
model on the GUI thread and queues the corresponding PTY resize command.

## 7. Lifecycle and ownership

Represent each session's lifecycle explicitly, for example:

```text
Created → Starting → Running → Stopping → Stopped
              └────→ Failed
```

The intended sequence is:

1. Create the backend, worker, and thread with clear ownership.
2. Move the worker to its thread and start that thread's event loop.
3. Request PTY startup through a queued command.
4. Mark the session running only after startup succeeds; propagate startup
   errors to the backend and UI/logging path.
5. On close, stop accepting new input, request worker shutdown, and let the
   worker close the notifier and descriptor and terminate/reap the child.
6. On shutdown completion, stop and join the thread, then destroy the worker
   and backend in an order that cannot leave queued callbacks pointing at
   deleted objects.

Shutdown must not wait indefinitely on the GUI thread. Define a bounded
graceful-shutdown period, process-group signaling policy (including
`SIGHUP`/`SIGKILL` escalation where needed), and final child-reaping path.
The exact policy should preserve the current behavior of closing the shell
and its foreground process group. Handle races among natural process exit,
explicit session close, app shutdown, and pending output signals so exit and
cleanup are each finalized only once.

`TerminalManager::closeSession()` must not immediately destroy objects still
referenced by queued signals. Coordinate removal with shutdown completion or
use an ownership pattern that safely invalidates late events.

## 8. Integration and migration steps

1. **Resolve launch safety.** Prototype the selected process-launch strategy
   in a small isolated path and validate PTY semantics on macOS.
2. **Define worker API and lifecycle.** Specify commands, result signals,
   errors, state transitions, output batching, and shutdown behavior.
3. **Move PTY ownership.** Make the worker the sole owner of `PtyProcess`,
   its descriptor, notifier, and child-process lifecycle.
4. **Route all I/O.** Replace direct backend calls for writes and resize with
   queued worker commands. Route terminal query replies through the same
   path.
5. **Connect output safely.** Deliver byte blocks to the backend on the GUI
   thread and keep parsing/model updates there for this first iteration.
6. **Integrate session management.** Update session creation, close, natural
   exit, workspace removal, and application shutdown to honor asynchronous
   worker cleanup.
7. **Remove stale assumptions.** Update architecture documentation that
   describes a dedicated reader thread or implies PTY callbacks already run
   outside the GUI thread.
8. **Validate multi-session behavior.** Exercise simultaneous output,
   independent input, resizing, process exit, rapid close/reopen, and app
   shutdown while shells are active.

## 9. Performance boundary and possible follow-up

In this first design, the session thread handles PTY I/O, while parsing and
screen-model updates remain on the GUI thread. This keeps QML-facing
behavior and current APIs straightforward, but output parsing may remain a
GUI bottleneck under heavy load.

If profiling shows that parsing is still a problem, consider moving the
parser and screen model into the session worker as a separate project. That
requires replacing direct `m_terminal` queries with a thread-safe interface,
such as immutable frame snapshots or versioned updates. Selection, search,
mouse reporting, clipboard behavior, resizing, and QML snapshot access all
currently depend on synchronous access to the model and would need explicit
redesign. Do not share mutable `Terminal` or `Screen` state between threads.

## 10. Acceptance criteria

- Each live session has its own worker thread and PTY resources are only
  accessed in that worker's thread.
- The GUI remains responsive while sessions produce output concurrently.
- Input, terminal-generated replies, and resize commands reach only the
  intended session and preserve command order.
- Session output, title/directory updates, exit status, selection, search,
  clipboard, and rendering remain correct.
- Closing one session leaves other sessions and their shells running.
- Natural exit, explicit close, and application shutdown release notifier,
  descriptor, thread, and child-process resources exactly once.
- Startup and shutdown failures are observable and do not leave orphaned
  child processes or join the GUI thread indefinitely.
- The process-launch strategy is verified to be safe with a multithreaded
  parent process on supported macOS versions.

