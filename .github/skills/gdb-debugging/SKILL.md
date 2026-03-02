---
name: gdb-debugging
description: >-
  Debug C++ applications using the GDB MCP server tools. Use when investigating
  runtime bugs, tracing code paths, inspecting variables in a running process,
  or diagnosing issues that cannot be found through static code analysis alone.
  Covers session management, breakpoints, backtraces, variable inspection,
  multi-threaded debugging, and strategies for async/coroutine pipelines.
metadata:
  author: gladius
  version: "1.0"
  requires: gdb-mcp-server
---

# Debugging C++ with GDB MCP

Use the `mcp_gdb-mcp_gdb_*` tools to start, control, and inspect a GDB session
attached to a C++ binary. This is for runtime debugging — when static analysis,
logs, and code reading aren't enough.

## When to Use GDB

- A code path is suspected but you can't confirm it statically
- A value at runtime differs from what you expect
- An async pipeline stalls and you need to see where threads are blocked
- A flag or variable has an unexpected value at a specific point
- You need to confirm whether a function is called (or how often)

## Session Lifecycle

### Start a session

```
mcp_gdb-mcp_gdb_start()
```

Returns a `session_id` used for all subsequent commands.

### Load the binary

```
mcp_gdb-mcp_gdb_command(session_id, "file /path/to/binary")
```

For Gladius:
```
file /home/jan/projects/gladius/gladius/out/build/linux-releaseWithDebug/src/gladius
```

### Run the program

```
mcp_gdb-mcp_gdb_command(session_id, "run")
```

### Terminate when done

```
mcp_gdb-mcp_gdb_terminate(session_id)
```

## Breakpoints

### Set breakpoints by file and line

```
break RenderWindow.cpp:1638
break ComputeCore.cpp:1255
```

### Set breakpoints by function name

```
break gladius::ui::RenderWindow::notifyAsyncEpochIncrement
```

### Manage breakpoints

```
info breakpoints          # list all
disable 1 2 3             # disable by number
enable 1 2 3              # re-enable
delete 3                  # remove permanently
```

### Conditional breakpoints

```
break RenderWindow.cpp:1638 if !state.isMoving
```

Useful for filtering high-frequency code paths to only break on interesting
states.

## Execution Control

```
continue                  # resume execution
next                      # step over (same thread)
step                      # step into
finish                    # run until current function returns
interrupt                 # pause a running program
```

## Inspecting State

### Backtrace

```
bt 10                     # show top 10 frames
bt                        # full backtrace
```

This is the most important command when a breakpoint hits — it tells you
**who called this code and why**.

### Print variables

```
print variableName
print this->m_someField
print result.epoch
print state.isMoving
```

### Atomic variables

GDB cannot call `std::atomic::load()` in optimized builds. Use the internal
member directly:

```
print m_asyncCurrentEpoch                 # prints "std::atomic<unsigned long> = { 9516 }"
print this->m_asyncCurrentEpoch._M_i      # prints the raw value
```

Do NOT try `print m_foo.load()` — it will error with "Cannot evaluate function
-- may be inlined".

### Expressions

```
print result.epoch < m_asyncCurrentEpoch._M_i
print (int)(state.renderingStepSize)
```

## Multi-Threaded Debugging

### List all threads

```
info threads
```

Shows thread ID, name, and current frame. Key things to look for:
- **Thread names** like `AsyncRenderWork`, `rusticl queue t` help identify roles
- **Where threads are blocked** — `futex_wait` means waiting for work,
  `syscall_cancel` means in a sleep or I/O

### Switch to a specific thread

```
thread 101
bt 20
```

Always switch to the thread of interest before running `bt` or `print`.

### Identify idle vs. active worker threads

If worker threads (e.g., `AsyncRenderWorker`) show:
```
coro::thread_pool::executor → futex_wait → syscall_cancel
```
They are **idle** — no work is enqueued. The bug is in the scheduling logic,
not the execution logic.

## Debugging Strategy

### 1. Form a hypothesis about where the pipeline breaks

Before starting GDB, identify the stages of the pipeline you're investigating.
For example, for async rendering:

1. Job scheduling (main thread)
2. Job execution (worker thread)
3. Result consumption (main thread)
4. Next-stage gating (main thread)

### 2. Set breakpoints at stage boundaries

Place breakpoints at the entry/exit of each stage. **Disable them initially**
if you need to interact with the application first.

```
break RenderWindow.cpp:1460    # SDF job scheduling
break RenderWindow.cpp:2619    # SDF execution start
break RenderWindow.cpp:1770    # SDF result consumption
break RenderWindow.cpp:1638    # low-res gate
disable 1 2 3 4
```

### 3. Reproduce the bug state, then enable breakpoints

```
enable 1 2 3 4
continue
```

### 4. Check which breakpoints fire (and which don't)

If no breakpoints fire, the code never reaches those paths. This is itself
a critical finding — look upstream.

### 5. When a breakpoint hits, check the backtrace first

The caller chain often reveals the root cause immediately. For example,
discovering that `notifyAsyncEpochIncrement()` is called from the UI thread
every frame immediately explains why async results are always "outdated."

### 6. Inspect gating variables

At conditional breakpoints or gates, print the relevant variables:

```
print sdfDirty
print sdfJobActive
print m_asyncCurrentEpoch
print result.epoch
```

Compare expected vs. actual values. Mismatches reveal the bug.

## Common Patterns and Pitfalls

### Epoch/version counter races

When debugging epoch-based invalidation systems:
- Print the epoch at job creation AND at result consumption
- If `result.epoch < currentEpoch`, something bumped the epoch during execution
- Set a breakpoint on the epoch-bump function and check `bt` to find who

### Sticky flags

Flags that are set but never cleared cause continuous re-triggering:
- Check where the flag is set (`grep` for `= true`)
- Check where it's cleared (`grep` for `= false`)
- If cleared only in `reset()` or similar, it's likely a per-frame flag that
  should be cleared after each read

### GDB MCP timeouts

The GDB MCP server has a short timeout (~1 second). If the inferior (debugged
program) is running and no breakpoint fires, commands like `info threads` will
time out. Solutions:

1. **Send SIGINT first:** Use `interrupt` command or `kill -SIGINT <pid>` from
   a terminal to pause the program
2. **Set breakpoints before continuing** so the program stops naturally
3. **Don't run GDB commands while the program is executing** — always ensure
   it's paused first

### Coroutine debugging

Coroutine frames may not show full backtraces. If you see truncated stacks
in worker threads, the coroutine machinery (e.g., `coro::thread_pool::executor`)
hides the logical call chain. Focus on:
- The coroutine's local variables and state
- Setting breakpoints inside the coroutine body rather than relying on caller info

## Quick Reference

| Command | Purpose |
|---------|---------|
| `mcp_gdb-mcp_gdb_start()` | Start GDB session |
| `mcp_gdb-mcp_gdb_command(id, cmd)` | Run GDB command |
| `mcp_gdb-mcp_gdb_terminate(id)` | End session |
| `file <path>` | Load binary |
| `run` | Start program |
| `break <loc>` | Set breakpoint |
| `continue` | Resume |
| `interrupt` | Pause |
| `bt` | Backtrace |
| `print <expr>` | Inspect value |
| `info threads` | List threads |
| `thread <n>` | Switch thread |
