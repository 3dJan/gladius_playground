# Gladius developer docs

This folder contains **developer-facing** documentation.

## Onboarding

- Human developer onboarding: [`docs/developer_onboarding.md`](developer_onboarding.md)
- Coding-agent / automation onboarding: [`docs/agent_onboarding.md`](agent_onboarding.md)

## Architecture & workflows (Mermaid)

- Rendering pipeline (UI → async preview → compute): [`docs/architecture/rendering_pipeline.md`](architecture/rendering_pipeline.md)
- Graph/Assembly → OpenCL / CommandStream → compilation: [`docs/architecture/graph_to_opencl.md`](architecture/graph_to_opencl.md)

## Deep dives (existing internal notes)

These are detailed implementation notes that already exist in the repo (they live under `gladius/docs/architecture/`).

- Async bounding box convergence: [`gladius/docs/architecture/async_bbox_flow.md`](../gladius/docs/architecture/async_bbox_flow.md)
- Async SDF + compilation pipeline notes: [`gladius/docs/architecture/async_sdf_and_compilation_implementation.md`](../gladius/docs/architecture/async_sdf_and_compilation_implementation.md)
- Parameter “fast path” signature mechanism: [`gladius/docs/architecture/parameter_fast_path_implementation.md`](../gladius/docs/architecture/parameter_fast_path_implementation.md)
