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

#include <mutex>
#include <string>

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

        [[nodiscard]] RenderProgram * getRenderProgram() const;

        [[nodiscard]] DualContouringSamplingProgram * getDualContouringSamplingProgram() const;

        [[nodiscard]] HierarchicalDCProgram * getHierarchicalDCProgram() const;

        [[nodiscard]] compute::ManifoldDualContouringProgram * getManifoldDualContouringProgram() const;

        [[nodiscard]] MeshPreparationProgram * getMeshPreparationProgram() const;

        [[nodiscard]] bool isAnyCompilationInProgress() const;

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

        void setModelSource(std::string source);

        void setVdbRequired(bool required);
        [[nodiscard]] bool isVdbSupported() const;
        [[nodiscard]] bool isVdbActive() const;
        [[nodiscard]] bool isVdbRequired() const;

        /// Debug helpers for headless diagnostics
        [[nodiscard]] bool hasModelSource() const;
        [[nodiscard]] std::string getModelSource() const;
        [[nodiscard]] std::string getDebugStateSummary() const;

        ModelState const & getSlicerState();
        ModelState const & getRendererState();

        /// Get the parameter signature from the last successful compilation
        [[nodiscard]] ParameterSignature const & getCompiledParameterSignature() const;

        /// Set the parameter signature after successful compilation
        void setCompiledParameterSignature(ParameterSignature signature);

        /// Check if a given assembly's parameter structure matches the compiled signature
        [[nodiscard]] bool isParameterSignatureCompatible(nodes::Assembly const & assembly) const;

      private:
        void compileSlicerProgram();
        void compileRenderProgram();

        void throwIfNoOpenGL() const;
        [[nodiscard]] events::Logger & getLogger() const;

        void reinitIfNecssary();

        void updateVdbActivationLocked();
        void propagateVdbActivationLocked();

        mutable std::recursive_mutex m_computeMutex; // TODO: replace with std::mutex

        SharedComputeContext m_ComputeContext;
        SharedResources m_resources;

        std::unique_ptr<SlicerProgram> m_slicerProgram;

        std::unique_ptr<RenderProgram> m_optimizedRenderProgram;

        std::unique_ptr<DualContouringSamplingProgram> m_dualContouringSamplingProgram;

        std::unique_ptr<HierarchicalDCProgram> m_hierarchicalDCProgram;

        std::unique_ptr<compute::ManifoldDualContouringProgram> m_manifoldDualContouringProgram;

        std::unique_ptr<MeshPreparationProgram> m_meshPreparationProgram;

        bool m_isComputationTimeLoggingEnabled = false;

        RequiredCapabilities m_capabilities = RequiredCapabilities::OpenGLInterop;
        events::SharedLogger m_eventLogger;

        ModelState m_renderState;

        ModelState m_slicerState;
        CodeGenerator m_codeGenerator = CodeGenerator::Code;
        bool m_isVdbSupported = false;
        bool m_isVdbRequired = false;
        bool m_isVdbActive = false;
        std::string m_vdbSupportFailureReason;

        mutable std::mutex m_modelSourceMutex;
        std::string m_modelSource;

        /// Parameter signature from last successful compilation
        /// Used to detect when fast-path parameter updates are possible
        mutable std::mutex m_parameterSignatureMutex;
        ParameterSignature m_compiledParameterSignature;
    };
}
