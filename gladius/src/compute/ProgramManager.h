#pragma once

#include <BitmapLayer.h>
#include <ContourExtractor.h>
#include <DualContouringSamplingProgram.h>
#include <EventLogger.h>
#include <GLImageBuffer.h>
#include <ImageRGBA.h>
#include <ModelState.h>
#include <RenderProgram.h>
#include <ResourceContext.h>
#include <SlicerProgram.h>
#include <compute/HierarchicalDCProgram.h>
#include <compute/ManifoldDualContouringProgram.h>
#include <compute/MeshPreparationProgram.h>
#include <compute/ParameterSignature.h>
#include <compute/types.h>
#include <nodes/BuildParameter.h>
#include <nodes/Model.h>
#include <nodes/nodesfwd.h>
#include <ui/OrbitalCamera.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace gladius
{
    class ProgramManager
    {
      public:
        explicit ProgramManager(SharedComputeContext context,
                                RequiredCapabilities requiredCapabilities,
                                events::SharedLogger logger,
                                SharedResources resources);

        void init();
        void reset();

        ComputeToken waitForComputeToken();
        OptionalComputeToken requestComputeToken();

        [[nodiscard]] SlicerProgram * getSlicerProgram() const;

        /// Return the currently preferred render program.
        /// The optimized program wins once compiled; otherwise the command-stream preview
        /// program is used when available.
        [[nodiscard]] RenderProgram * getBestRenderProgram() const;

        /// Non-blocking variant of getBestRenderProgram(). Returns std::nullopt if background
        /// compilation/loading currently owns the program-manager locks.
        [[nodiscard]] std::optional<RenderProgram *> tryGetBestRenderProgram() const;

        /// Return which render backend is currently selected by getBestRenderProgram().
        [[nodiscard]] RenderBackend getSelectedRenderBackend() const;

        /// Non-blocking variant of getSelectedRenderBackend().
        [[nodiscard]] std::optional<RenderBackend> tryGetSelectedRenderBackend() const;

        /// Non-blocking readiness check for the currently selected render program.
        [[nodiscard]] std::optional<bool> tryIsBestRenderProgramReady() const;

        [[nodiscard]] RenderProgram * getRenderProgram() const;

        [[nodiscard]] RenderProgram * getPreviewRenderProgram() const;

        [[nodiscard]] RenderProgram * getOptimizedRenderProgram() const;

        [[nodiscard]] DualContouringSamplingProgram * getDualContouringSamplingProgram() const;

        [[nodiscard]] HierarchicalDCProgram * getHierarchicalDCProgram() const;

        [[nodiscard]] compute::ManifoldDualContouringProgram * getManifoldDualContouringProgram() const;

        [[nodiscard]] MeshPreparationProgram * getMeshPreparationProgram() const;

        [[nodiscard]] bool isAnyCompilationInProgress() const;

        /// Return whether compilation that blocks model publication is still running.
        /// Optimized render compilation is not blocking while a command-stream preview program
        /// exists; slicer compilation remains blocking for SDF/slicing correctness.
        [[nodiscard]] bool isBlockingCompilationInProgress() const;

        /// Non-blocking check for compilation progress using atomic flags only.
        /// Safe to call from any thread without risk of blocking.
        [[nodiscard]] bool isAnyCompilationInProgressNonBlocking() const noexcept;

        /// Request shutdown of all ongoing compilations.
        /// Sets the shutdown flag on all programs to abort compilation early.
        void requestShutdownAll();

        /// Wait for all ongoing compilations to finish.
        /// Should be called after requestShutdownAll() during application shutdown.
        void waitForAllCompilations();

        [[nodiscard]] ComputeContext & getComputeContext() const;

        void compileSlicerProgramBlocking();

        void logMsg(std::string msg) const;

        void recompileIfRequired();
        void recompileBlockingNoLock();
        
        /// Recompile the ManifoldDualContouring program with current model source
        void recompileBlockingForManifoldDC();
        [[nodiscard]] bool ensureSlicerProgramCompiled();
        void ensureHierarchicalDcProgramCompiled();
        void ensureManifoldDcProgramCompiled();

        void setComputeContext(std::shared_ptr<ComputeContext> context);

        [[nodiscard]] events::SharedLogger getSharedLogger() const;

        [[nodiscard]] CodeGenerator getCodeGenerator() const;
        void setCodeGenerator(CodeGenerator generator);

        /// Temporarily defer optimized render compilation while preview compilation continues.
        /// This allows file loading to publish the interactive command-stream preview before the
        /// slower optimized renderer starts compiling in the background.
        void setOptimizedRenderCompilationDeferred(bool deferred);
        [[nodiscard]] bool isOptimizedRenderCompilationDeferred() const;

        /// Temporarily defer slicer compilation while the render preview is published first.
        void setSlicerCompilationDeferred(bool deferred);
        [[nodiscard]] bool isSlicerCompilationDeferred() const;

        void setModelSource(std::string source);
        void setModelSources(std::string optimizedSource,
                 std::optional<std::string> previewSource,
                 bool compileOptimizedRenderProgram);

        void setVdbRequired(bool required);
        [[nodiscard]] bool isVdbSupported() const;
        [[nodiscard]] bool isVdbActive() const;
        [[nodiscard]] bool isVdbRequired() const;
        /// Returns the human-readable reason why NanoVDB is unavailable on this device.
        /// Empty when isVdbSupported() returns true.
        [[nodiscard]] std::string getVdbSupportFailureReason() const;

        /// Debug helpers for headless diagnostics
        [[nodiscard]] bool hasModelSource() const;
        [[nodiscard]] bool hasPreviewModelSource() const;
        [[nodiscard]] std::string getModelSource() const;
        [[nodiscard]] std::string getPreviewModelSource() const;
        [[nodiscard]] std::string getDebugStateSummary() const;

        ModelState const & getSlicerState();
        ModelState const & getRendererState();
        ModelState const & getPreviewRendererState();

        /// Get the parameter signature from the last successful compilation
        [[nodiscard]] ParameterSignature const & getCompiledParameterSignature() const;

        /// Set the parameter signature after successful compilation
        void setCompiledParameterSignature(ParameterSignature signature);

        /// Check if a given assembly's parameter structure matches the compiled signature
        [[nodiscard]] bool isParameterSignatureCompatible(nodes::Assembly const & assembly) const;

      private:
        void compileSlicerProgram();
        void compilePreviewRenderProgram();
        void compileRenderProgram();

        void throwIfNoOpenGL() const;
        [[nodiscard]] events::Logger & getLogger() const;

        [[nodiscard]] bool isOptimizedRenderProgramReadyLocked() const;

        [[nodiscard]] std::pair<RenderProgram *, RenderBackend>
        getBestRenderProgramAndBackendLocked() const;
        [[nodiscard]] RenderProgram * getCachedBestRenderProgram() const noexcept;
        void updateCachedBestRenderProgram(RenderProgram const * program,
                   RenderBackend backend) const;
        void invalidateCachedBestRenderProgram() const noexcept;

        void reinitIfNecssary();

        void updateVdbActivationLocked();
        void propagateVdbActivationLocked();

        mutable std::recursive_mutex m_computeMutex; // TODO: replace with std::mutex

        SharedComputeContext m_ComputeContext;
        SharedResources m_resources;

        std::unique_ptr<SlicerProgram> m_slicerProgram;

        std::unique_ptr<RenderProgram> m_optimizedRenderProgram;

        std::unique_ptr<RenderProgram> m_previewRenderProgram;

        std::unique_ptr<DualContouringSamplingProgram> m_dualContouringSamplingProgram;

        std::unique_ptr<HierarchicalDCProgram> m_hierarchicalDCProgram;

        std::unique_ptr<compute::ManifoldDualContouringProgram> m_manifoldDualContouringProgram;

        std::unique_ptr<MeshPreparationProgram> m_meshPreparationProgram;

        bool m_isComputationTimeLoggingEnabled = false;

        RequiredCapabilities m_capabilities = RequiredCapabilities::OpenGLInterop;
        events::SharedLogger m_eventLogger;

        ModelState m_renderState;

        ModelState m_previewRenderState;

        ModelState m_slicerState;
        CodeGenerator m_codeGenerator = CodeGenerator::Automatic;
        bool m_isVdbSupported = false;
        bool m_isVdbRequired = false;
        bool m_isVdbActive = false;
        std::string m_vdbSupportFailureReason;

        mutable std::mutex m_modelSourceMutex;
        std::string m_modelSource;
        std::string m_previewModelSource;
        bool m_hasPreviewModelSource = false;
        bool m_compileOptimizedRenderProgram = true;
        bool m_deferOptimizedRenderCompilation = false;
        bool m_deferSlicerCompilation = false;

        mutable std::atomic<RenderBackend> m_cachedSelectedRenderBackend{
          RenderBackend::Unavailable};
        mutable std::atomic<bool> m_cachedBestRenderProgramReady{false};

        /// Parameter signature from last successful compilation
        /// Used to detect when fast-path parameter updates are possible
        mutable std::mutex m_parameterSignatureMutex;
        ParameterSignature m_compiledParameterSignature;
    };
}
