# OpenCL Binary Cache Assessment Plan

## Step 1 — Catalogue existing cache implementation

- **Action**: Review `CLProgram` sources to map out how binary caching currently works (hashing, save/load paths, validation logic).
- **Reasoning**: Understanding the existing flow is essential before judging production readiness or proposing changes.

## Step 2 — Trace integration points

- **Action**: Follow cache-related APIs (such as `setCacheDirectory` and `setCacheEnabled`) through `ProgramBase` and `ProgramManager` to see where the cache is configured or toggled.
- **Reasoning**: If the pipeline never turns the feature on, we need to document that fact and identify the missing wiring that prevents production use.

## Step 3 — Identify production gaps

- **Action**: Compare the implemented behaviour with production requirements—persistence, invalidation, concurrency, diagnostics—to spot gaps blocking a reliable rollout.
- **Reasoning**: Enumerating concrete shortcomings clarifies the work required to make the cache safe for shipping.

## Step 4 — Analyse enablement risks

- **Action**: Evaluate what failure modes might appear if the cache were simply enabled today (e.g., stale binaries, corrupted writes, driver mismatches).
- **Reasoning**: Highlighting risks informs stakeholders why the feature remains disabled and what mitigations are needed.
