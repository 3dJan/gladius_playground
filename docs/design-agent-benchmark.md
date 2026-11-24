# Design Agent Benchmark

The design-agent benchmark runs the **real** native design agent (agent loop, design tools, design
graph, permission policy) headlessly against a local LLM, measures how well the harness performs,
and decides whether the produced geometry is actually what was asked for.

It exists because the agent was previously only reachable through the ImGui panel
(`src/ui/AgentPanels.cpp`), so there was no way to run it unattended, no way to quantify harness
quality, and no way to tell whether the model that came out was correct.

## Tasks

| id | complexity | prompt | deterministic gates |
| --- | --- | --- | --- |
| `sphere_r5` | primitive | Create a sphere with a radius of 5 mm | each axis span 8–12 mm, volume 300–780 mm³, ≥200 triangles, fill fraction 0.45–0.60 |
| `sphere_gyroid` | boolean + lattice | Create a sphere with a 5 mm radius intersected with a gyroid structure | each axis span 8–12 mm, volume 10–523 mm³, ≥500 triangles, fill fraction 0.05–0.46 |
| `lego_brick_3x4` | dimensional parametric | Create a 3×4 LEGO compatible brick | spans 7–14 / 20–28 / 28–36 mm, volume 1000–9000 mm³, ≥500 triangles, fill fraction 0.20–0.75 |

Task table: [`tests/unittests/agentBench/AgentBenchTask.h`](../tests/unittests/agentBench/AgentBenchTask.h).

Span gates are **order-independent**: the three measured spans and the three accepted ranges are each
sorted ascending before they are paired (`min_span_mm`, `mid_span_mm`, `max_span_mm`), so the agent may
orient a part however it likes.

The **fill fraction** (mesh volume divided by mesh bounding-box volume) is what separates a shape from
the solid it is cut or built from: a solid block fills 1.0, a sphere of the measured spans 0.52, an open
lattice or a hollow shell clearly less. For the LEGO brick this gate — not the camera — decides the
"hollow underside" requirement: the renderer lights from above, so downward facing faces always shade to
black and a straight-from-below view tells the judge nothing. A solid brick without the cavity fails
`fill_fraction`.

## Verdict rule

```
taskPassed = judgeAvailable AND judgePasses AND allGeometryGatesPassed
```

* **Visual judge** — one fresh, toolless completion against the same model: the rendered views plus
  the task rubric, answered as strict JSON
  `{"passes": bool, "confidence": 0..1, "observed": "...", "issues": [...]}`.
* **Geometry gates** — the exported mesh is measured independently of the model: binary STL is
  parsed and volume (divergence theorem), bounding box and triangle count are checked against the
  task table. The `finish_design` ceremony (requirement → function → variant → configuration →
  activate → finish) is what makes the agent *complete* at all, and `agentCompleted` is reported
  separately so "never finished" is distinguishable from "finished but wrong geometry".

## How the harness works

* **Seeded document** — every run starts from `tests/unittests/examples/template.3mf`, not from
  `Document::newModel()`. A `newModel()` root is a managed void whose resource id is `0`, and
  `set_program_snippet` can never declare or replace id `0`, so nothing a scripted or real agent
  writes would ever be rendered. The template's root is `main (ID: 3)` with
  `(vec3 color, float shape) main_3(vec3 pos)`, which is what the system prompt in
  `agent::ContextBuilder::defaultSystemPrompt()` tells the model to preserve.
* **Rounds** — the harness re-seeds the document, graph, todos, checkpoints, knowledge and
  conversation before each task, and sandboxes the user library in `<output>/user-library`
  (`GLADIUS_USER_LIBRARY_DIR`) so imports and exports are counted hermetically.
* **Judge views** — the camera is framed with the same `ui::OrbitalCamera` +
  `adjustDistanceToTarget()` path used by the thumbnail renderer, then zoomed out by an extra 60 %
  so the model never touches the image border; three presets (isometric, front, top) are rendered.
* **Mesh export** — the layered marching-cubes exporter repairs degenerate triangles
  (`io::collapseGeometricallyDegenerateTriangles` + `io::repairTriangleWindingConsistency`) before
  validating, the same way the manifold exporter and the coloured 3MF writer already do. Without it,
  high-frequency lattices such as a LEGO brick fail export with *"Mesh contains a degenerate
  triangle"* and no geometry gate can run at all.
* **Output-limit recovery** — `ProviderConfig::maxTokens` is raised for benchmark runs, and
  `agent::AgentLoop` counts turns the provider cut off (`truncatedTurns` in the report) and replies
  to a cut-off answer with an instruction to emit snippets one helper function at a time. Cut-off
  answers are the main cause of `set_program_snippet: snippet is required` failures.
* **Robust tool arguments** — array-valued tool arguments also accept a single string
  (`"tags": "gyroid"`), because local models get this wrong often enough to dominate the failure
  count otherwise. The system prompt states the rule as well.

## Running

The benchmark is part of the normal `gladius_test` binary but is **off by default**: it needs a GPU
runtime and a reachable LLM endpoint, and each task runs for minutes.

```bash
cmake --preset macos-webgpu-releaseWithDebug
cmake --build out/build/macos-webgpu-releaseWithDebug --target gladius_test

cd gladius/out/build/macos-webgpu-releaseWithDebug/tests/unittests
GLADIUS_RUN_AGENT_BENCH=1 ./gladius_test --gtest_filter='DesignAgentBench*'
```

Run the binary directly rather than through CTest — the per-task cases are long-running and the
aggregate report test must see the other runs in the same process.

> **The working directory must be the build's `tests/unittests` directory.** On macOS the executable
> cannot resolve its own location (`getAppDir()` reads `/proc/self/exe`, which does not exist), so
> `examples/template.3mf` and `library/` are resolved relative to the *current directory*. Started
> from anywhere else the benchmark silently seeds a **blank document**: `get_program_snippet` returns
> `""`, nothing renders, every `set_program_snippet` call fails, and the whole run measures nothing.
> The same applies to the shipped `library/` used by `list_library_items`.

### Environment

| variable | default | meaning |
| --- | --- | --- |
| `GLADIUS_RUN_AGENT_BENCH` | unset | `1` enables the benchmark |
| `GLADIUS_AGENT_BENCH_ENDPOINT` | `http://localhost:47831/v1/messages` | provider endpoint |
| `GLADIUS_AGENT_BENCH_MODEL` | `iq3_s` | model name |
| `GLADIUS_AGENT_BENCH_API_KEY` | empty | sent as `x-api-key` when non-empty |
| `GLADIUS_AGENT_BENCH_API` | `anthropic` | `anthropic` or `openai` wire format |
| `GLADIUS_AGENT_BENCH_STREAM` | `1` | use SSE streaming |
| `GLADIUS_AGENT_BENCH_MAX_TURNS` | `40` | model-turn budget per task |
| `GLADIUS_AGENT_BENCH_MAX_TOOL_CALLS` | `200` | tool-call budget per task |
| `GLADIUS_AGENT_BENCH_WALL_MINUTES` | `30` | wall-clock budget per task (safety net; the turn budget is what shapes the metrics) |
| `GLADIUS_AGENT_BENCH_TIMEOUT_SECONDS` | `600` | per-request HTTP timeout (a local model can spend minutes on one turn) |
| `GLADIUS_AGENT_BENCH_MAX_TOKENS` | `8192` | completion budget per model turn (a whole program snippet must fit) |
| `GLADIUS_AGENT_BENCH_THINKING_TOKENS` | `1024` | hidden-reasoning budget per turn; `0` leaves it to the provider. Uncapped reasoning dominated wall time and once consumed the whole judge budget without producing a verdict |
| `GLADIUS_AGENT_BENCH_CACHE_BREAKPOINTS` | `tools` | explicit `cache_control` placement: `none`, `tools` (after the tool schemas) or `all` (also after the newest message). `none` measurably degrades outcomes, so `tools` is the default |
| `GLADIUS_AGENT_BENCH_RENDER_SIZE` | `512` | square render resolution |
| `GLADIUS_AGENT_BENCH_OUTPUT` | `agent_bench` | output directory for report + artifacts |
| `GLADIUS_USER_LIBRARY_DIR` | unset | hermetic user-library sandbox (also used by the benchmark) |
| `GLADIUS_AGENT_PROVIDER_DUMP` | unset | write each provider request as `request-NNN.json` in this directory (images redacted to their byte count). This is how a prompt-cache regression is found: diff consecutive requests and check that every byte sent before is still sent unchanged |

Benchmark runs are always traced (`<output>/<taskId>/trace.jsonl`). For the interactive app the
trace is opt-in — see [Profiling and bottleneck analysis](#profiling-and-bottleneck-analysis):
`GLADIUS_AGENT_TRACE=<file>` and `GLADIUS_AGENT_TRACE_PROMPT=1`.

## Metrics

Per run ([`results.json`](../tests/unittests/agentBench/AgentBenchReport.h)):

* `modelTurns`, `toolCalls`, `toolFailures`, `unknownToolCalls`, `providerFailures`,
  `truncatedTurns`
* `perTool` — calls and failures for every tool name
* `librarySearches`, `libraryImports` (distinct entries), `libraryExports`, `libraryFilesAdded`
* `viewportCaptures`, `designGraphMutations`
* `usage` — prompt/completion/cache tokens summed over all turns, plus `usageWithJudge`
* `wallMs`, `agentCompleted`, `agentError`, `failureDetails`
* `geometry`, `geometryChecks`, `judge`, `verdict`, `artifacts`
* `trace.profile` — the per-step aggregate described under
  [Profiling and bottleneck analysis](#profiling-and-bottleneck-analysis)

Artifacts per task under `<output>/<taskId>/`: `view-isometric.png`, `view-front.png`,
`view-top.png`, `model.stl`, `model.3mf`, `trace.jsonl`, `timeline.json`; the sandboxed library is
`<output>/user-library`.

## Comparing against a baseline

```bash
python3 scripts/agent_bench_compare.py \
    gladius/tests/baselines/agent_bench/baseline.json \
    gladius/out/build/macos-webgpu-releaseWithDebug/tests/unittests/agent_bench/results.json
```

Exit codes: `0` no regression, `1` at least one gate regressed, `2` task-set mismatch. The primary
gate is the number of passing tasks; cost/effort metrics (turns, tool calls, tool failures,
provider failures, unknown tool calls, truncated turns, prompt/output tokens) additionally fail
when they worsen by more than *both* `--token-tolerance` (default 50 %) and `--counter-grace`
(default 5 absolute), so a single extra retry is noise while a systemic regression is not.
`wallMs` is reported but never gates: the local server's throughput depends on machine load.

## Profiling and bottleneck analysis

`results.json` says *how much* a run cost. The trace says *where it went*, which is what makes the
harness optimizable. One recorder lives in the product ([`src/agent/trace/AgentTrace.h`](../src/agent/trace/AgentTrace.h))
so an interactive session and a benchmark run produce the same records.

### Records

Every run writes `<output>/<taskId>/trace.jsonl`, one JSON object per line. All records carry `tMs`
(milliseconds since run start) and `runId`; the type decides the rest.

| `type` | emitted by | interesting fields |
| --- | --- | --- |
| `run_meta` | host | model, endpoint, limits — describes the run, never a timeline span |
| `turn` | `AgentLoop` | `durationMs`, `buildMs` (context assembly), `completionMs`, `ttftMs`, `firstTokenMs` (first generated token, reasoning included), `firstContentMs` (first *visible* output), `streamEvents`, `thinkingChars`, `textChars`, `toolJsonChars`, `promptBytes` (per component), `context` (`imagesKept`, `messagesSent`, `messagesPruned`), `usage`, `stopReason` |
| `tool_call` | `AgentLoop` | `name`, `policy`, `durationMs`, `success`, `denied`, `argsBytes`, `resultBytes`, trimmed `error` |
| `phase` | harness | `name` (`seed_document`, `export_stl`, `measure_mesh`, `save_document`, `render:isometric`, `judge`), `durationMs` |
| `note` | `AgentLoop` | `kind`: `retry_after_failure`, `autonomous_nudge`, `truncation_nudge` — this is how wasted turns get attributed |

With `GLADIUS_AGENT_TRACE_PROMPT=1` each `turn` also carries the full request (system prompt plus
messages). That is megabytes per run, so it stays opt-in; it is the only way to see *what* the model
was actually sent when the aggregate says the prompt is too big.

Alongside it, `<output>/<taskId>/timeline.json` is Chrome Trace Event JSON: open it in
[Perfetto](https://ui.perfetto.dev) or `chrome://tracing`. Two lanes keep agent work and measurement
work from overlapping, and each turn is split into nested `context build` / `queue+prefill` /
`thinking` / `generate` slices, which is exactly the split an optimization can attack.

### Aggregates

The same records are aggregated into `trace.profile` inside `results.json` (and printed per task), and
[`scripts/agent_bench_profile.py`](../scripts/agent_bench_profile.py) recomputes them from a trace file
without rebuilding:

```bash
python3 scripts/agent_bench_profile.py /tmp/agent_bench_v6/sphere_r5/trace.jsonl
python3 scripts/agent_bench_profile.py /tmp/agent_bench_v6   # whole run, all tasks
python3 scripts/agent_bench_profile.py /tmp/agent_bench_v4 /tmp/agent_bench_v6   # before/after diff
```

| question | aggregate |
| --- | --- |
| Is the harness or the model the bottleneck? | time split: `modelMs` / `toolMs` / `harnessMs` / `contextBuildMs` / `unaccountedMs` (`unaccountedMs` is reported instead of being hidden) |
| Is a turn slow because of prefill, hidden reasoning, or decoding? | `modelPrefillMs` / `modelThinkingMs` / `modelGenerateMs`, `ttftP50Ms`/`ttftP95Ms` and `prefillTokPerSec`/`decodeTokPerSec` — prefill ends at the **first generated token**, because a reasoning model starts generating long before any of it is visible; thinking is the gap to the first visible output |
| Is the prompt cache working at all? | `cacheMissTurns`, `cacheRewindTurns` (turns whose cached prefix got *shorter* — the signature of history being rewritten in place), `systemPromptMutations`, `historyPrunes` / `prunedMessages` |
| Is the prompt the problem, and which part of it? | `contextTax` (Σ per-turn prompt tokens ÷ final-turn prompt tokens — how much history is re-sent), `promptBytesByComponent` |
| Did one tool result poison the context? | `largestToolResultBytes` + turn |
| Which tool should be made cheaper first? | tool ranking by total time, with failures and output bytes |
| How much did we spend on mistakes? | `failedToolCalls`, `turnsAfterFailure` with their milliseconds and prompt tokens, `truncatedTurns`, nudges |
| Is the model acting or talking? | `turnsWithoutToolCalls` (one is the closing turn, more is hesitation) vs `mutatingToolCalls` |
| Did the last change help? | run the script with two traces and read the diff |

A run directory works as input too: the script then aggregates every `<taskId>/trace.jsonl` below it.
Wall time and `contextTax` are combined per task, because each task's clock restarts at zero.

### What profiling the harness found

Baseline run (strata `iq3_s`, all three tasks passing, ~41 minutes of wall time) split roughly 88%
prompt prefill, 9% output generation, 3% harness measurement (the vision judge alone is about two
thirds of that) and under 1% tool execution. Tools are therefore not the place to optimize; the
prompt and the model are. `promptBytesByComponent` says why: images dominate by an order of
magnitude (about 60 MB of image bytes over a run against 2.9 MB of other tool results), and
`capture_viewport` alone returned 6.3 MB. A `contextTax` of 10–20x means most prompt tokens are
re-sent history.

That profile drove four harness changes, each of which the same aggregates then confirmed:

1. **Hidden reasoning was being billed to prefill.** With the reasoning split enabled, the LEGO task
   spends 137 s prefilling, 112 s *thinking* and 51 s generating. Uncapped thinking had already blown
   the task budget once, and the vision judge can spend its whole completion budget thinking before
   it emits a verdict — hence the explicit budgets in the environment table.
2. **History was being rewritten in place.** Superseded screenshots were replaced by stub text and old
   tool results were re-serialized, which changed bytes the server had already cached, so the cached
   prefix could never grow and every turn re-prefilled the conversation. `cacheRewindTurns` reports
   the failure mode, and a context-policy test asserts the byte-level invariant instead.
3. **A required snippet format was undocumented.** `set_program_snippet` accepts a snippet only when
   every function block carries its `// Function: <name> (ID: <n>)` header. One run paid 42 failed
   tool calls and 9 minutes to learn this, so the requirement now sits in the system prompt and in
   the error hint.
4. **Cache breakpoints are a policy, not a constant.** `tools` (cache the system prompt plus schemas)
   beats `none` and `all` on this endpoint; with `none` the LEGO task failed its geometry gates.

Measured effect on the LEGO task (same endpoint, same model, `tools` breakpoints), against the
committed baseline in `tests/baselines/agent_bench/baseline.json`:

| | baseline | current |
| --- | --- | --- |
| model turns | 20 | 22 |
| tool calls / failures | 48 / 1 | 45 / 1 |
| wall time | 868 s | 303 s |
| prompt tokens (with judge) | 359 948 | 255 279 |
| completion tokens | 59 258 | 15 385 |

Turn count barely moved while everything that costs money did: the harness stopped wasting turns on
avoidable tool errors, stopped re-prefilling rewritten history, and stopped paying for unbounded
reasoning. `contextTax` for this task is 14.1x. `wallMs` still varies with machine load, which is why
the comparer reports it but never gates on it.

Still open: on this endpoint the *read-back* amount stays pinned at the system-prompt-plus-tools
prefix even though the conversation prefix is provably append-only, so `promptCacheHitRate` stays
well below what the same requests reach when replayed on their own. `GLADIUS_AGENT_PROVIDER_DUMP`
plus replaying the dumps is how that gets bisected further.

### Interactive sessions

The same trace is available while using the app, so a problem seen live does not have to be
reproduced as a test:

```bash
GLADIUS_AGENT_TRACE=/tmp/gladius_agent/trace.jsonl \
GLADIUS_AGENT_TRACE_PROMPT=1 \
./gladius
```

On run completion the profile summary goes to `stderr` and `trace.jsonl.timeline.json` is written next
to the trace.

## Known limitations

* Local-model runs are nondeterministic: a single run per task is noisy, which is why the comparer
  gates on pass counts and tolerant cost deltas rather than exact numbers. On `iq3_s` the spread
  between two consecutive all-passing runs was large — the gyroid task measured 13 turns / 93 k
  prompt tokens in one run and 19 turns / 190 k in the next — so treat a single cost regression on
  one task as a prompt to re-run, not as a verdict. Pass counts are the trustworthy signal.
* A fresh WebGPU document can report `BuildVolumeFallback` bounds; the geometry gates therefore
  measure the **exported mesh** rather than trusting the bounds probe, and bounds are recorded only
  as context.
* `finish_design` requires the full design-graph ceremony, so a model that never performs it shows
  up as `agentCompleted=false` with the geometry of whatever it did manage to build.
