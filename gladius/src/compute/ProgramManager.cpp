#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
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
#include "RenderProgram.h"
#include "ResourceContext.h"
#include "SlicerProgram.h"
#include "compute/HierarchicalDCProgram.h"
#include "compute/ProgramManager.h"
#include "gpgpu.h"
#include "nodes/GraphFlattener.h"
#include "nodes/OptimizeOutputs.h"
#include <ToCommandStreamVisitor.h>
#include <ToOCLVisitor.h>

namespace gladius
{
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

        bool const hasFp64 = m_ComputeContext && m_ComputeContext->supportsFp64();
        bool const isRusticl = m_ComputeContext && m_ComputeContext->getCapabilities().rusticl;

        m_isVdbSupported = hasFp64 && !isRusticl;
        m_vdbSupportFailureReason.clear();

        if (!hasFp64)
        {
            m_isVdbSupported = false;
            m_vdbSupportFailureReason = "OpenCL device lacks fp64 support";
            if (m_eventLogger)
            {
                m_eventLogger->logWarning(
                  "OpenCL device lacks fp64 support; NanoVDB features will be disabled.");
            }
            else
            {
                std::cerr << "OpenCL device lacks fp64 support; NanoVDB features will be disabled.\n";
            }
        }
        else if (isRusticl)
        {
            m_isVdbSupported = false;
            m_vdbSupportFailureReason = "NanoVDB is not supported on the rusticl OpenCL runtime";
            if (m_eventLogger)
            {
                m_eventLogger->logWarning(
                  "NanoVDB is currently disabled for rusticl OpenCL runtimes.");
            }
            else
            {
                std::cerr << "NanoVDB is currently disabled for rusticl OpenCL runtimes.\n";
            }
        }
        else if (m_eventLogger)
        {
            m_eventLogger->logInfo("NanoVDB support enabled for active OpenCL device.");
        }

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
        }

        // Set up binary caching
        auto cacheDir = std::filesystem::temp_directory_path() / "gladius" / "opencl_cache";
        m_slicerProgram->setCacheDirectory(cacheDir);
        m_optimizedRenderProgram->setCacheDirectory(cacheDir);
        m_dualContouringSamplingProgram->setCacheDirectory(cacheDir);
        m_hierarchicalDCProgram->setCacheDirectory(cacheDir);

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

    void ProgramManager::recompileBlockingNoLock()
    {
        ProfileFunction std::lock_guard<std::recursive_mutex> lock(m_computeMutex);
        propagateVdbActivationLocked();

        m_optimizedRenderProgram->setModelKernel(m_modelSource);
        m_slicerProgram->setModelKernel(m_modelSource);

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
        return m_optimizedRenderProgram->isCompilationInProgress() ||
               m_slicerProgram->isCompilationInProgress() ||
               (m_hierarchicalDCProgram && m_hierarchicalDCProgram->isCompilationInProgress());
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
