# Build & Test Tasks

Always use VS Code tasks instead of running cmake/ninja/ctest manually in the terminal.

| Goal | Task label |
|---|---|
| Build everything | `Build ALL (linux-releaseWithDebug)` |
| Run all tests | `Run Gladius Tests (ReleaseWithDebug, summary)` |
| Incremental build | `Build incremental` |

Use the `run_task` or `create_and_run_task` tool (or the VS Code task runner) to invoke these tasks.
Use the `testFailure` tool to analyse failing test output.

Example on Linux:


```bash
cd /home/jan/projects/gladius/gladius && cmake --preset linux-relesaseWithDebug -B out/build/linux-releaseWithDebug && cmake --build out/build/linux-releaseWithDebug --parallel 8"
```