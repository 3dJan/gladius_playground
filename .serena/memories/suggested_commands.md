VS Code tasks (preferred):
- Build: "Build ALL (linux-releaseWithDebug)"
- Tests: "Run Gladius Tests (linux-releaseWithDebug)" (ctest preset ReleaseWithDebug)
- Focused MDC GPU watertightness regression: "Run MDC watertightness test (GPU, xml)" or "Run MDC watertightness test (GPU)" (sets GLADIUS_RUN_GPU_TESTS=1)
- Focused MDC edge-neighbor offsets test: "Run MDC edge-neighbor offsets test"

Notes:
- Per repo guidelines, avoid running cmake/ninja manually; use tasks.
- Coverage tasks exist (Configure with Coverage + Run Coverage Analysis).
