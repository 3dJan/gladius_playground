# GPU Access Coordination

Gladius uses multiple OpenCL command queues: one lazily-created queue per calling thread plus explicit worker queues for async rendering. OpenCL queues do not implicitly synchronize with each other, so shared buffers/images must be ordered explicitly.

## Core invariant

Every shared OpenCL memory-object access across queues must be ordered by one of:

1. an event wait list produced by `GpuAccessCoordinator`, or
2. an explicit host wait that is reported back to the coordinator.

Host-side mutexes are not sufficient unless they are held until the GPU command using the protected resource has completed.

## Main types

- `GpuAccessCoordinator` is the pure, GPU-free state model. It tracks resource IDs, generations, read/write leases, fake/production event IDs, stale generations, and deferred retirement.
- `OpenClEventRegistry` maps coordinator event IDs to `cl::Event` objects and polls or waits for completion.
- `GpuKernelAccessGuard` is the production RAII bridge. It begins one or more resource accesses, converts dependency IDs to OpenCL wait lists, and records the returned kernel event when the enqueue succeeds.
- `RenderPayloadSnapshot` captures the generation handles for the common render payload: primitive metadata, primitive data, precomputed SDF, parameter buffer, and command buffer.

## Published generations

Each `Buffer<T>` and `ImageImpl<T>` owns a stable coordinator resource ID and a current generation. Host writes, buffer reallocations, image reallocations, and clears retire the previous generation before mutating or releasing the backing OpenCL object.

A kernel launch should use the generation handles captured at launch time. If a later host write advances a generation, stale snapshots can be rejected instead of silently reading mixed old/new state.

## Conservative first-slice policy

The first implementation is whole-resource based:

- reads can overlap with reads;
- writes wait for previous readers and writers;
- reads wait for the previous writer;
- `Primitives::data` is tracked as one conservative resource even though it contains several logical regions;
- old generations are not released until known events have completed.

This is intentionally safer than region-level aliasing. Region-level optimization can be added later once workflow tests prove the invariants.

The current production rollout also prefers conservative generation retirement over immediate physical double-buffering. Host writes and reallocations wait for tracked readers/writers before mutating the backing object, so the implementation is safe even when only one physical buffer exists. Targeted double buffers can still be introduced later for performance/overlap (`PreComputedSdf`, `DistanceInitBuffer`, low-res preview images, and small parameter/command buffers), but they are no longer required to prevent cross-queue memory-object races.

## Safe mode

Guarded GPU access uses the conservative serialized mode by default. Each guarded launch drains tracked GPU work before enqueue and waits for its own returned event before returning. This is slower than maximally overlapping command queues, but it keeps the production behavior deterministic and avoids environment-dependent synchronization semantics.

## Adding a new kernel launch

1. Identify every shared OpenCL buffer/image argument.
2. Capture the relevant generation handles before enqueue.
3. Create a `GpuKernelAccessGuard` with read/write modes.
4. Pass `guard.waitEvents()` into `CLProgram::runNonBlockingWithWaitList(...)`.
5. Call `guard.complete(returnedEvent)` immediately after a successful enqueue.

If a launch cannot declare all shared resources yet, prefer a conservative whole-resource access over leaving the path untracked.
