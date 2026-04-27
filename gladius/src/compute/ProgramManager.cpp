#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <iostream>

#include <fmt/core.h>
#include <lodepng.h>

#include "CliReader.h"
#include "Contour.h"
#include "Mesh.h"
#include "Profiling.h"
#include "exceptions.h"
#include "ProgramManager.h"
#include "ComputeContext.h"
#include "RenderProgram.h"
#include "ResourceContext.h"
#include "SlicerProgram.h"
#include "compute/HierarchicalDCProgram.h"
#include "compute/MeshPreparationProgram.h"
#include "compute/ProgramManager.h"
#include "gpgpu.h"
#include "nodes/GraphFlattener.h"
#include "nodes/OptimizeOutputs.h"
#include <ToCommandStreamVisitor.h>
#include <ToOCLVisitor.h>

namespace gladius
{
    namespace
    {
        std::filesystem::path openClCacheDirectory()
        {
            std::error_code ec;
            std::filesystem::path base;

#ifdef _WIN32
            if (auto const * localAppData = std::getenv("LOCALAPPDATA"))
            {
                base = std::filesystem::path(localAppData) / "gladius";
            }
#else
            if (auto const * xdgCacheHome = std::getenv("XDG_CACHE_HOME"))
            {
                base = std::filesystem::path(xdgCacheHome) / "gladius";
            }
            else if (auto const * home = std::getenv("HOME"))
            {
                base = std::filesystem::path(home) / ".cache" / "gladius";
            }
#endif
            if (base.empty())
            {
                base = std::filesystem::temp_directory_path(ec) / "gladius";
            }

            auto cacheDir = base / "opencl_cache";
            std::filesystem::create_directories(cacheDir, ec);
            if (ec)
            {
                ec.clear();
                cacheDir = std::filesystem::temp_directory_path(ec) / "gladius" / "opencl_cache";
                std::filesystem::create_directories(cacheDir, ec);
            }
            return cacheDir;
        }
    } // namespace

    ProgramManager::ProgramManager(SharedComputeContext context,
                                   RequiredCapabilities requiredCapabilities,
                                   events::SharedLogger logger,
                                   SharedResources resources)
        : m_ComputeContext(context)
        , m_resources(resources)
        , m_capabilities(requiredCapabilities)
        , m_eventLogger(logger)

    {
        init();
    }

    void ProgramManager::init()
    {
        ProfileFunction;

        std::lock_guard<std::recursive_mutex> lock(m_computeMutex);
        m_slicerProgram = std::make_unique<SlicerProgram>(m_ComputeContext, m_resources);
        m_optimizedRenderProgram = std::make_unique<RenderProgram>(m_ComputeContext, m_resources);
        m_dualContouringSamplingProgram =
          std::make_unique<DualContouringSamplingProgram>(m_ComputeContext, m_resources);
        m_hierarchicalDCProgram =
          std::make_unique<HierarchicalDCProgram>(m_ComputeContext, m_resources);
        m_manifoldDualContouringProgram =
          std::make_unique<compute::ManifoldDualContouringProgram>(m_ComputeContext, m_resources);
        m_meshPreparationProgram =
          std::make_unique<MeshPreparationProgram>(m_ComputeContext, m_resources);

        // NanoVDB is now enabled for all OpenCL runtimes, including rusticl
        // fp64 hardware support is not strictly required (OpenCL can emulate doubles)
        m_isVdbSupported = true;
        m_vdbSupportFailureReason.clear();

        updateVdbActivationLocked();

        // m_slicerProgram->setEnableVdb(m_enableVdb);
        // m_optimizedRenderProgram->setEnableVdb(m_enableVdb);
        // m_dualContouringSamplingProgram->setEnableVdb(m_enableVdb);
        // m_hierarchicalDCProgram->setEnableVdb(m_enableVdb);

        // Propagate logger to programs so that CL diagnostics go to the event logger
        if (m_eventLogger)
        {
            m_slicerProgram->setLogger(m_eventLogger);
            m_optimizedRenderProgram->setLogger(m_eventLogger);
            m_dualContouringSamplingProgram->setLogger(m_eventLogger);
            m_hierarchicalDCProgram->setLogger(m_eventLogger);
            m_manifoldDualContouringProgram->setLogger(m_eventLogger);
            m_meshPreparationProgram->setLogger(m_eventLogger);
        }

        // Set up binary caching
        auto cacheDir = openClCacheDirectory();
        m_slicerProgram->setDebugLabel("SlicerProgram");
        m_optimizedRenderProgram->setDebugLabel("RenderProgram");
        m_dualContouringSamplingProgram->setDebugLabel("DualContouringSamplingProgram");
        m_hierarchicalDCProgram->setDebugLabel("HierarchicalDCProgram");
        m_manifoldDualContouringProgram->setDebugLabel("ManifoldDualContouringProgram");
        m_meshPreparationProgram->setDebugLabel("MeshPreparationProgram");
        m_slicerProgram->setCacheDirectory(cacheDir);
        m_optimizedRenderProgram->setCacheDirectory(cacheDir);
        m_dualContouringSamplingProgram->setCacheDirectory(cacheDir);
        m_hierarchicalDCProgram->setCacheDirectory(cacheDir);
        m_manifoldDualContouringProgram->setCacheDirectory(cacheDir);
        m_meshPreparationProgram->setCacheDirectory(cacheDir);
        m_slicerProgram->setEnableTwoLevelPipeline(true);
        m_optimizedRenderProgram->setEnableTwoLevelPipeline(true);

        m_optimizedRenderProgram->buildKernelLib();
        recompileIfRequired();
        LOG_LOCATION
    }

    void ProgramManager::reset()
    {
        ProfileFunction std::lock_guard<std::recursive_mutex> lock(m_computeMutex);
        m_renderState.signalCompilationRequired();
        m_slicerState.signalCompilationRequired();
        m_isVdbRequired = false;
        updateVdbActivationLocked();
    }

    ComputeToken ProgramManager::waitForComputeToken()
    {
        return ComputeToken(m_computeMutex);
    }

    OptionalComputeToken ProgramManager::requestComputeToken()
    {
        if (!m_computeMutex.try_lock())
        {
            return {};
        }
        std::lock_guard<std::recursive_mutex> lock(m_computeMutex, std::adopt_lock);
        return OptionalComputeToken(m_computeMutex);
    }

    void ProgramManager::compileSlicerProgram()
    {
        ProfileFunction std::lock_guard<std::mutex> lock(m_modelSourceMutex);
        std::lock_guard<std::recursive_mutex> lockCompute(m_computeMutex);

        if (!m_slicerProgram->isCompilationInProgress())
        {
            m_slicerProgram->setEnableVdb(m_isVdbActive);
            m_slicerProgram->setModelKernel(m_modelSource);
            if (m_dualContouringSamplingProgram)
            {
                m_dualContouringSamplingProgram->setModelKernel(m_modelSource);
            }
            m_slicerProgram->recompileNonBlocking();
            m_slicerState.signalCompilationStarted();
        }
    }

    void ProgramManager::compileRenderProgram()
    {
        ProfileFunction std::lock_guard<std::mutex> lock(m_modelSourceMutex);
        std::lock_guard<std::recursive_mutex> lockCompute(m_computeMutex);
        LOG_LOCATION
        if (!m_optimizedRenderProgram->isCompilationInProgress())
        {
            LOG_LOCATION
            m_optimizedRenderProgram->setEnableVdb(m_isVdbActive);
            m_optimizedRenderProgram->setModelKernel(m_modelSource);
            if (m_dualContouringSamplingProgram)
            {
                m_dualContouringSamplingProgram->setModelKernel(m_modelSource);
            }
            m_optimizedRenderProgram->recompileNonBlocking();
            m_renderState.signalCompilationStarted();
        }
    }

    void ProgramManager::recompileIfRequired()
    {
        ProfileFunction;
        LOG_LOCATION
        if (!m_optimizedRenderProgram->isCompilationInProgress())
        {
            m_renderState.signalCompilationFinished();
        }

        if (!m_slicerProgram->isCompilationInProgress())
        {
            m_slicerState.signalCompilationFinished();
        }

        if (!m_renderState.isCompilationRequired())
        {
            return;
        }

        logMsg("starting compilation of optimized program");
        compileRenderProgram();

        compileSlicerProgram();
    }

    void ProgramManager::recompileBlockingForManifoldDC()
    {
        ProfileFunction;
        ensureManifoldDcProgramCompiled();
    }

    void ProgramManager::ensureHierarchicalDcProgramCompiled()
    {
        ProfileFunction std::lock_guard<std::recursive_mutex> lock(m_computeMutex);
        std::lock_guard<std::mutex> lockModel(m_modelSourceMutex);

        if (m_modelSource.empty() || !m_hierarchicalDCProgram)
        {
            return;
        }

        m_hierarchicalDCProgram->setEnableVdb(m_isVdbActive);
        if (!m_hierarchicalDCProgram->isValid() || m_hierarchicalDCProgram->isCompilationInProgress())
        {
            m_hierarchicalDCProgram->setModelKernel(m_modelSource);
            m_hierarchicalDCProgram->waitForCompilation();
            m_hierarchicalDCProgram->recompileBlocking();
        }
        else
        {
            m_hierarchicalDCProgram->setModelKernel(m_modelSource);
            if (!m_hierarchicalDCProgram->isValid())
            {
                m_hierarchicalDCProgram->recompileBlocking();
            }
        }
    }

    void ProgramManager::ensureManifoldDcProgramCompiled()
    {
        ProfileFunction std::lock_guard<std::recursive_mutex> lock(m_computeMutex);
        std::lock_guard<std::mutex> lockModel(m_modelSourceMutex);
        
        if (m_modelSource.empty() || !m_manifoldDualContouringProgram)
        {
            return;
        }

        m_manifoldDualContouringProgram->setEnableVdb(m_isVdbActive);
        m_manifoldDualContouringProgram->setModelKernel(m_modelSource);
        m_manifoldDualContouringProgram->waitForCompilation();
        if (!m_manifoldDualContouringProgram->isValid())
        {
            m_manifoldDualContouringProgram->recompileBlocking();
        }
    }

    void ProgramManager::recompileBlockingNoLock()
    {
        ProfileFunction std::lock_guard<std::recursive_mutex> lock(m_computeMutex);
        propagateVdbActivationLocked();

        m_optimizedRenderProgram->setModelKernel(m_modelSource);
        m_slicerProgram->setModelKernel(m_modelSource);
        if (m_dualContouringSamplingProgram)
        {
            m_dualContouringSamplingProgram->setModelKernel(m_modelSource);
        }
        if (m_hierarchicalDCProgram)
        {
            m_hierarchicalDCProgram->setModelKernel(m_modelSource);
            m_hierarchicalDCProgram->recompileBlocking();
        }
        if (m_manifoldDualContouringProgram)
        {
            m_manifoldDualContouringProgram->setModelKernel(m_modelSource);
            m_manifoldDualContouringProgram->recompileBlocking();
        }

        m_optimizedRenderProgram->recompileBlocking();
        m_slicerProgram->recompileBlocking();
        m_renderState.signalCompilationFinished();

        m_slicerState.signalCompilationFinished();
    }

    void ProgramManager::setComputeContext(std::shared_ptr<ComputeContext> context)
    {
        ProfileFunction std::lock_guard<std::recursive_mutex> lockCompute(m_computeMutex);

        m_ComputeContext = std::move(context);
        reset();
        init();
    }

    void ProgramManager::throwIfNoOpenGL() const
    {
        if (m_capabilities == RequiredCapabilities::ComputeOnly)
        {
            throw std::runtime_error("Operation requires OpenGL which is not available");
        }
    }

    bool ProgramManager::isVdbRequired() const
    {
        ProfileFunction std::lock_guard<std::recursive_mutex> lock(m_computeMutex);
        return m_isVdbRequired;
    }

    [[nodiscard]] bool ProgramManager::isAnyCompilationInProgress() const
    {
         ProfileFunction;
         std::lock_guard<std::recursive_mutex> lock(m_computeMutex);

         return (m_optimizedRenderProgram && m_optimizedRenderProgram->isCompilationInProgress()) ||
             (m_slicerProgram && m_slicerProgram->isCompilationInProgress()) ||
             (m_dualContouringSamplingProgram && m_dualContouringSamplingProgram->isCompilationInProgress()) ||
             (m_hierarchicalDCProgram && m_hierarchicalDCProgram->isCompilationInProgress()) ||
             (m_manifoldDualContouringProgram && m_manifoldDualContouringProgram->isCompilationInProgress()) ||
             (m_meshPreparationProgram && m_meshPreparationProgram->isCompilationInProgress());
    }

    [[nodiscard]] bool ProgramManager::isAnyCompilationInProgressNonBlocking() const noexcept
    {
        // SAFETY: Program pointers are set during initialization and never nullified
        // during normal operation, making this lock-free check safe.
        // Each isCompilationInProgress() returns an atomic<bool>.
        return (m_optimizedRenderProgram && m_optimizedRenderProgram->isCompilationInProgress()) ||
            (m_slicerProgram && m_slicerProgram->isCompilationInProgress()) ||
            (m_dualContouringSamplingProgram && m_dualContouringSamplingProgram->isCompilationInProgress()) ||
            (m_hierarchicalDCProgram && m_hierarchicalDCProgram->isCompilationInProgress()) ||
            (m_manifoldDualContouringProgram && m_manifoldDualContouringProgram->isCompilationInProgress()) ||
            (m_meshPreparationProgram && m_meshPreparationProgram->isCompilationInProgress());
    }

    void ProgramManager::requestShutdownAll()
    {
        // Request shutdown on all programs (no locking needed - just sets atomic flags)
        if (m_optimizedRenderProgram)
        {
            m_optimizedRenderProgram->requestShutdown();
        }
        if (m_slicerProgram)
        {
            m_slicerProgram->requestShutdown();
        }
        if (m_dualContouringSamplingProgram)
        {
            m_dualContouringSamplingProgram->requestShutdown();
        }
        if (m_hierarchicalDCProgram)
        {
            m_hierarchicalDCProgram->requestShutdown();
        }
        if (m_manifoldDualContouringProgram)
        {
            m_manifoldDualContouringProgram->requestShutdown();
        }
        if (m_meshPreparationProgram)
        {
            m_meshPreparationProgram->requestShutdown();
        }
    }

    void ProgramManager::waitForAllCompilations()
    {
        // Wait for all compilations to complete
        // Note: Lock is needed to safely access program pointers
        std::lock_guard<std::recursive_mutex> lock(m_computeMutex);

        if (m_optimizedRenderProgram)
        {
            m_optimizedRenderProgram->waitForCompilation();
        }
        if (m_slicerProgram)
        {
            m_slicerProgram->waitForCompilation();
        }
        if (m_dualContouringSamplingProgram)
        {
            m_dualContouringSamplingProgram->waitForCompilation();
        }
        if (m_hierarchicalDCProgram)
        {
            m_hierarchicalDCProgram->waitForCompilation();
        }
        if (m_manifoldDualContouringProgram)
        {
            m_manifoldDualContouringProgram->waitForCompilation();
        }
        if (m_meshPreparationProgram)
        {
            m_meshPreparationProgram->waitForCompilation();
        }
    }

    ComputeContext & ProgramManager::getComputeContext() const
    {
        return *m_ComputeContext;
    }

    void ProgramManager::compileSlicerProgramBlocking()
    {
        ProfileFunction std::lock_guard<std::mutex> lock(m_modelSourceMutex);
        std::lock_guard<std::recursive_mutex> lockCompute(m_computeMutex);
        m_slicerState.signalCompilationStarted();
        m_slicerProgram->setEnableVdb(m_isVdbActive);
        m_slicerProgram->waitForCompilation();
        m_slicerProgram->recompileNonBlocking();
        m_slicerProgram->waitForCompilation();
        m_slicerState.signalCompilationFinished();
    }

    void ProgramManager::logMsg(std::string msg) const
    {
        if (m_eventLogger)
        {
            getLogger().addEvent({msg, events::Severity::Info});
        }
        // If no logger, remain silent to avoid console noise
    }

    events::Logger & ProgramManager::getLogger() const
    {
        if (!m_eventLogger)
        {
            throw std::runtime_error("logger is missing");
        }
        return *m_eventLogger;
    }

    void ProgramManager::setVdbRequired(bool required)
    {
        ProfileFunction std::lock_guard<std::recursive_mutex> lock(m_computeMutex);

        if (required && !m_isVdbSupported)
        {
            auto const detail = m_vdbSupportFailureReason.empty()
                                   ? std::string{}
                                   : fmt::format(" ({})", m_vdbSupportFailureReason);
            auto message =
              fmt::format("This model requires NanoVDB, but the active OpenCL device cannot provide it{}.",
                          detail);

            if (m_eventLogger)
            {
                getLogger().addEvent({message, events::Severity::Error});
            }
            else
            {
                std::cerr << message << '\n';
            }

            throw GladiusException(std::move(message));
        }

        if (m_isVdbRequired == required)
        {
            return;
        }

        m_isVdbRequired = required;
        updateVdbActivationLocked();
    }

    bool ProgramManager::isVdbSupported() const
    {
        ProfileFunction std::lock_guard<std::recursive_mutex> lock(m_computeMutex);
        return m_isVdbSupported;
    }

    bool ProgramManager::isVdbActive() const
    {
        ProfileFunction std::lock_guard<std::recursive_mutex> lock(m_computeMutex);
        return m_isVdbActive;
    }

    void ProgramManager::updateVdbActivationLocked()
    {
        bool const newActive = m_isVdbSupported && m_isVdbRequired;
        bool const stateChanged = (newActive != m_isVdbActive);
        m_isVdbActive = newActive;
        propagateVdbActivationLocked();

        if (stateChanged && m_eventLogger)
        {
            auto const stateMessage =
              fmt::format("NanoVDB {}", m_isVdbActive ? "enabled" : "disabled");
            getLogger().addEvent({stateMessage, events::Severity::Info});
        }
    }

    void ProgramManager::propagateVdbActivationLocked()
    {
        auto const applyState = [this](auto & programPtr)
        {
            if (programPtr)
            {
                programPtr->setEnableVdb(m_isVdbActive);
            }
        };

        applyState(m_slicerProgram);
        applyState(m_optimizedRenderProgram);
        applyState(m_dualContouringSamplingProgram);
        applyState(m_hierarchicalDCProgram);
        applyState(m_manifoldDualContouringProgram);
    }

    void ProgramManager::reinitIfNecssary()
    {
        ProfileFunction

          std::lock_guard<std::recursive_mutex>
            lock(m_computeMutex);
        if (m_ComputeContext->isValid())
        {
            return;
        }
        m_eventLogger->addEvent({"Reinitializing compute context"});

        reset();
        init();
    }

    SlicerProgram * ProgramManager::getSlicerProgram() const
    {
        return m_slicerProgram.get();
    }

    RenderProgram * ProgramManager::getRenderProgram() const
    {
        return m_optimizedRenderProgram.get();
    }

    DualContouringSamplingProgram * ProgramManager::getDualContouringSamplingProgram() const
    {
        return m_dualContouringSamplingProgram.get();
    }

    HierarchicalDCProgram * ProgramManager::getHierarchicalDCProgram() const
    {
        return m_hierarchicalDCProgram.get();
    }

    compute::ManifoldDualContouringProgram * ProgramManager::getManifoldDualContouringProgram() const
    {
        return m_manifoldDualContouringProgram.get();
    }

    MeshPreparationProgram * ProgramManager::getMeshPreparationProgram() const
    {
        return m_meshPreparationProgram.get();
    }

    events::SharedLogger ProgramManager::getSharedLogger() const
    {
        return m_eventLogger;
    }

    CodeGenerator ProgramManager::getCodeGenerator() const
    {
        return m_codeGenerator;
    }

    void ProgramManager::setCodeGenerator(CodeGenerator generator)
    {
        m_codeGenerator = generator;
    }

    void ProgramManager::setModelSource(std::string source)
    {
        std::lock_guard<std::mutex> lock(m_modelSourceMutex);
        m_modelSource = std::move(source);
        if (m_eventLogger)
        {
            getLogger().addEvent(
              {fmt::format("ProgramManager.setModelSource: size={} bytes", m_modelSource.size()),
               events::Severity::Info});
        }
        m_slicerState.signalCompilationRequired();
        m_renderState.signalCompilationRequired();
    }

    bool ProgramManager::hasModelSource() const
    {
        std::lock_guard<std::mutex> lock(m_modelSourceMutex);
        return !m_modelSource.empty();
    }

    std::string ProgramManager::getModelSource() const
    {
        std::lock_guard<std::mutex> lock(m_modelSourceMutex);
        return m_modelSource;
    }

    std::string ProgramManager::getDebugStateSummary() const
    {
          std::lock_guard<std::mutex> lock(m_modelSourceMutex);
          std::lock_guard<std::recursive_mutex> lockCompute(m_computeMutex);
        std::stringstream ss;
        ss << "ProgramManager: modelSource="
           << (m_modelSource.empty() ? 0 : (int) m_modelSource.size())
           << "B renderUpToDate=" << (m_renderState.isModelUpToDate() ? 1 : 0)
           << " slicerUpToDate=" << (m_slicerState.isModelUpToDate() ? 1 : 0)
           << " renderCompiling=" << (m_optimizedRenderProgram->isCompilationInProgress() ? 1 : 0)
              << " slicerCompiling=" << (m_slicerProgram->isCompilationInProgress() ? 1 : 0)
              << " vdbSupported=" << (m_isVdbSupported ? 1 : 0)
              << " vdbRequired=" << (m_isVdbRequired ? 1 : 0)
              << " vdbActive=" << (m_isVdbActive ? 1 : 0);
        return ss.str();
    }

    ModelState const & ProgramManager::getSlicerState()
    {
        return m_slicerState;
    }

    ModelState const & ProgramManager::getRendererState()
    {
        return m_renderState;
    }

    ParameterSignature const & ProgramManager::getCompiledParameterSignature() const
    {
        std::lock_guard<std::mutex> lock(m_parameterSignatureMutex);
        return m_compiledParameterSignature;
    }

    void ProgramManager::setCompiledParameterSignature(ParameterSignature signature)
    {
        std::lock_guard<std::mutex> lock(m_parameterSignatureMutex);
        m_compiledParameterSignature = std::move(signature);
    }

    bool ProgramManager::isParameterSignatureCompatible(nodes::Assembly const & assembly) const
    {
        std::lock_guard<std::mutex> lock(m_parameterSignatureMutex);

        // If no signature has been compiled yet, not compatible (need initial compilation)
        if (!m_compiledParameterSignature.isValid())
        {
            return false;
        }

        // Compute current signature and compare
        auto const currentSignature = ParameterSignature::compute(assembly);
        return currentSignature.matches(m_compiledParameterSignature);
    }
}
