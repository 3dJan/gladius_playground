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
| `sphere_gyroid` | boolean + lattice | Create a sphere with a 5 mm radius intersected with a gyroid structure | each axis span 8–12 mm, volume 10–523 mm³, ≥500 triangles, fill fraction 0.01–0.46 |
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

* **Seeded document** — every run starts from the same seed the interactive app uses
  (`design::seedVolumetricTemplate()` loads `examples/template.3mf`), unless
  `GLADIUS_AGENT_BENCH_DOCUMENT_SEED=blank` asks for the empty `Document::newModel()` document so
  the run can prove the renderability gate fires. A missing template is a hard initialisation error,
  never a silent fall back to a blank document. The template's root is `main (ID: 3)` with
  `(vec3 color, float shape) main_3(vec3 pos)` plus a level set, a bounding-box domain mesh and a
  build item, which is what the system prompt in
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
* **Empty-completion guard** — a turn that returns neither text nor a tool call is a provider fault,
  not an agent decision. It used to fall into the "the agent is finished" branch, which ended the run
  as a silent success with an empty document; `agent::AgentLoop` now sends a corrective message and
  retries twice (`empty_completion_retry` note, `emptyTurns` in the report) before failing the run
  with a classified `MalformedResponse` error that names the output limit when the stop reason says so.
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
| `GLADIUS_AGENT_BENCH_TOOL_SURFACE` | `geometry` | `geometry` registers only the eleven geometry tools (see [Lean tool surface](#lean-tool-surface)); `full` registers all of them, as the interactive app does. A role name (`requirements`, `decomposition`, `component`, `review`) selects that role's tool allowlist *and* its system prompt, which is how one run simulates one agent of the hierarchical topology |
| `GLADIUS_AGENT_BENCH_SCOPE_NODE` | unset | design node to scope the live context to (subtree + inherited interface contract only; requirements, todos and sibling branches are withheld). Empty = whole graph, the orchestrator view |
| `GLADIUS_AGENT_BENCH_DOCUMENT_SEED` | `template` | `template` seeds `examples/template.3mf` (the same seed a UI "new model" run gets); `blank` starts from `Document::newModel()`, which has no level set or build item, so the renderability gate and `ensure_output_chain` repair path are what decide the outcome |
| `GLADIUS_AGENT_BENCH_AUTO_FINISH_GRAPH` | `1` | `finish_design` derives the design-graph node, variant and active configuration itself instead of expecting four hand-written tool calls |
| `GLADIUS_AGENT_BENCH_MAX_VIEWPORT_REVIEWS` | `2` | screenshots a task may request; further calls fail with an error that points at `finish_design`. `0` is unlimited |
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
  `truncatedTurns`, `emptyTurns`
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

## What the agent starts from, and what `finish_design` guarantees

A program is not a model. Until a function is sampled by a level set and that level set is placed by
a build item, the scene is empty, the viewport is blank, and both the judge and the mesh gates have
nothing to measure. Two facts used to hide that from the agent:

* `Document::newModel()` succeeds — it builds a valid-void root that renders nothing.
* `finish_design` only validated the design *graph*, and `autoCompleteDesignGraphOnFinish` derived
  the missing bookkeeping itself, so a document that rendered nothing still returned
  `{"success": true, "valid": true}`.

Three things close that loop, and every caller shares one implementation:

* **`design::collectRenderabilityFacts(document)`** — one probe that answers "would this document
  render?" and returns typed blockers (`no_build_items`, `no_levelset`, `levelset_without_function`,
  `root_without_shape_output`, `root_output_unsourced`, …), each with the fix that clears it. It
  feeds `get_model_summary`, the blank-scene note in `capture_viewport`, the per-turn live context
  block, and `finish_design`.
* **`finish_design` gate** — the document is checked *before* the graph. A non-renderable document
  fails with `renderabilityBlockers` instead of a pass, so "I think I'm done" is no longer
  self-certifying.
* **`ensure_output_chain`** — the repair tool the blockers name. It reconciles the scene with the
  function that should be visible: an existing level set is rebound to that function (`chainAction:
  rebound`), a chain that already samples it is left alone (`already-wired`), and only when the scene
  has none at all are the domain mesh, level set and build item created (`created`, through
  `io::createVolumetricOutputChain`, the same builder the MCP `create_level_set` tool uses). The tool
  then re-probes and returns the new facts, so the agent can fix a blank scene in one call rather
  than six. Reconciling instead of appending matters: a second level set on the same function places
  the geometry twice, which doubles the measured volume and bounding box.

The live context line the agent sees is deliberately blunt:

```
- THE MODEL DOES NOT RENDER YET: no_levelset -> Call ensure_output_chain ...
```

`GLADIUS_AGENT_BENCH_DOCUMENT_SEED=blank` runs the whole benchmark against the seed that used to
produce a confident, empty document. It is the regression test for the gate: the geometry gates and
`agentCompleted` must reflect the scene, not the agent's opinion.

Measured, on `strata/iq3_s` (one run per seed, `python3 scripts/agent_bench_compare.py` to re-check):

| Seed | Passed | What the gate did |
| --- | --- | --- |
| `template` | 2/3 to 3/3 | The scene was already wired, so every `ensure_output_chain` call reported `chainAction: already-wired` and no run paid for a repair. |
| `blank` | 2/3 | The line is in the live context of every turn until the scene renders: on the blank-seed brick task, 13 requests carried `THE MODEL DOES NOT RENDER YET` and the seven that followed `ensure_output_chain` returning `chainAction: created` did not. The seeded run carries it zero times. `ensure_output_chain` failed once on each sphere task with a real error — `Function 3 does not exist. Read get_model_summary for the ids.` and `Function 3 has no scalar 'shape' output…` — and the agent recovered from both. `sphere_r5` and `lego_brick_3x4` still passed; `sphere_gyroid` produced a program whose surface the bounds probe could not find, so it failed `mesh_export` instead of passing on the judge alone — exactly the old silent pass, now caught. |

Before this change a blank-seed run scored 3/3 while exporting three empty meshes.

The gate is not free. The eleventh tool schema raises the fixed prefix from 1 709 to 1 844 tokens per
turn (+8 %); the system prompt itself did not grow. Everything else is turns: a seeded document is
already wired, yet the model still calls `ensure_output_chain` once or twice per task, and each extra
call is another turn that pays for the whole prefix. Five seeded runs after the gate landed took 24 to
39 turns and 95 848 to 195 724 prompt tokens, against 23 turns / 79 510 tokens before it. Two follow-ups
the profile suggests: hide `ensure_output_chain` while the scene already renders (the probe can decide
that), and return a near-empty result for an already-wired chain so calling it costs almost nothing.

On the seeded document `sphere_r5` and `sphere_gyroid` passed all five runs; `lego_brick_3x4` is the
noisy one, and its failures are task quality rather than a harness regression. Seven seeded lego runs
after the gate landed: two passed (fill 0.40 — a real shell with studs and clutch tubes — and 0.66,
which is the committed baseline), three exported a block that was too solid (fill 0.81, 0.96 and 1.02:
the model never hollowed the underside or added the clutch tubes), and two produced a program the
harness could not measure at all (a rejected snippet left the bounds probe with no surface; imported
library functions were left with unconnected `pos` and `height` inputs). A single lego failure is
therefore a prompt to re-run, not a verdict; the same task has cost anything from 33 k to 148 k prompt
tokens, which is why the comparer gates pass counts first.

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
| `note` | `AgentLoop` | `kind`: `retry_after_failure`, `autonomous_nudge`, `truncation_nudge`, `empty_completion_retry` — this is how wasted turns get attributed |

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

Measured effect on the LEGO task (same endpoint, same model, `tools` breakpoints), against the baseline
that was committed at the time:

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

Read-back now reaches the full prefix on a short, image-light task: with the per-request tunables at
their defaults and `tools` breakpoints, `sphere_r5` caches 99.8% of its prompt (`cacheMissTurns`
9 -> 0) while the two screenshot-heavy tasks stay near 4-7%, because every retained screenshot sits
inside the cached window and is re-written on the next turn. `GLADIUS_AGENT_PROVIDER_DUMP` plus
replaying the dumps is how that gets bisected further.

Guard verification run (same endpoint, model `gyro-gyro-m`, all three tasks passing):

| task | turns | tool calls | prompt tokens | cache hit | contextTax | `emptyTurns` |
| --- | --- | --- | --- | --- | --- | --- |
| sphere_r5 | 9 -> 7 | 9 -> 8 | 25 378 -> 20 571 | 0% -> 99.8% | 6.99 -> 5.92 | 0 |
| sphere_gyroid | 12 -> 9 | 15 -> 10 | 45 892 -> 29 391 | 0% -> 6.5% | 8.10 -> 7.07 | 0 |
| lego_brick_3x4 | 9 -> 10 | 12 -> 11 | 49 701 -> 48 042 | 0% -> 4.1% | 6.67 -> 7.66 | 0 |

`emptyTurns` stayed at zero across the run, which is what "the fix does not fire on healthy turns"
means; the guard itself is covered by unit tests rather than by a benchmark gate, since provoking it
needs a provider that answers with an empty completion. `wallMs` roughly doubled on the two
screenshot tasks because that run used a different (heavier reasoning) model than the committed
baseline — wall time stays a reported, never gated, metric for exactly this reason. The same
comparison flags `lego_brick_3x4` `outputTokens` 3 574 -> 6 321 as a secondary regression, which is
also model-driven: hidden reasoning tokens are counted as output.

### Lean tool surface

Every turn pays for the whole prompt prefix, so the cheapest token to remove is one the model is
never going to call. The default run therefore configs the harness the way a design session actually
works (`GLADIUS_AGENT_BENCH_TOOL_SURFACE=geometry`, the default):

| Knob | Default | Effect |
| --- | --- | --- |
| `GLADIUS_AGENT_BENCH_TOOL_SURFACE` | `geometry` | Register only the eleven tools a geometry session needs (`begin_edit`, `commit_edit`, `get_program_snippet`, `set_program_snippet`, `get_model_summary`, `ensure_output_chain`, `list_library_items`, `import_library_item`, `export_library_item`, `capture_viewport`, `finish_design`); the planning, knowledge, checkpoint, and variant tools are not registered at all. 2.4 KB of JSON schemas per turn instead of 37 tools / 8.4 KB, and the system prompt drops the ceremony and planning sections only those tools need (4.2 KB -> 3.3 KB). |
| `GLADIUS_AGENT_BENCH_AUTO_FINISH_GRAPH` | `1` | `finish_design` wires the built root function into design → level set → mesh → build item itself. The chain is six tool calls the model has no opinion about. |
| `GLADIUS_AGENT_BENCH_MAX_VIEWPORT_REVIEWS` | `2` | Screenshots a geometry-mode session may request; further calls fail with an error that points at `finish_design` instead of paying for another ~800-token image that stays in every later turn. |

`transport=direct` (in-process, no socket) and `mcp` are equivalent for cost — direct mode only saves
the startup noise. `GLADIUS_AGENT_BENCH_TOOL_SURFACE=full` restores the previous configuration, which
is what `baseline.json` was first captured with.

What it bought, on the same machine, endpoint (`strata/iq3_s`) and task set:

| Task | full surface | lean surface |
| --- | --- | --- |
| `sphere_r5` | 45 765 | 21 796 |
| `sphere_gyroid` | 92 921 | 27 562 |
| `lego_brick_3x4` | 255 279 | 30 152 |
| **total** | **393 965** | **79 510** |

Two consecutive lean runs landed within 0.3 % of each other on total prompt tokens (the
full-surface runs spread by ~15 %): with almost nothing left in the prompt and no retries the run is
close to deterministic, which makes the cost gates trustworthy again.

That last claim did not survive the renderability gate. Five lean runs after the gate landed totalled
95 848, 109 268, 122 049, 142 448 and 195 724 prompt tokens — a spread of 2x — while only two of them
passed 3/3. Almost all of the spread is turn count: the local model needs 6 to 17 turns for the same
shape, and a seeded document is wired already yet `ensure_output_chain` still gets called once or twice
per task. The committed baseline is now one of the post-gate 3/3 runs (122 049 prompt tokens, first
turn 1 844), so the trustworthy signals are the pass count and the per-task counters, not the total.


Two consequences to know about before reading the report:

* The first turn is now ~1.7 k tokens, below this endpoint's cache minimum, so `promptCacheHitRate`
  is 0.0 and the comparer skips that gate (see `--cache-min-prompt-tokens`). Total prompt tokens are
  still gated; with a lean prefix the cache is a rounding error.
* Cutting the tool list cuts the model's escape hatches too, so the graph path has to be solid.
  Three harness bugs had to be fixed before the lean configuration could pass 3/3: a snippet that
  used the `in_<name>` port aliases wrote those names back into `main`'s signature as parameters
  (`ExpressionToGraphConverter` auto-declared any unknown `in_*` identifier), which left the
  assembly-level call node with an input that had no source and made *every* later refresh fail
  while `set_program_snippet` still reported success; `syncFunctionCallPorts` pruned obsolete
  *outputs* but never the inputs a dropped parameter had left behind; and `list_library_items`
  matched the whole query as one substring, so `{"query": "lego brick"}` returned nothing even
  though the mechanical library contains a brick. Each has a unit test.

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
  gates on pass counts and tolerant cost deltas rather than exact numbers. On `iq3_s` with the old
  full tool surface the spread between two consecutive all-passing runs was large — the gyroid task
  measured 13 turns / 93 k prompt tokens in one run and 19 turns / 190 k in the next — so treat a
  single cost regression on one task as a prompt to re-run, not as a verdict. The lean geometry
  surface retries far less, and its two consecutive runs differed by 0.3 % overall. Pass counts
  remain the trustworthy signal.
* A fresh WebGPU document can report `BuildVolumeFallback` bounds; the geometry gates therefore
  measure the **exported mesh** rather than trusting the bounds probe, and bounds are recorded only
  as context.
* `finish_design` requires the full design-graph ceremony, so a model that never performs it shows
  up as `agentCompleted=false` with the geometry of whatever it did manage to build.
