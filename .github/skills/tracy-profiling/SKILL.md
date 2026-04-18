---
name: tracy-profiling
description: >-
  Profile and analyze Gladius UI-thread performance using Tracy instrumentation
  and the tracy-analyze CLI tool. Use when investigating UI stutters, frame
  drops, mutex contention, or any per-frame performance issue. Covers adding
  Tracy zones, capturing traces, analyzing captures with tracy-analyze, and
  interpreting results to find blockers.
metadata:
  author: gladius
  version: "1.0"
  tools: tracy-analyze, tracy-csvexport, tracy-profiler
---

# Tracy Profiling

Profile Gladius UI-thread performance to find blocking operations, mutex
contention, and hot zones that cause frame drops.

## When to Use

- User reports UI stutters or freezes during editing/compilation
- Need to verify a performance fix actually reduced frame times
- Investigating mutex contention between UI and background threads
- Want data-driven evidence before/after a performance change

## Prerequisites

### Tracy Instrumentation (already in Gladius)

Gladius uses Tracy v0.11.1 via vcpkg. Zones are added with:

```cpp
#include <tracy/Tracy.hpp>

void myFunction()
{
    ProfileFunction;  // macro that creates a ZoneScoped
    // ... function body
}
```

Or with named zones for more specificity:

```cpp
void myFunction()
{
    ZoneNamedN(zone1, "MySpecificSection", true);
    // ... code to profile
}
```

### tracy-analyze CLI

The analysis tool lives at `/home/jan/projects/tracy-analyze`. Install it:

```bash
cd /home/jan/projects/tracy-analyze
pip install -e .
```

It requires `tracy-csvexport`. Build it once:

```bash
cd /home/jan/projects/tracy-analyze
./scripts/build_csvexport.sh
cp build/tracy-csvexport ~/.local/bin/
```

## Workflow

### 1. Add Tracy Zones

Add `ProfileFunction` to suspected hot functions. Focus on:

- **Per-frame functions** called from the render loop (highest impact)
- **Functions that acquire mutexes** (contention risk)
- **GPU dispatch calls** (synchronous waits)

Key locations in Gladius:

| File | Function | Why |
|------|----------|-----|
| `RenderWindow.cpp` | `render()`, `renderAsync()` | Main render loop |
| `ProgramManager.cpp` | `isAnyCompilationInProgress()` | Mutex contention risk |
| `ModelEditor.cpp` | `visitNodes()` | Per-frame graph traversal |
| `Document.cpp` | `validateAssemblyIfDirty()` | Per-frame validation |
| `ComputeCore.cpp` | `renderLowResPreview()` | Synchronous GPU on UI thread |

### 2. Build and Run with Tracy

Build normally — Tracy is always compiled in:

```
Task: Build incremental
```

Run Gladius and perform the operations that cause stutters. Tracy captures
automatically while the application runs.

### 3. Capture a Trace

Option A: Use `tracy-capture` (CLI) to save while Gladius runs:

```bash
tracy-capture -o /tmp/tracy/my_capture.tracy
```

Option B: Use `tracy-profiler` (GUI) at `~/.local/bin/tracy-profiler` to
connect, observe live, and save.

### 4. Analyze the Capture

```bash
# Quick stall report
tracy-analyze capture.tracy --stalls-only

# Full analysis
tracy-analyze capture.tracy

# Focus on a specific zone
tracy-analyze capture.tracy --filter "isAnyCompilation"

# JSON for scripting/comparison
tracy-analyze capture.tracy --format json --top 10

# Specific thread
tracy-analyze capture.tracy --thread 1

# 30 FPS budget instead of 60
tracy-analyze capture.tracy --budget-ms 33
```

### 5. Interpret Results

**Key sections in the output:**

| Section | What to look for |
|---------|-----------------|
| Histogram | Most zones should be <1ms. Tail entries >16ms are stalls |
| UI Stalls | Zones exceeding frame budget — these are your targets |
| Top by Max | Single worst invocations — often mutex waits or GPU syncs |
| Top by Total | Cumulative hogs — death by a thousand cuts |
| Threads | Which threads are active and their total compute time |

**Common patterns:**

- **Mutex contention**: A zone shows huge max but tiny avg. The background
  thread holds a mutex, and the UI thread blocks waiting for it. Fix: use
  non-blocking alternatives (atomic reads, try_lock).

- **Synchronous GPU**: A compute dispatch on the UI thread with >10ms
  duration. Fix: move to async dispatch with result polling.

- **Per-frame O(N) work**: High total time with thousands of calls, each
  <1ms but adding up. Fix: skip work when no structural changes pending,
  or debounce.

## Common Pitfalls

### csvexport -u is essential

Always use the unwrap mode (`-u` flag, which `tracy-analyze` does automatically)
to get per-event data. The default aggregated mode loses timing information
needed to correlate stalls with specific moments.

### Don't build custom C++ tools against Tracy's internal APIs

Tracy uses `short_ptr` (6-byte compressed pointers) that rely on slab
allocators. These pointers are only valid in the original process context.
A standalone tool that loads a `.tracy` file gets different slab mappings, so
`short_ptr::get()` returns invalid addresses and crashes. Use the official
`csvexport` tool instead — it handles this correctly.

### Main thread detection

`tracy-analyze` auto-detects the main thread as the one with the most zones.
This works for Gladius but verify with `--thread <id>` if results look wrong.

### Build with RelWithDebInfo

Tracy zones need symbol names. The `linux-releaseWithDebug` preset includes
them. Pure Release builds strip symbols and zones show as `(unknown)`.

## Quick Reference

```bash
# Install once
cd /home/jan/projects/tracy-analyze && pip install -e .
./scripts/build_csvexport.sh && cp build/tracy-csvexport ~/.local/bin/

# Capture
tracy-capture -o /tmp/tracy/capture.tracy

# Analyze
tracy-analyze /tmp/tracy/capture.tracy --stalls-only
tracy-analyze /tmp/tracy/capture.tracy --format json > report.json

# Compare before/after
tracy-analyze before.tracy --format json > before.json
tracy-analyze after.tracy --format json > after.json
diff <(jq '.stalls' before.json) <(jq '.stalls' after.json)
```
