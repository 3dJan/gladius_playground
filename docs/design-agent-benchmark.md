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
| `GLADIUS_AGENT_BENCH_RENDER_SIZE` | `512` | square render resolution |
| `GLADIUS_AGENT_BENCH_OUTPUT` | `agent_bench` | output directory for report + artifacts |
| `GLADIUS_USER_LIBRARY_DIR` | unset | hermetic user-library sandbox (also used by the benchmark) |

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

Artifacts per task under `<output>/<taskId>/`: `view-isometric.png`, `view-front.png`,
`view-top.png`, `model.stl`, `model.3mf`; the sandboxed library is `<output>/user-library`.

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

## Known limitations

* Local-model runs are nondeterministic: a single run per task is noisy, which is why the comparer
  gates on pass counts and tolerant cost deltas rather than exact numbers.
* A fresh WebGPU document can report `BuildVolumeFallback` bounds; the geometry gates therefore
  measure the **exported mesh** rather than trusting the bounds probe, and bounds are recorded only
  as context.
* `finish_design` requires the full design-graph ceremony, so a model that never performs it shows
  up as `agentCompleted=false` with the geometry of whatever it did manage to build.
