# Graph / Assembly → OpenCL (or CommandStream) → compilation

This page explains how Gladius turns the editable graph into executable OpenCL, how compilation is orchestrated, and where the parameter “fast path” fits.

Canonical implementation files:

- `gladius/src/Document.cpp` (`Document::refreshWorker`, `updateFlatAssembly`)
- `gladius/src/compute/ComputeCore.cpp` (`ComputeCore::refreshProgram`)
- `gladius/src/compute/ProgramManager.cpp` (`ProgramManager::setModelSource`, `recompileIfRequired`, signature tracking)
- `gladius/src/nodes/ToOCLVisitor.cpp` (`nodes::ToOclVisitor` code generator)
- `gladius/src/nodes/ToCommandStreamVisitor.cpp` (`nodes::ToCommandStreamVisitor` command stream generator)
- `gladius/src/ui/render/AsyncRenderTypes.h` (job types include `ProgramCompilation`, `ParameterUpdate`, `SDFPrecomputation`)

## End-to-end flow (from a “compile” button)

The main workflow is initiated by the UI calling `MainWindow::refreshModel()` → `Document::refreshModelIfNoCompilationIsRunning()`.

Actual heavy lifting happens off the UI thread in `Document::refreshWorker()`.

```mermaid
flowchart TD
    UI[UI: MainWindow::refreshModel] -->|calls| D1[Document::refreshModelIfNoCompilationIsRunning]
    D1 -->|starts| D2[Document::refreshModelAsync]
    D2 -->|std::async| W[Document::refreshWorker (background)]

    W -->|Compute token| T[ComputeCore::waitForComputeToken]
    W --> UIO[Assembly::updateInputsAndOutputs]
    W --> REG[Document::updateParameterRegistration]
    W --> PARAM[Document::updateParameter]
    W --> FLAT[Document::updateFlatAssembly]

    FLAT -->|lower + optimize + flatten| L1[LowerFunctionGradient]
    FLAT --> L2[LowerNormalizeDistanceField]
    FLAT --> OPT[OptimizeOutputs]
    FLAT --> GF[GraphFlattener::flatten]

    W -->|set code| RP[ComputeCore::refreshProgram(flatAssembly)]
    RP -->|generates source| PM[ProgramManager::setModelSource]
    W -->|compile| C1[ComputeCore::recompileIfRequired]
    C1 --> C2[ProgramManager::recompileIfRequired]
    C2 --> R[RenderProgram::recompileNonBlocking]
    C2 --> S[SlicerProgram::recompileNonBlocking]
    C2 --> DC[HDC/MDC programs recompile]

    W -->|poll| WAIT{isCompilationInProgress?}
    WAIT -->|yes| WAIT
    WAIT -->|no| SDF[ComputeCore::precomputeSdfAsync]
    SDF -->|event wait| BBOX[ComputeCore::updateBBox]
```

## Assembly preparation (lowering + flattening)

`Document::updateFlatAssembly()` performs the transformation from the editable assembly to the “flat” assembly that is used for code generation:

- validates the assembly (`validateAssembly()`)
- runs lowering passes:
  - `nodes::LowerFunctionGradient`
  - `nodes::LowerNormalizeDistanceField`
- optimizes outputs (`nodes::OptimizeOutputs`)
- flattens with `nodes::GraphFlattener::flatten()`

All in: `gladius/src/Document.cpp`.

## Code generation: OpenCL vs CommandStream

`ComputeCore::refreshProgram(nodes::SharedAssembly assembly)` (in `gladius/src/compute/ComputeCore.cpp`) chooses the backend via `m_codeGenerator`:

- `CodeGenerator::Code`
  - uses `nodes::ToOclVisitor`
  - calls `assembly->visitNodes(visitor)`
  - writes OpenCL source into a stringstream
  - `m_programs.setModelSource(optimizedKernel.str())`

- `CodeGenerator::CommandStream`
  - clears and builds a `CommandBuffer`
  - uses `nodes::ToCommandStreamVisitor` + `assembly->visitAssemblyNodes(visitor)`
  - writes the generated kernel code into a stringstream
  - `m_programs.setModelSource(modelKernel.str())`

### Practical note: backend feature differences

The two backends are not strictly feature-identical. If you’re debugging a “works in one generator but not the other” issue, start by comparing the corresponding visitor implementations:

- `gladius/src/nodes/ToOCLVisitor.cpp`
- `gladius/src/nodes/ToCommandStreamVisitor.cpp`

## Compilation orchestration (ProgramManager)

Once `ProgramManager::setModelSource(...)` is called, `ProgramManager` marks compilation as required:

- `m_slicerState.signalCompilationRequired();`
- `m_renderState.signalCompilationRequired();`

`ProgramManager::recompileIfRequired()` then:

- checks whether previous async compilations completed
- if compilation is required:
  - `compileRenderProgram()` (sets model kernel and starts `RenderProgram::recompileNonBlocking()`)
  - compiles slicer program similarly
  - may also recompile meshing programs (HDC/MDC) depending on configuration/state

Source: `gladius/src/compute/ProgramManager.cpp`.

## Parameter “fast path” (avoid recompilation on value-only changes)

There is an explicit mechanism to detect whether parameter *structure* has changed (added/removed parameters), versus only parameter *values* changing.

- `ComputeCore::refreshProgram(...)` captures the signature after code generation:
  - `auto signature = ParameterSignature::compute(*assembly);`
  - `m_programs.setCompiledParameterSignature(signature);`

- `ProgramManager::isParameterSignatureCompatible(Assembly const&)` compares the currently computed signature to the last compiled signature.

- `Document::updateParameter()` uses this to decide whether it can attempt a fast upload path:
  - `bool canUseFastPath = m_core->isParameterSignatureCompatible(*m_assembly);`
  - then tries `m_core->tryToupdateParameter(*m_assembly)`.

Even if the signature mismatches, it still attempts an update, but logs an informational event and relies on the next full refresh to recapture the signature.

## Capability gates (example: NanoVDB)

Compilation may be influenced by device/runtime capabilities. For example, NanoVDB can be required by a model but disabled if unsupported.

Key APIs in `ProgramManager`:

- `setVdbRequired(bool)` (throws if required but not supported)
- `updateVdbActivationLocked()` / `propagateVdbActivationLocked()`

Source: `gladius/src/compute/ProgramManager.cpp`.

## Related deep dives

- Parameter fast path details: [`gladius/docs/architecture/parameter_fast_path_implementation.md`](../../gladius/docs/architecture/parameter_fast_path_implementation.md)
- Async SDF + compilation notes: [`gladius/docs/architecture/async_sdf_and_compilation_implementation.md`](../../gladius/docs/architecture/async_sdf_and_compilation_implementation.md)
