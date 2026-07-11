#include "Document.h"

#include "BackupManager.h"
#include "CliReader.h"
#include "CliWriter.h"
#include "FileChooser.h"
#include "FileSystemUtils.h"
#include "MeshBVH.h"
#include "MeshExporter.h"
#include "Profiling.h"
#include "ResourceManager.h"
#include "SpatialMeshResource.h"
#include "TimeMeasurement.h"
#include "compute/ComputeCore.h"
#include "exceptions.h"
#include "imguinodeeditor.h"
#include "io/3mf/BeamLatticeExporter.h"
#include "io/3mf/ImageExtractor.h"
#include "io/3mf/ImageStackCreator.h"
#include "io/3mf/LibraryMetadata.h"
#include "io/3mf/ResourceDependencyGraph.h"
#include "io/3mf/ResourceIdUtil.h" // for resourceIdToUniqueResourceId
#include "io/3mf/Writer3mf.h"
#include "io/DualContouringStlExporter.h"
#include "io/ImporterVdb.h"
#include "io/ManifoldDualContouringStlExporter.h"
#include "io/VdbImporter.h"
#include "nodes/GraphFlattener.h"
#include "nodes/LowerFunctionGradient.h"
#include "nodes/LowerNormalizeDistanceField.h"
#include "nodes/Model.h"
#include "nodes/OptimizeOutputs.h"
#include "nodes/ToCommandStreamVisitor.h"
#include "nodes/ToOCLVisitor.h"
#include "nodes/Validator.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fmt/format.h>
#include <iostream>
#include <tracy/Tracy.hpp>
#include <utility>

#include <cmrc/cmrc.hpp>

CMRC_DECLARE(gladius_resources);

namespace gladius
{
    namespace
    {
        constexpr double kNanoVdbBudgetUtilization = 0.5;

        [[nodiscard]] NanoVdbBuildPolicy
        makeNanoVdbBuildPolicy(SharedComputeContext const & computeContext,
                               NanoVdbFailurePolicy const failurePolicy) noexcept
        {
            NanoVdbBuildPolicy policy{};
            policy.failurePolicy = failurePolicy;

            if (!computeContext)
            {
                return policy;
            }

            size_t const deviceGlobalMemBytes = computeContext->getDeviceGlobalMemBytes();
            size_t const deviceMaxAllocBytes = computeContext->getDeviceMaxAllocBytes();

            if (deviceGlobalMemBytes == 0u && deviceMaxAllocBytes == 0u)
            {
                return policy;
            }

            size_t budgetFromGlobal = 0u;
            if (deviceGlobalMemBytes != 0u)
            {
                budgetFromGlobal = static_cast<size_t>(
                  static_cast<double>(deviceGlobalMemBytes) * kNanoVdbBudgetUtilization);
            }

            if (deviceMaxAllocBytes == 0u)
            {
                policy.budgetBytes = budgetFromGlobal;
                return policy;
            }

            if (budgetFromGlobal == 0u)
            {
                policy.budgetBytes = deviceMaxAllocBytes;
                return policy;
            }

            policy.budgetBytes = std::min(deviceMaxAllocBytes, budgetFromGlobal);
            return policy;
        }

        [[nodiscard]] MeshSdfEvaluationConfig makeInteractivePreviewMeshSdfConfig(
          MeshSdfEvaluationConfig cfg) noexcept
        {
            cfg.method = MeshSdfMethod::PureBVH;
            cfg.fwnUseSignCache = false;
            return cfg;
        }

        [[nodiscard]] bool needsMeshSdfResourceUpdate(SpatialMeshResource const & resource,
                                                      MeshSdfEvaluationConfig const & cfg) noexcept
        {
            auto const & current = resource.evaluationConfig();
            return requiresMeshRebuild(current, cfg) ||
                   current.fwnUseSignCache != cfg.fwnUseSignCache ||
                   (current.method == MeshSdfMethod::FastWindingNumber &&
                    cfg.method == MeshSdfMethod::FastWindingNumber &&
                    current.fwnBeta != cfg.fwnBeta);
        }

        class ScopedNanoVdbFailurePolicy
        {
          public:
            ScopedNanoVdbFailurePolicy(std::atomic<NanoVdbFailurePolicy> & policy,
                                       NanoVdbFailurePolicy const next) noexcept
                : m_policy(policy)
                , m_previous(policy.exchange(next, std::memory_order_relaxed))
            {
            }

            ~ScopedNanoVdbFailurePolicy() noexcept
            {
                m_policy.store(m_previous, std::memory_order_relaxed);
            }

          private:
            std::atomic<NanoVdbFailurePolicy> & m_policy;
            NanoVdbFailurePolicy m_previous;
        };

        class ScopedLoadingFlag
        {
          public:
            explicit ScopedLoadingFlag(std::atomic<bool> & loading) noexcept
                : m_loading(loading)
            {
                m_loading.store(true, std::memory_order_relaxed);
            }

            ~ScopedLoadingFlag() noexcept
            {
                m_loading.store(false, std::memory_order_relaxed);
            }

          private:
            std::atomic<bool> & m_loading;
        };

        class ScopedOptimizedRenderCompilationDeferral
        {
          public:
            ScopedOptimizedRenderCompilationDeferral(ComputeCore & core, bool const active)
                : m_core(&core)
                , m_active(active)
                , m_previousOptimizedDeferred(active ? core.isOptimizedRenderCompilationDeferred()
                                                     : false)
                , m_previousSlicerDeferred(active ? core.isSlicerCompilationDeferred() : false)
            {
                if (m_active)
                {
                    m_core->setOptimizedRenderCompilationDeferred(true);
                    m_core->setSlicerCompilationDeferred(true);
                }
            }

            ~ScopedOptimizedRenderCompilationDeferral()
            {
                (void) restore();
            }

            /// Restore the previous settings and report whether any deferred compilation
            /// is allowed afterwards.
            [[nodiscard]] bool restore()
            {
                if (!m_active || m_restored || m_core == nullptr)
                {
                    return false;
                }

                m_core->setOptimizedRenderCompilationDeferred(m_previousOptimizedDeferred);
                m_core->setSlicerCompilationDeferred(m_previousSlicerDeferred);
                m_restored = true;
                return !m_previousOptimizedDeferred || !m_previousSlicerDeferred;
            }

          private:
            ComputeCore * m_core = nullptr;
            bool m_active = false;
            bool m_previousOptimizedDeferred = false;
            bool m_previousSlicerDeferred = false;
            bool m_restored = false;
        };

        class ScopedMeshSdfEvaluationConfigOverride
        {
          public:
            ScopedMeshSdfEvaluationConfigOverride(Document & document,
                                                  MeshSdfEvaluationConfig const & cfg,
                                                  bool const active)
                : m_document(&document)
                , m_active(active)
                , m_previousConfig(active ? document.getMeshSdfEvaluationConfig()
                                          : MeshSdfEvaluationConfig{})
            {
                if (m_active)
                {
                    m_document->setMeshSdfEvaluationConfig(cfg);
                }
            }

            ~ScopedMeshSdfEvaluationConfigOverride()
            {
                restore();
            }

            void restore()
            {
                if (!m_active || m_restored || m_document == nullptr)
                {
                    return;
                }
                m_document->setMeshSdfEvaluationConfig(m_previousConfig);
                m_restored = true;
            }

          private:
            Document * m_document = nullptr;
            bool m_active = false;
            MeshSdfEvaluationConfig m_previousConfig{};
            bool m_restored = false;
        };
    }

    using namespace std;

    AssemblyToken Document::waitForAssemblyToken() const
    {
        return AssemblyToken(m_assemblyMutex);
    }

    OptionalAssemblyToken Document::requestAssemblyToken() const
    {
        if (!m_assemblyMutex.try_lock())
        {
            return {};
        }
        std::lock_guard<std::mutex> lock(m_assemblyMutex, std::adopt_lock);
        return OptionalAssemblyToken(m_assemblyMutex);
    }

    void Document::resetGeneratorContext()
    {

        if (!m_assembly || !m_core)
        {
            throw std::runtime_error("No assembly or core");
        }
        // Get the resource context directly as a shared pointer
        auto resourceContextPtr = m_core->getResourceContext();
        m_generatorContext = std::make_unique<nodes::GeneratorContext>(
          resourceContextPtr, m_assembly->getFilename().remove_filename());
        m_primitiveDateNeedsUpdate = true;
    }

    Document::Document(std::shared_ptr<ComputeCore> core)
        : m_core(std::move(core))
    {
        m_channels.push_back(
          BitmapChannel{"DownSkin",
                        [&](float z_mm, Vector2 pixelSize_mm)
                        { return m_core->generateDownSkinMap(z_mm, std::move(pixelSize_mm)); }});

        m_channels.push_back(
          BitmapChannel{"UpSkin",
                        [&](float z_mm, Vector2 pixelSize_mm)
                        { return m_core->generateUpSkinMap(z_mm, std::move(pixelSize_mm)); }});

        newModel();
        resetGeneratorContext();

        // Initialize backup manager
        m_backupManager.initialize();
    }

    Document::~Document()
    {
        // Join in-flight async workers before any member is destroyed. The workers
        // (refreshWorker / file-load) capture `this` and touch members such as
        // m_isLoading, m_loadingError and m_buildItems that are declared after the
        // future members. Relying on the implicit std::future destructors to join
        // would do so only after those later-declared members are already gone,
        // producing a use-after-free during shutdown.
        try
        {
            if (m_futureModelRefresh.valid())
            {
                m_futureModelRefresh.wait();
            }
        }
        catch (...)
        {
            // Never throw from a destructor.
        }

        try
        {
            if (m_futureFileLoad.valid())
            {
                m_futureFileLoad.wait();
            }
        }
        catch (...)
        {
            // Never throw from a destructor.
        }
    }

    bool Document::refreshModelAsync()
    {
        if (!m_assembly || !m_core)
        {
            return false;
        }

        // Run a lightweight graph sync + validation pass before scheduling any
        // background refresh work. This keeps invalid graphs from entering the
        // expensive payload/compile path and avoids transient "compiling"
        // states while the user is still resolving graph issues.
        if (!prepareAssemblyForRefresh())
        {
            return false;
        }

        // Signal compilation started on the UI thread BEFORE launching the worker.
        // This ensures isRendererReady() returns false immediately, preventing
        // render() from rendering a frame with stale state (flicker).
        // The worker also calls signalCompilationStarted() after acquiring the
        // compute token — that call is redundant but harmless.
        m_core->getMeshResourceState()->signalCompilationStarted();

        // saveBackup();
        {
            m_futureModelRefresh = std::async(std::launch::async,
                                              [&]()
                                              {
                                                  try
                                                  {
                                                      refreshWorker();
                                                  }
                                                  catch (std::exception const & e)
                                                  {
                                                      auto logger = getSharedLogger();
                                                      if (logger)
                                                      {
                                                          logger->addEvent(
                                                            {std::string(
                                                               "Background compilation failed: ") +
                                                               e.what(),
                                                             events::Severity::Error});
                                                      }
                                                      m_core->getMeshResourceState()
                                                        ->signalCompilationFinished();
                                                  }
                                              });
        }
        return true;
    }

    bool Document::prepareAssemblyForRefresh(nodes::ValidationContext const context)
    {
        if (!m_assembly)
        {
            return false;
        }

        std::lock_guard<std::mutex> const lock(m_assemblyMutex);

        std::optional<std::string> syncError;
        try
        {
            m_assembly->updateInputsAndOutputs();
        }
        catch (std::exception const & e)
        {
            syncError = e.what();
        }

        markValidationDirty();
        bool const graphValid = validateAssemblyIfDirty(context);

        if (!syncError.has_value())
        {
            return graphValid;
        }

        nodes::ValidationIssue issue{};
        issue.message = *syncError;
        issue.type = nodes::IssueType::GraphSyncError;
        if (syncError->find("function") != std::string::npos ||
            syncError->find("Function") != std::string::npos)
        {
            issue.type = nodes::IssueType::FunctionNotFound;
        }
        issue.severity = nodes::IssueSeverity::Error;
        issue.fixSuggestion = nodes::getFixSuggestion(issue.type);
        issue.modelId = 0u;
        issue.nodeId = {};
        m_issueList.add(std::move(issue));

        if (context != nodes::ValidationContext::Interactive)
        {
            auto logger = getSharedLogger();
            if (logger)
            {
                logger->addEvent({*syncError, events::Severity::Error});
            }
        }

        return false;
    }

    void Document::loadAllMeshResources()
    {
        io::Importer3mf importer{getSharedLogger()};
        importer.setMeshRepairConfig(m_meshRepairConfig);
        importer.setMeshSdfEvaluationConfig(m_meshSdfEvaluationConfig);
        importer.setNanoVdbBuildPolicy(getNanoVdbBuildPolicy());

        if (!m_3mfmodel)
        {
            return;
        }

        if (m_resourceDependencyGraph)
        {
            ZoneScopedN("Document::loadReachableMeshResources");
            importer.loadMeshes(
              m_3mfmodel,
              *this,
              m_resourceDependencyGraph->getAllRequiredResourceIdsForBuildItems());
            return;
        }

        // The normal load path builds this graph before mesh extraction so
        // unused mesh resources can be skipped. If this fallback path is used,
        // keep loading all meshes rather than risking missing geometry.
        assert(m_resourceDependencyGraph &&
               "Resource dependency graph should be built before loading meshes");
        importer.loadMeshes(m_3mfmodel, *this);
    }

    void Document::refreshWorker(RefreshMode const refreshMode)
    {
        ProfileFunction;

        auto meshResourceState = m_core->getMeshResourceState();

        // Capture the structural edit epoch at the start of this worker run.
        // If a new structural edit arrives while we're working, the epoch will
        // differ and we can exit early instead of completing stale work.
        auto const startEpoch = m_structuralEditEpoch.load(std::memory_order_acquire);
        auto const isStale = [this, startEpoch]()
        {
            return m_structuralEditEpoch.load(std::memory_order_acquire) != startEpoch;
        };

        if (isStale())
        {
            meshResourceState->signalCompilationFinished();
            return;
        }

        // Validate before taking the compute token so invalid graphs can fail
        // fast without waiting on in-flight GPU work.
        if (!prepareAssemblyForRefresh())
        {
            meshResourceState->signalCompilationFinished();
            return;
        }

        if (isStale())
        {
            meshResourceState->signalCompilationFinished();
            return;
        }

        auto computeToken = m_core->waitForComputeToken();
        ScopedOptimizedRenderCompilationDeferral optimizedRenderDeferral{
          *m_core,
          refreshMode == RefreshMode::InteractiveFirst};

        meshResourceState->signalCompilationStarted();

        // Build the lightweight 3MF resource dependency graph before extracting
        // mesh geometry, so loadAllMeshResources() can skip meshes that are not
        // reachable from any build item. Large unused meshes then no longer
        // contribute BVH/repair/payload work on first frame.
        rebuildResourceDependencyGraph();
        loadAllMeshResources();

        if (auto pendingMeshSdfConfig = takePendingMeshSdfEvaluationConfig())
        {
            if (applyMeshSdfEvaluationConfigToResources(*pendingMeshSdfConfig) > 0u)
            {
                m_primitiveDateNeedsUpdate = true;
            }
        }

        updateParameterRegistration();
        updateParameter();
        m_parameterDirty = true;
        m_contoursDirty = true;

        if (isStale())
        {
            meshResourceState->signalCompilationFinished();
            return;
        }

        updateFlatAssembly();

        m_core->refreshProgram(m_flatAssembly);

        // Use non-blocking compilation with polling
        m_core->recompileIfRequired();

        auto const shouldWaitForInitialCompilation = [this, refreshMode]()
        {
            if (refreshMode == RefreshMode::InteractiveFirst)
            {
                return !m_core->isRenderProgramReady() && m_core->isCompilationInProgress();
            }
            return m_core->isCompilationInProgress();
        };

        // Wait for the compilation needed by this refresh mode. Interactive-first only needs the
        // preview render program; slicer/optimized work can continue after the first frame.
        while (shouldWaitForInitialCompilation())
        {
            if (isStale())
            {
                // Wait for the in-flight GPU compile needed by this refresh mode to finish so
                // ModelState stays consistent. The GPU work is already submitted — waiting here
                // avoids signalling "finished" while required programs are still being compiled by
                // the driver.
                while (shouldWaitForInitialCompilation())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                meshResourceState->signalCompilationFinished();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // One more pass ensures ProgramManager updates its internal ModelState flags
        // (signalCompilationFinished) so subsequent steps observe up-to-date slicer state.
        m_core->recompileIfRequired();

        if (refreshMode != RefreshMode::InteractiveFirst)
        {
            // Don't invalidate SDF - let it stay valid during recomputation to avoid flicker
            // The async computation will atomically replace it
            m_core->resetBoundingBox();

            // Launch async SDF precomputation with OpenCL events
            auto const & queue = m_core->getComputeContext()->GetQueue();
            cl::Event sdfEvent = m_core->precomputeSdfAsync(queue);
            bool const sdfEventValid = sdfEvent() != nullptr;

            bool sdfUpdated = false;
            bool sdfUpdatedViaAsync = false;

            // Wait for SDF computation to complete (non-blocking on CPU, async on GPU)
            if (sdfEventValid)
            {
                sdfEvent.wait();
                sdfUpdated = true;
                sdfUpdatedViaAsync = true;
            }
            else
            {
                // Fallback to synchronous computation if async launch failed
                if (m_core->precomputeSdfForWholeBuildPlatform())
                {
                    sdfUpdated = true;
                }
                else {}
            }

            if (sdfUpdated)
            {
                m_core->setSdfValid(true);
                if (sdfUpdatedViaAsync)
                {
                    // Now that the SDF exists, update the bounding box serially (still off the UI
                    // thread)
                    m_core->updateBBox();
                }
            }
            else
            {
                m_core->invalidatePreCompSdf("refreshWorkerFailure");
            }
        }
        else
        {
            m_core->invalidatePreCompSdf("interactiveFirstInitialPreview");
        }

        if (optimizedRenderDeferral.restore())
        {
            // Start the fully compiled render program only after the command-stream preview and
            // initial SDF work are ready. The first visible frame after loading a 3MF can then use
            // the interactive backend while optimized OpenCL compilation continues in the
            // background.
            m_core->recompileIfRequired();
        }

        meshResourceState->signalCompilationFinished();
    }

    void Document::updateFlatAssembly()
    {
        ProfileFunction;
        using namespace gladius::events;

        if (!m_assembly)
        {
            return;
        }

        if (!validateAssembly())
        {
            return;
        }

        try
        {
            nodes::Assembly assemblyToFlat{*m_assembly};

            nodes::LowerFunctionGradient gradientLowering{assemblyToFlat, getSharedLogger()};
            gradientLowering.run();

            nodes::LowerNormalizeDistanceField normalizeLowering{assemblyToFlat,
                                                                  getSharedLogger()};
            normalizeLowering.run();

            nodes::OptimizeOutputs optimizer{&assemblyToFlat};
            optimizer.optimize();

            // Pass the dependency graph to the flattener if available
            nodes::GraphFlattener flattener =
              m_resourceDependencyGraph
                ? nodes::GraphFlattener(assemblyToFlat, m_resourceDependencyGraph.get())
                : nodes::GraphFlattener(assemblyToFlat);

            m_flatAssembly = std::make_shared<nodes::Assembly>(flattener.flatten());
        }
        catch (std::exception const & e)
        {
            auto logger = getSharedLogger();
            if (logger)
                logger->addEvent(
                  {std::string("Error flattening assembly: ") + e.what(), Severity::Error});
        }
    }

    void Document::updateMemoryOffsets()
    {
        if (!m_generatorContext)
        {
            throw std::runtime_error("No generator context");
        }

        for (auto & model : m_assembly->getFunctions())
        {
            if (!model.second)
            {
                continue;
            }
            for (auto & node : *model.second)
            {
                node.second->updateMemoryOffsets(*m_generatorContext);
            }
        }
    }

    void Document::saveBackup()
    {
        // Only create backups when UI mode is active
        if (!m_uiMode)
        {
            return;
        }

        // Use the new BackupManager for improved backup handling
        try
        {
            // Create a temporary file for the backup
            auto tempDir = std::filesystem::temp_directory_path();
            auto tempBackupFile = tempDir / "gladius_temp_backup.3mf";

            // Preserve the original filename before saving backup
            auto originalFilename = m_currentAssemblyFileName;

            // Save current model to temporary file
            saveAs(tempBackupFile, false);

            // Restore the original filename (backup shouldn't change current file)
            m_currentAssemblyFileName = originalFilename;

            // Get original filename for backup naming
            std::string originalName = "untitled";
            if (originalFilename.has_value() && !originalFilename->empty())
            {
                originalName = originalFilename->stem().string();
            }

            // Create backup using BackupManager
            m_backupManager.createBackup(tempBackupFile, originalName);

            // Clean up temporary file
            if (std::filesystem::exists(tempBackupFile))
            {
                std::filesystem::remove(tempBackupFile);
            }

            // Update backup time
            m_lastBackupTime = std::chrono::system_clock::now();
        }
        catch (const std::exception &)
        {
            // Backup failure shouldn't crash the application
            // Could log error here if logging is available
        }
    }

    bool Document::refreshModelIfNoCompilationIsRunning()
    {
        ProfileFunction;

        // This method is called from the UI frame loop. Only use non-blocking probes here;
        // the background load/refresh worker can hold the compute token for several seconds
        // while importing meshes and preparing GPU resources.
        if (m_core->isAnyCompilationInProgressNonBlocking())
        {
            return false;
        }

        auto const meshResourceState = m_core->getMeshResourceState();
        if (meshResourceState && !meshResourceState->isModelUpToDate())
        {
            return false;
        }

        {
            auto computeToken = m_core->requestComputeToken();
            if (!computeToken.has_value())
            {
                return false;
            }
        }

        if (m_core->isAnyCompilationInProgressNonBlocking())
        {
            return false;
        }

        return refreshModelAsync();
    }

    void Document::newModel()
    {
        ProfileFunction;
        {

            m_assembly = std::make_shared<nodes::Assembly>();
            m_assembly->assemblyModel()->createValidVoid();
        }
        m_modelFileName.clear();
        m_3mfmodel.reset();

        io::Importer3mf importer{getSharedLogger()};
        importer.setMeshRepairConfig(m_meshRepairConfig);
        importer.setMeshSdfEvaluationConfig(m_meshSdfEvaluationConfig);
        m_3mfmodel = importer.get3mfWrapper()->CreateModel();

        m_core->getResourceContext()->clearImageStacks();
        resetGeneratorContext();
    }

    void Document::newEmptyModel()
    {
        ProfileFunction;
        {

            m_assembly = std::make_shared<nodes::Assembly>();
            m_assembly->assemblyModel()->createBeginEndWithDefaultInAndOuts();
        }
        m_modelFileName.clear();
        m_3mfmodel.reset();

        io::Importer3mf importer{getSharedLogger()};
        importer.setMeshRepairConfig(m_meshRepairConfig);
        importer.setMeshSdfEvaluationConfig(m_meshSdfEvaluationConfig);
        m_3mfmodel = importer.get3mfWrapper()->CreateModel();

        m_core->getResourceContext()->clearImageStacks();
        resetGeneratorContext();
    }

    void Document::newFromTemplate()
    {
        auto templateFiletName = getAppDir() / "examples/template.3mf";
        if (!std::filesystem::exists(templateFiletName))
        {
            newModel();
            return;
        }
        loadNonBlocking(templateFiletName);
    }

    bool Document::updateParameter()
    {
        ProfileFunction;
        if (!m_assembly || !m_core)
        {
            return false;
        }

        updatePayload();

        // Check if we can use the fast path (parameter structure unchanged)
        bool const canUseFastPath = m_core->isParameterSignatureCompatible(*m_assembly);

        auto attemptParameterUpdate = [&]() -> bool
        {
            if (!m_core->tryToupdateParameter(*m_assembly))
            {
                return false;
            }
            return true;
        };

        bool updateSucceeded = false;

        if (canUseFastPath)
        {
            updateSucceeded = attemptParameterUpdate();
        }
        else
        {
            // Slow path: parameter structure changed
            // NOTE: We don't call refreshModelAsync() here because updateParameter()
            // is often called FROM WITHIN refreshWorker(), which would cause recursion.
            // Instead, we just update normally and let the signature be recaptured
            // on the next full refresh cycle.
            auto logger = getSharedLogger();
            if (logger)
            {
                logger->addEvent(
                  {"Parameter structure mismatch detected (will be updated on next refresh)",
                   events::Severity::Info});
            }

            updateSucceeded = attemptParameterUpdate();
        }

        m_parameterDirty = !updateSucceeded;
        return updateSucceeded;
    }

    void Document::updateParameterRegistration()
    {

        if (!m_assembly)
        {
            return;
        }

        for (auto & model : m_assembly->getFunctions())
        {
            if (!model.second)
            {
                continue;
            }
            for (auto & node : *model.second)
            {
                model.second->registerInputs(*node.second);
            }
        }
    }

    void Document::updatePayload()
    {
        ProfileFunction;
        if (!m_generatorContext)
        {
            resetGeneratorContext();
        }
        if (!m_generatorContext)
        {
            throw std::runtime_error("No generator context");
        }
        try
        {
            if (!m_core)
            {
                return;
            }

            auto computeToken = m_core->requestComputeToken();
            if (!computeToken.has_value())
            {
                return; // Background worker holds the compute mutex; skip to avoid blocking
            }

            if (!m_generatorContext)
            {
                return;
            }
            // Get the primitives directly as a shared_ptr
            m_generatorContext->primitives = m_core->getPrimitives();
            if (!m_generatorContext->primitives)
            {
                return;
            }

            {

                if (!m_assembly)
                {
                    return;
                }

                m_generatorContext->basePath = m_assembly->getFilename().remove_filename();
            }
            // Get the compute context directly as a shared_ptr
            m_generatorContext->computeContext = m_core->getComputeContext();
            if (!m_generatorContext->computeContext)
            {
                return;
            }

            CL_ERROR(m_core->getComputeContext()->GetQueue().finish());

            updateMemoryOffsets(); // determines which resources are needed
            if (m_primitiveDateNeedsUpdate)
            {
                if (m_generatorContext->primitives)
                {
                    m_generatorContext->primitives->clear();
                }

                updateParameterRegistration();

                {

                    for (auto & model : m_assembly->getFunctions())
                    {
                        if (!model.second)
                        {
                            continue;
                        }
                        for (auto & node : *model.second)
                        {
                            if (node.second)
                            {
                                node.second->generate(*m_generatorContext);
                            }
                        }
                    }
                }

                m_generatorContext->resourceManager.loadResources();
                m_generatorContext->resourceManager.writeResources(*m_generatorContext->primitives);
                
                // Build voxel acceleration grids for spatial mesh resources on GPU
                auto buildParams = m_generatorContext->resourceManager.collectVoxelGridBuildParams();
                if (!buildParams.empty())
                {
                    size_t const builtCount = m_core->buildMeshVoxelGrids(buildParams);
                    if (builtCount == buildParams.size())
                    {
                        m_generatorContext->resourceManager.markVoxelGridsBuilt();
                    }
                }

                m_primitiveDateNeedsUpdate = false;
            }

            // Build FWN aggregate buffers on the GPU before any FWN render or
            // sign-cache kernel can consume them. This replaces the previous
            // CPU aggregate pre-pass while preserving in-order queue safety.
            auto fwnAggregateBuildParams = m_generatorContext->resourceManager.collectFwnAggregateBuildParams();
            if (!fwnAggregateBuildParams.empty())
            {
                GLADIUS_FWN_PREP_SCOPE("Document::updatePrimitiveData FWN aggregate stage");
                GLADIUS_FWN_PREP_LOG("Document::updatePrimitiveData FWN aggregate builds=" +
                                     std::to_string(fwnAggregateBuildParams.size()));
                size_t const builtCount = m_core->buildMeshFwnAggregates(fwnAggregateBuildParams);
                if (builtCount > 0u)
                {
                    m_generatorContext->resourceManager.markFwnAggregatesBuilt(fwnAggregateBuildParams,
                                                                               builtCount);
                }
            }

            // Queue bounded coarse FWN sign-cache work. The cache becomes visible
            // to kernels only after the final queued ready-offset patch executes,
            // so FWN falls back to full winding traversal until the cache is ready.
            auto signCacheBuildParams = m_generatorContext->resourceManager.collectSignCacheBuildParams();
            if (!signCacheBuildParams.empty())
            {
                GLADIUS_FWN_PREP_SCOPE("Document::updatePrimitiveData FWN sign-cache stage");
                GLADIUS_FWN_PREP_LOG("Document::updatePrimitiveData FWN sign-cache steps=" +
                                     std::to_string(signCacheBuildParams.size()));
                size_t const queuedCount = m_core->buildMeshSignCaches(signCacheBuildParams);
                if (queuedCount > 0u)
                {
                    m_generatorContext->resourceManager.markSignCacheBuildProgress(signCacheBuildParams, queuedCount);
                }
            }

            // update start and end indices
            updateMemoryOffsets();
        }
        catch (std::exception const & e)
        {
            auto logger = getSharedLogger();
            if (logger)
                logger->addEvent(
                  {std::string("unhandled exception: ") + e.what(), events::Severity::Error});
        }
    }

    void Document::refreshModelBlocking()
    {
        ProfileFunction;
        m_core->getSlicerProgram()->waitForCompilation();
        {

            refreshWorker();
        }
        try
        {
            if (m_futureModelRefresh.valid())
            {
                m_futureModelRefresh.get(); // wait for the future to finish
            }
        }
        catch (std::future_error const & e)
        {
            auto logger = getSharedLogger();
            if (logger)
                logger->addEvent(
                  {std::string("future error: ") + e.what(), events::Severity::Error});
        }
        m_core->compileSlicerProgramBlocking();
        updateParameter();

        saveBackup();
    }

    void Document::exportAsStl(std::filesystem::path const & filename)
    {
        exportAsStl(filename, io::StlExportOptions{});
    }

    void Document::exportAsStl(std::filesystem::path const & filename,
                               io::StlExportOptions const & options)
    {
        refreshModelBlocking();

        auto logger = getSharedLogger();

        switch (options.method)
        {
        case io::SurfaceExtractionMethod::LayeredMarchingCubes:
        {
            vdb::MeshExporter exporter;
            exporter.setQualityLevel(options.marchingCubesQualityLevel);
            exporter.beginExport(filename, *m_core);
            while (exporter.advanceExport(*m_core))
            {
                if (logger)
                {
                    logger->addEvent(
                      {fmt::format("Processing layer with z = {}", m_core->getSliceHeight()),
                       events::Severity::Info});
                }
            }
            exporter.finalizeExportSTL(*m_core);
            break;
        }
        case io::SurfaceExtractionMethod::DualContouring:
        {
            io::DualContouringOptions dualOptions = options.dualContouring;
            io::DualContouringStlExporter exporter(logger);
            exporter.setOptions(std::move(dualOptions));
            exporter.beginExport(filename, *m_core);
            while (exporter.advanceExport(*m_core)) {}
            bool const failed = exporter.hasError();
            auto const errorText = exporter.errorMessage();
            exporter.finalize();
            if (failed)
            {
                throw std::runtime_error(errorText.empty() ? "Dual contouring STL export failed"
                                                           : errorText);
            }
            if (logger)
            {
                logger->addEvent(
                  {fmt::format("Dual contouring STL export completed: {}", filename.string()),
                   events::Severity::Info});
            }
            break;
        }
        case io::SurfaceExtractionMethod::ManifoldDualContouring:
        {
            io::ManifoldDualContouringOptions manifoldOptions = options.manifoldDualContouring;
            io::ManifoldDualContouringStlExporter exporter(logger);
            exporter.setOptions(std::move(manifoldOptions));
            exporter.beginExport(filename, *m_core);
            while (exporter.advanceExport(*m_core)) {}
            bool const failed = exporter.hasError();
            auto const errorText = exporter.errorMessage();
            exporter.finalize();
            if (failed)
            {
                throw std::runtime_error(
                  errorText.empty() ? "Manifold dual contouring STL export failed" : errorText);
            }
            if (logger)
            {
                logger->addEvent({fmt::format("Manifold dual contouring STL export completed: {}",
                                              filename.string()),
                                  events::Severity::Info});
            }
            break;
        }
        default:
            throw std::runtime_error("Unsupported surface extraction method");
        }
    }

    void Document::markFileAsChanged()
    {
        m_fileChanged = true;
        m_validationDirty = true;
    }

    void Document::invalidatePrimitiveData()
    {
        m_primitiveDateNeedsUpdate = true;
    }

    void Document::load(std::filesystem::path filename)
    {
        ScopedNanoVdbFailurePolicy failurePolicyScope{m_nanovdbFailurePolicy,
                                                      NanoVdbFailurePolicy::Fail};

        try
        {
            loadImpl(filename);
            // reset back up time
            m_lastBackupTime = std::chrono::system_clock::now();

            // Initial validation with FileLoad context - logs errors once for file loading
            validateAssembly(nodes::ValidationContext::FileLoad);

            refreshModelBlocking();
            m_core->updateBBox();
        }
        catch (NanoVdbBuildRejectedError const &)
        {
            newModel();
            throw;
        }
    }

    void Document::loadNonBlocking(std::filesystem::path filename)
    {
        // Collect any finished previous load operation without blocking the caller. A still
        // running load owns document/compute state; waiting here would freeze the UI thread.
        if (m_futureFileLoad.valid())
        {
            if (m_futureFileLoad.wait_for(std::chrono::milliseconds(0)) !=
                std::future_status::ready)
            {
                auto logger = getSharedLogger();
                if (logger)
                {
                    logger->addEvent({"Ignored file load request: another file is still loading",
                                      events::Severity::Warning});
                }
                return;
            }

            try
            {
                m_futureFileLoad.get();
            }
            catch (const std::exception &)
            {
                // Previous load error already stored
            }
        }

        // Clear any previous error
        {
            std::lock_guard<std::mutex> lock(m_loadingErrorMutex);
            m_loadingError.clear();
        }

        // Launch async file loading
        m_isLoading.store(true, std::memory_order_relaxed);
        try
        {
            m_futureFileLoad = std::async(
              std::launch::async,
              [this, filename]()
              {
                  ScopedLoadingFlag loadingScope{m_isLoading};
                  ScopedNanoVdbFailurePolicy failurePolicyScope{m_nanovdbFailurePolicy,
                                                                NanoVdbFailurePolicy::Degrade};

                  try
                  {
                      auto const refreshMode = filename.extension() == ".3mf"
                                                 ? RefreshMode::InteractiveFirst
                                                 : RefreshMode::Normal;
                      auto const previewMeshSdfConfig =
                        makeInteractivePreviewMeshSdfConfig(m_meshSdfEvaluationConfig);
                      ScopedMeshSdfEvaluationConfigOverride previewMeshConfigOverride{
                        *this, previewMeshSdfConfig, refreshMode == RefreshMode::InteractiveFirst};

                      loadImpl(filename);
                      // Initial validation with FileLoad context - logs
                      // errors once
                      validateAssembly(nodes::ValidationContext::FileLoad);
                      // Chain into async model refresh. 3MF loads publish
                      // a lightweight command-stream preview first, then
                      // start the optimized renderer and slicer
                      // compilation in the background.
                      refreshWorker(refreshMode);
                  }
                  catch (std::exception const & e)
                  {
                      // Store error for UI to display
                      {
                          std::lock_guard<std::mutex> lock(m_loadingErrorMutex);
                          m_loadingError = e.what();
                      }
                      auto logger = getSharedLogger();
                      if (logger)
                      {
                          logger->addEvent({fmt::format("File load error: {}", e.what()),
                                            events::Severity::Error});
                      }
                  }
                  catch (...)
                  {
                      std::lock_guard<std::mutex> lock(m_loadingErrorMutex);
                      m_loadingError = "Unknown file load error";
                  }
              });
        }
        catch (...)
        {
            m_isLoading.store(false, std::memory_order_relaxed);
            throw;
        }
    }

    bool Document::isLoadingInProgress() const
    {
        return m_isLoading.load();
    }

    std::string Document::getLoadingError() const
    {
        std::lock_guard<std::mutex> lock(m_loadingErrorMutex);
        return m_loadingError;
    }

    void Document::merge(std::filesystem::path filename)
    {
        mergeImpl(filename);
        (void) refreshModelAsync(); // Result intentionally ignored for merge
    }

    void Document::mergeOnly(std::filesystem::path filename)
    {
        mergeImpl(filename);
    }

    nodes::FunctionMatch Document::mergeAndResolve(std::filesystem::path filename,
                                                   std::string const & targetFunctionName)
    {
        // Snapshot existing function IDs before the merge.
        std::set<nodes::ResourceId> existingIds;
        if (m_assembly)
        {
            for (auto const & [id, _] : m_assembly->getFunctions())
            {
                existingIds.insert(id);
            }
        }

        // Try selective import: prune the source to only the tagged function
        // and its transitive dependencies before merging.
        if (filename.extension() == ".3mf")
        {
            auto prunedModel = io::pruneSourceForImport(filename, getSharedLogger());
            if (prunedModel.has_value())
            {
                io::mergeModelInto3mfDoc(*prunedModel, filename, *this);
            }
            else
            {
                mergeImpl(filename);
            }
        }
        else
        {
            mergeImpl(filename);
        }
        m_primitiveDateNeedsUpdate = true;

        if (!m_assembly)
        {
            return {};
        }

        return m_assembly->findImportedFunction(targetFunctionName, existingIds, nullptr);
    }

    void Document::saveAs(std::filesystem::path filename, bool writeThumbnail)
    {
        if (filename.extension() == ".3mf")
        {
            if (writeThumbnail && m_core)
            {
                auto computeToken = m_core->waitForComputeToken();
                (void) computeToken;

                // Ensure the GPU pipeline is fully up-to-date so the
                // thumbnail reflects the latest model state (colors, geometry, etc.).
                updateParameterRegistration();
                updateParameter();
                updateFlatAssembly();
                m_core->tryRefreshProgramProtected(getFlatAssembly());

                if (!m_core->prepareImageRendering())
                {
                    writeThumbnail = false;
                }
            }
            io::saveTo3mfFile(filename, *this, writeThumbnail);
        }

        m_fileChanged = false;
        m_currentAssemblyFileName = filename;

        {

            m_assembly->setFilename(filename);
        }
    }

    nodes::SharedAssembly Document::getAssembly() const
    {

        return m_assembly;
    }

    nodes::SharedAssembly Document::getFlatAssembly() const
    {
        return m_flatAssembly;
    }

    std::optional<std::filesystem::path> Document::getCurrentAssemblyFilename() const
    {
        return m_currentAssemblyFileName;
    }

    float Document::getFloatParameter(ResourceId modelId,
                                      std::string const & nodeName,
                                      std::string const & parameterName)
    {

        auto const & parameter = findParameterOrThrow(modelId, nodeName, parameterName);
        auto val = parameter.getValue();
        if (auto * pval = std::get_if<float>(&val))
        {
            return *pval;
        }
        throw ParameterCouldNotBeConvertedToFloat();
    }

    void Document::setFloatParameter(ResourceId modelId,
                                     std::string const & nodeName,
                                     std::string const & parameterName,
                                     float value)
    {

        findParameterOrThrow(modelId, nodeName, parameterName).setValue(value);
    }

    std::string & Document::getStringParameter(ResourceId modelId,
                                               std::string const & nodeName,
                                               std::string const & parameterName)
    {

        auto const & parameter = findParameterOrThrow(modelId, nodeName, parameterName);
        auto val = parameter.getValue();
        if (auto * pval = std::get_if<std::string>(&val))
        {
            return *pval;
        }
        throw ParameterCouldNotBeConvertedToString();
    }

    void Document::setStringParameter(ResourceId modelId,
                                      std::string const & nodeName,
                                      std::string const & parameterName,
                                      std::string const & value)
    {

        findParameterOrThrow(modelId, nodeName, parameterName).setValue(value);
    }

    nodes::float3 & Document::getVector3fParameter(ResourceId modelId,
                                                   std::string const & nodeName,
                                                   std::string const & parameterName)
    {

        auto const & parameter = findParameterOrThrow(modelId, nodeName, parameterName);
        auto val = parameter.getValue();
        if (auto * pval = std::get_if<nodes::float3>(&val))
        {
            return *pval;
        }
        throw ParameterCouldNotBeConvertedToVector();
    }

    void Document::setVector3fParameter(ResourceId modelId,
                                        std::string const & nodeName,
                                        std::string const & parameterName,
                                        nodes::float3 const & value)
    {

        findParameterOrThrow(modelId, nodeName, parameterName).setValue(value);
    }

    PolyLines Document::generateContour(float z, nodes::SliceParameter const & sliceParameter) const
    {
        if (z != m_core->getSliceHeight())
        {
            m_core->setSliceHeight(z);
            m_core->requestContourUpdate(sliceParameter);
        }

        auto contourExtractor = m_core->getContour();
        PolyLines contours = contourExtractor->getContour();
        if (sliceParameter.offset != 0.f)
        {
            return contourExtractor->generateOffsetContours(sliceParameter.offset, contours);
        }
        return contours;
    }

    BoundingBox Document::computeBoundingBox() const
    {

        if (!m_core->updateBBox())
        {
            return {};
        }
        m_core->getResourceContext()->releasePreComputedSdf(); // saving memory (api usage)
        return m_core->getBoundingBox().value_or(BoundingBox{});
    }

    gladius::Mesh Document::generateMesh() const
    {

        return gladius::vdb::generatePreviewMesh(*m_core, *m_assembly);
    }

    BitmapChannels & Document::getBitmapChannels()
    {
        return m_channels;
    }

    nodes::GeneratorContext & Document::getGeneratorContext()
    {
        return *m_generatorContext;
    }

    SharedComputeContext Document::getComputeContext() const
    {
        if (!m_core)
        {
            throw std::runtime_error("No core");
        }
        return m_core->getComputeContext();
    }

    NanoVdbBuildPolicy Document::getNanoVdbBuildPolicy() const
    {
        return makeNanoVdbBuildPolicy(
          m_core ? m_core->getComputeContext() : SharedComputeContext{},
          m_nanovdbFailurePolicy.load(std::memory_order_relaxed));
    }

    NanoVdbBuildIssueSummary Document::getNanoVdbBuildIssueSummary() const
    {
        NanoVdbBuildIssueSummary summary{};
        std::string firstMessage;

        for (auto const & [key, resource] : getResourceManager().getResourceMap())
        {
            if (key.getResourceType() != ResourceType::Mesh)
            {
                continue;
            }

            auto const * spatialMesh = dynamic_cast<SpatialMeshResource const *>(resource.get());
            if (spatialMesh == nullptr ||
                spatialMesh->evaluationConfig().method != MeshSdfMethod::NanoVDB ||
                !spatialMesh->hasNanoVdbBuildIssue())
            {
                continue;
            }

            ++summary.affectedMeshCount;
            summary.hasIssue = true;
            auto const & buildInfo = spatialMesh->getNanoVdbBuildInfo();
            summary.suggestedVoxelSize_mm = std::max(summary.suggestedVoxelSize_mm,
                                                     buildInfo.suggestedVoxelSize_mm);

            if (firstMessage.empty())
            {
                firstMessage = spatialMesh->formatNanoVdbBuildMessage(key.getDisplayName());
            }
        }

        if (!summary.hasIssue)
        {
            return summary;
        }

        if (summary.affectedMeshCount == 1u)
        {
            summary.message = std::move(firstMessage);
            return summary;
        }

        summary.message = fmt::format(
          "NanoVDB is unavailable for {} mesh resources. First issue: {}",
          summary.affectedMeshCount,
          firstMessage);
        return summary;
    }

    MeshQualityIssueSummary Document::getMeshQualityIssueSummary() const
    {
        MeshQualityIssueSummary summary{};
        std::string firstMessage;

        for (auto const & [key, resource] : getResourceManager().getResourceMap())
        {
            if (key.getResourceType() != ResourceType::Mesh)
            {
                continue;
            }

            auto const * spatialMesh = dynamic_cast<SpatialMeshResource const *>(resource.get());
            if (spatialMesh == nullptr || !spatialMesh->hasMeshQualityIssues())
            {
                continue;
            }

            ++summary.affectedMeshCount;
            summary.hasIssue = true;

            auto const & quality = spatialMesh->getMeshQualityDiagnostics();
            summary.degenerateTriangleCount +=
              static_cast<std::size_t>(quality.degenerateTriangleCount);
            summary.boundaryEdgeCount += static_cast<std::size_t>(quality.boundaryEdgeCount);
            summary.nonManifoldEdgeCount +=
              static_cast<std::size_t>(quality.nonManifoldEdgeCount);

            if (firstMessage.empty())
            {
                firstMessage = spatialMesh->formatMeshQualityMessage(key.getDisplayName());
            }
        }

        if (!summary.hasIssue)
        {
            return summary;
        }

        if (summary.affectedMeshCount == 1u)
        {
            summary.message = std::move(firstMessage);
            return summary;
        }

        summary.message = fmt::format(
          "Mesh topology diagnostics affect {} mesh resources ({} boundary edges, {} "
          "non-manifold edges, {} degenerate triangles total). First issue: {}",
          summary.affectedMeshCount,
          summary.boundaryEdgeCount,
          summary.nonManifoldEdgeCount,
          summary.degenerateTriangleCount,
          firstMessage);
        return summary;
    }

    events::SharedLogger Document::getSharedLogger() const
    {
        if (!m_core)
        {
            throw std::runtime_error("No core");
        }
        return m_core->getSharedLogger();
    }

    std::shared_ptr<ComputeCore> Document::getCore()
    {
        return m_core;
    }

    void Document::set3mfModel(Lib3MF::PModel model)
    {
        m_3mfmodel = std::move(model);
    }

    Lib3MF::PModel Document::get3mfModel() const
    {
        return m_3mfmodel;
    }

    nodes::Model & Document::createNewFunction()
    {
        if (!m_3mfmodel)
        {
            throw std::runtime_error("No 3mf model loaded");
        }

        auto const new3mfFunc = m_3mfmodel->AddImplicitFunction();
        auto const modelId = new3mfFunc->GetModelResourceID();

        std::lock_guard<std::mutex> lock(m_assemblyMutex);
        m_assembly->addModelIfNotExisting(modelId);
        auto & model = *m_assembly->getFunctions().at(modelId);
        model.createBeginEnd();
        return model;
    }

    nodes::Model & Document::createLevelsetFunction(std::string const & name)
    {
        if (!m_3mfmodel)
        {
            throw std::runtime_error("No 3mf model loaded");
        }

        auto const new3mfFunc = m_3mfmodel->AddImplicitFunction();
        auto const modelId = new3mfFunc->GetModelResourceID();

        std::lock_guard<std::mutex> lock(m_assemblyMutex);
        m_assembly->addModelIfNotExisting(modelId);
        auto & model = *m_assembly->getFunctions().at(modelId);

        // Create begin and end nodes
        model.createBeginEnd();
        model.setDisplayName(name);

        // Add pos vector input to begin node using addArgument so that both
        // the output port and parameter entry are created (consistent with
        // createBeginEndWithDefaultInAndOuts).
        model.addArgument(nodes::FieldNames::Pos,
                          nodes::VariantParameter(nodes::float3{0.0f, 0.0f, 0.0f}));

        // Add color vector output and shape scalar output to end node
        model.getEndNode()->parameter()[nodes::FieldNames::Color] =
          nodes::VariantParameter(nodes::float3{0.5f, 0.5f, 0.5f});
        model.getEndNode()->parameter()[nodes::FieldNames::Shape] =
          nodes::VariantParameter(float{-1.f});

        model.registerInputs(*model.getEndNode());
        model.getBeginNode()->updateNodeIds();
        model.getEndNode()->updateNodeIds();

        return model;
    }

    nodes::Model & Document::copyFunction(nodes::Model const & sourceModel,
                                          std::string const & name)
    {
        if (!m_3mfmodel)
        {
            throw std::runtime_error("No 3mf model loaded");
        }

        auto const new3mfFunc = m_3mfmodel->AddImplicitFunction();
        auto const modelId = new3mfFunc->GetModelResourceID();

        std::lock_guard<std::mutex> lock(m_assemblyMutex);
        m_assembly->addModelIfNotExisting(modelId);
        auto & model = *m_assembly->getFunctions().at(modelId);

        // Copy the source model
        model = sourceModel;
        model.setDisplayName(name);
        model.setResourceId(modelId);

        return model;
    }

    nodes::Model & Document::wrapExistingFunction(nodes::Model & sourceModel,
                                                  std::string const & name)
    {
        if (!m_3mfmodel)
        {
            throw std::runtime_error("No 3mf model loaded");
        }

        auto const new3mfFunc = m_3mfmodel->AddImplicitFunction();
        auto const modelId = new3mfFunc->GetModelResourceID();

        std::lock_guard<std::mutex> lock(m_assemblyMutex);
        m_assembly->addModelIfNotExisting(modelId);
        auto & model = *m_assembly->getFunctions().at(modelId);

        // Create begin and end nodes with same inputs and outputs as source
        model.createBeginEnd();
        model.setDisplayName(name);

        // Copy inputs from source model
        auto const & sourceInputs = sourceModel.getInputs();
        for (auto const & [inputName, inputPort] : sourceInputs)
        {
            model.getBeginNode()->addOutputPort(inputName, inputPort.getTypeIndex());
        }
        model.registerOutputs(*model.getBeginNode());

        // Copy outputs from source model
        auto const & sourceOutputs = sourceModel.getOutputs();
        for (auto const & [outputName, outputPort] : sourceOutputs)
        {
            model.getEndNode()->parameter()[outputName] =
              nodes::createVariantTypeFromTypeIndex(outputPort.getTypeIndex());
        }
        model.registerInputs(*model.getEndNode());

        // Create Resource node for the source function
        auto resourceNode = model.create<nodes::Resource>();
        resourceNode->parameter().at(nodes::FieldNames::ResourceId) =
          nodes::VariantParameter(sourceModel.getResourceId());

        // Create FunctionCall node
        auto functionCallNode = model.create<nodes::FunctionCall>();
        functionCallNode->parameter()
          .at(nodes::FieldNames::FunctionId)
          .setInputFromPort(resourceNode->getOutputs().at(nodes::FieldNames::Value));

        // Set display name to source function name
        auto sourceFunctionName = sourceModel.getDisplayName();
        if (sourceFunctionName.has_value())
        {
            functionCallNode->setDisplayName(sourceFunctionName.value());
        }

        // Update the function call node's inputs and outputs to match the source model
        functionCallNode->updateInputsAndOutputs(sourceModel);
        model.registerInputs(*functionCallNode);
        model.registerOutputs(*functionCallNode);

        // Connect begin node outputs to function call inputs
        for (auto const & [inputName, inputPort] : sourceInputs)
        {
            auto beginOutputIter = model.getBeginNode()->getOutputs().find(inputName);
            auto functionInputIter = functionCallNode->parameter().find(inputName);

            if (beginOutputIter != model.getBeginNode()->getOutputs().end() &&
                functionInputIter != functionCallNode->parameter().end())
            {
                functionInputIter->second.setInputFromPort(beginOutputIter->second);
            }
        }

        // Connect function call outputs to end node inputs
        for (auto const & [outputName, outputPort] : sourceOutputs)
        {
            auto functionOutputIter = functionCallNode->getOutputs().find(outputName);
            auto endInputIter = model.getEndNode()->parameter().find(outputName);

            if (functionOutputIter != functionCallNode->getOutputs().end() &&
                endInputIter != model.getEndNode()->parameter().end())
            {
                endInputIter->second.setInputFromPort(functionOutputIter->second);
            }
        }

        model.getBeginNode()->updateNodeIds();
        model.getEndNode()->updateNodeIds();

        return model;
    }

    nodes::VariantParameter & Document::findParameterOrThrow(ResourceId modelId,
                                                             std::string const & nodeName,
                                                             std::string const & parameterName)
    {

        auto const modelIter = m_assembly->getFunctions().find(modelId);
        if (modelIter == std::end(m_assembly->getFunctions()))
        {
            throw ParameterAndModelNotFound();
        }

        auto & model = modelIter->second;
        auto const node = model->findNode(nodeName);
        if (!node)
        {
            throw ParameterAndNodeNotFound();
        }

        auto const parameterIter = node->parameter().find(parameterName);
        if (parameterIter == std::end(node->parameter()))
        {
            throw ParameterNotFoundException();
        }
        return parameterIter->second;
    }

    void Document::loadImpl(const std::filesystem::path & filename)
    {
        auto computeToken = m_core->waitForComputeToken();
        m_buildItems.clear();
        // clear event logger
        auto logger = getSharedLogger();
        if (logger)
        {
            logger->clear();
        }

        // Check if file exists before attempting to load
        if (!std::filesystem::exists(filename))
        {
            if (logger)
            {
                logger->addEvent(
                  {fmt::format("File not found: {}", filename.string()), events::Severity::Error});
            }
            newModel(); // Create empty model if file doesn't exist
            return;
        }

        resetGeneratorContext();
        m_core->reset();
        m_core->getResourceContext()->clearImageStacks();
        auto newFilename = filename;
        m_primitiveDateNeedsUpdate = true;

        {

            m_assembly->setFilename(filename);
        }

        if (filename.extension() == ".vdb")
        {
            newEmptyModel();
            io::loadFromOpenVdbFile(filename, *this);
            return;
        }

        newFilename.replace_extension(".3mf");
        m_currentAssemblyFileName = newFilename;

        if (filename.extension() == ".3mf")
        {
            {

                m_assembly = {};
            }

            try
            {
                io::loadFrom3mfFile(filename, *this);
            }
            catch (NanoVdbBuildRejectedError const &)
            {
                throw;
            }
            catch (std::exception const & e)
            {
                auto logger = getSharedLogger();
                if (logger)
                    logger->addEvent(
                      {std::string("unhandled exception: ") + e.what(), events::Severity::Error});
                newModel();
                return;
            }
        }
    }

    void Document::mergeImpl(const std::filesystem::path & filename)
    {
        if (filename.extension() == ".3mf")
        {
            io::mergeFrom3mfFile(filename, *this);
        }
        m_primitiveDateNeedsUpdate = true;
    }

    std::size_t Document::queueMeshSdfEvaluationConfigUpdate(MeshSdfEvaluationConfig const & cfg)
    {
        m_meshSdfEvaluationConfig = cfg;

        std::size_t affectedResources = 0u;
        for (auto const & [key, resource] : getResourceManager().getResourceMap())
        {
            if (key.getResourceType() != ResourceType::Mesh)
            {
                continue;
            }
            auto const * spatialMesh = dynamic_cast<SpatialMeshResource const *>(resource.get());
            if (spatialMesh != nullptr && needsMeshSdfResourceUpdate(*spatialMesh, cfg))
            {
                ++affectedResources;
            }
        }

        if (affectedResources == 0u)
        {
            return 0u;
        }

        {
            std::lock_guard<std::mutex> lock(m_pendingMeshSdfEvaluationConfigMutex);
            m_pendingMeshSdfEvaluationConfig = cfg;
        }
        m_primitiveDateNeedsUpdate = true;

        // Defer the heavy resource rebuild/upload to the existing structural-refresh debounce.
        // This gives the interactive preview at least one UI frame before voxel/FWN preparation
        // can compete for the compute device.
        signalStructuralEdit();
        return affectedResources;
    }

    std::optional<MeshSdfEvaluationConfig> Document::takePendingMeshSdfEvaluationConfig()
    {
        std::lock_guard<std::mutex> lock(m_pendingMeshSdfEvaluationConfigMutex);
        auto pending = m_pendingMeshSdfEvaluationConfig;
        m_pendingMeshSdfEvaluationConfig.reset();
        return pending;
    }

    std::size_t Document::applyMeshSdfEvaluationConfigToResources(
      MeshSdfEvaluationConfig const & cfg)
    {
        auto const nanovdbBuildPolicy = getNanoVdbBuildPolicy();
        std::size_t changedResources = 0u;
        for (auto const & [key, resource] : getResourceManager().getResourceMap())
        {
            if (key.getResourceType() != ResourceType::Mesh)
            {
                continue;
            }
            auto * spatialMesh = dynamic_cast<SpatialMeshResource *>(resource.get());
            if (spatialMesh != nullptr)
            {
                spatialMesh->setNanoVdbBuildPolicy(nanovdbBuildPolicy);
                bool const changed = spatialMesh->setEvaluationConfig(cfg);
                if (changed)
                {
                    ++changedResources;
                }

                if (spatialMesh->evaluationConfig().method == MeshSdfMethod::NanoVDB &&
                    spatialMesh->hasMeshQualityIssues())
                {
                    auto const message = spatialMesh->formatMeshQualityMessage(
                      key.getDisplayName());
                    auto logger = getSharedLogger();
                    if (logger)
                    {
                        logger->addEvent(
                          {message,
                           nanovdbBuildPolicy.failurePolicy == NanoVdbFailurePolicy::Fail
                             ? events::Severity::Error
                             : events::Severity::Warning});
                    }

                    if (nanovdbBuildPolicy.failurePolicy == NanoVdbFailurePolicy::Fail)
                    {
                        throw NanoVdbBuildRejectedError(message);
                    }
                }

                if (changed && spatialMesh->evaluationConfig().method == MeshSdfMethod::NanoVDB &&
                    spatialMesh->hasNanoVdbBuildIssue())
                {
                    auto const message = spatialMesh->formatNanoVdbBuildMessage(
                      key.getDisplayName());
                    auto logger = getSharedLogger();
                    if (logger)
                    {
                        logger->addEvent(
                          {message,
                           nanovdbBuildPolicy.failurePolicy == NanoVdbFailurePolicy::Fail
                             ? events::Severity::Error
                             : events::Severity::Warning});
                    }

                    if (nanovdbBuildPolicy.failurePolicy == NanoVdbFailurePolicy::Fail)
                    {
                        throw NanoVdbBuildRejectedError(message);
                    }
                }
            }
        }
        return changedResources;
    }

    void Document::injectSmoothingKernel(std::string const & kernel)
    {
        m_core->injectSmoothingKernel(kernel);
    }

    nodes::BuildItems::iterator Document::addBuildItem(nodes::BuildItem && item)
    {
        m_buildItems.emplace_back(std::move(item));
        return std::prev(m_buildItems.end());
    }

    nodes::BuildItems const & Document::getBuildItems() const
    {
        return m_buildItems;
    }

    void Document::clearBuildItems()
    {
        m_buildItems.clear();

        m_assembly->assemblyModel()->clear();
        m_assembly->assemblyModel()->createBeginEndWithDefaultInAndOuts();
        m_assembly->assemblyModel()->setManaged(true);
    }

    void Document::replaceMeshResource(ResourceKey const & key, SharedMesh mesh)
    {
        // auto * res = getGeneratorContext().resourceManager.getResourcePtr(key);
        // TODO: Implement
    }

    std::optional<ResourceKey> Document::addMeshResource(std::filesystem::path const & filename)
    {
        vdb::VdbImporter reader;
        auto logger = getSharedLogger();
        try
        {
            reader.loadStl(filename);
            /* code */
        }
        catch (const std::exception & e)
        {
            auto logger = getSharedLogger();
            if (logger)
                logger->addEvent(
                  {std::string("STL load error: ") + e.what(), events::Severity::Error});
            return {};
        }

        auto mesh = reader.getMesh();
        return addMeshResource(std::move(mesh), filename.filename().string());
    }

    ResourceKey Document::addMeshResource(vdb::TriangleMesh && mesh, std::string const & name)
    {
        if (!m_3mfmodel)
        {
            throw std::runtime_error("No 3mf model loaded");
        }

        auto const new3mfMesh = m_3mfmodel->AddMeshObject();
        new3mfMesh->SetName(name);

        for (auto & vertex : mesh.vertices)
        {
            new3mfMesh->AddVertex({vertex.x(), vertex.y(), vertex.z()});
        }

        for (auto & triangle : mesh.indices)
        {
            new3mfMesh->AddTriangle({triangle[0], triangle[1], triangle[2]});
        }

        auto & resourceManager = getGeneratorContext().resourceManager;

        ResourceKey key = ResourceKey(new3mfMesh->GetModelResourceID(), ResourceType::Mesh);
        key.setDisplayName(name);

        // Build spatial mesh data using BVH for fast SDF queries
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        vertices.reserve(mesh.vertices.size());
        indices.reserve(mesh.indices.size());

        for (auto const & v : mesh.vertices)
        {
            vertices.push_back(float4{static_cast<float>(v.x()), static_cast<float>(v.y()), 
                                      static_cast<float>(v.z()), 0.0f});
        }

        for (auto const & tri : mesh.indices)
        {
            indices.push_back(TriangleIndices{static_cast<int>(tri[0]), 
                                              static_cast<int>(tri[1]), 
                                              static_cast<int>(tri[2])});
        }

        MeshBVHBuilder builder;
        auto spatialData = builder.build(vertices, indices);
        resourceManager.addResource(key, std::move(spatialData));

        resourceManager.loadResources();

        return key;
    }

    void Document::deleteResource(ResourceId id)
    {
        {

            m_assembly->deleteModel(id); // will just return if not a function
        }

        if (m_3mfmodel)
        {
            // NOTE: Keep in mind that id is a ModelResourceID, no a UniqueResourceID
            auto resIter = m_3mfmodel->GetResources();
            while (resIter->MoveNext())
            {
                auto resource = resIter->GetCurrent();
                if (resource->GetModelResourceID() == id)
                {
                    m_3mfmodel->RemoveResource(resource);
                    break;
                }
            }
        }
    }

    void Document::deleteResource(ResourceKey key)
    {
        auto id = key.getResourceId();
        if (!id)
        {
            return;
        }
        deleteResource(id.value());

        auto & resourceManager = getGeneratorContext().resourceManager;
        resourceManager.deleteResource(key);
    }

    void Document::deleteFunction(ResourceId id)
    {
        {

            m_assembly->deleteModel(id);
        }

        if (m_3mfmodel)
        {
            // NOTE: Keep in mind that id is a ModelResourceID, no a UniqueResourceID
            auto resIter = m_3mfmodel->GetResources();
            while (resIter->MoveNext())
            {
                auto resource = resIter->GetCurrent();
                if (resource->GetModelResourceID() == id)
                {
                    m_3mfmodel->RemoveResource(resource);
                    break;
                }
            }
        }
    }
    ResourceManager & Document::getResourceManager()
    {
        return getGeneratorContext().resourceManager;
    }

    ResourceManager const & Document::getResourceManager() const
    {
        return m_generatorContext->resourceManager;
    }

    void Document::addBoundingBoxAsMesh()
    {
        auto const bbox = computeBoundingBox();

        // create mesh from bounding box
        vdb::TriangleMesh mesh;

        // Top
        mesh.addTriangle({bbox.min.x, bbox.min.y, bbox.max.z},
                         {bbox.max.x, bbox.min.y, bbox.max.z},
                         {bbox.max.x, bbox.max.y, bbox.max.z});

        mesh.addTriangle({bbox.min.x, bbox.min.y, bbox.max.z},
                         {bbox.max.x, bbox.max.y, bbox.max.z},
                         {bbox.min.x, bbox.max.y, bbox.max.z});

        // Bottom
        mesh.addTriangle({bbox.min.x, bbox.min.y, bbox.min.z},
                         {bbox.max.x, bbox.min.y, bbox.min.z},
                         {bbox.max.x, bbox.max.y, bbox.min.z});

        mesh.addTriangle({bbox.min.x, bbox.min.y, bbox.min.z},
                         {bbox.max.x, bbox.max.y, bbox.min.z},
                         {bbox.min.x, bbox.max.y, bbox.min.z});

        // Front
        mesh.addTriangle({bbox.min.x, bbox.min.y, bbox.min.z},
                         {bbox.max.x, bbox.min.y, bbox.min.z},
                         {bbox.max.x, bbox.min.y, bbox.max.z});

        mesh.addTriangle({bbox.min.x, bbox.min.y, bbox.min.z},
                         {bbox.max.x, bbox.min.y, bbox.max.z},
                         {bbox.min.x, bbox.min.y, bbox.max.z});

        // Back
        mesh.addTriangle({bbox.min.x, bbox.max.y, bbox.min.z},
                         {bbox.max.x, bbox.max.y, bbox.min.z},
                         {bbox.max.x, bbox.max.y, bbox.max.z});

        mesh.addTriangle({bbox.min.x, bbox.max.y, bbox.min.z},
                         {bbox.max.x, bbox.max.y, bbox.max.z},
                         {bbox.min.x, bbox.max.y, bbox.max.z});

        // Left
        mesh.addTriangle({bbox.min.x, bbox.min.y, bbox.min.z},
                         {bbox.min.x, bbox.min.y, bbox.max.z},
                         {bbox.min.x, bbox.max.y, bbox.max.z});

        mesh.addTriangle({bbox.min.x, bbox.min.y, bbox.min.z},
                         {bbox.min.x, bbox.max.y, bbox.max.z},
                         {bbox.min.x, bbox.max.y, bbox.min.z});

        // Right
        mesh.addTriangle({bbox.max.x, bbox.min.y, bbox.min.z},
                         {bbox.max.x, bbox.min.y, bbox.max.z},
                         {bbox.max.x, bbox.max.y, bbox.max.z});

        mesh.addTriangle({bbox.max.x, bbox.min.y, bbox.min.z},
                         {bbox.max.x, bbox.max.y, bbox.max.z},
                         {bbox.max.x, bbox.max.y, bbox.min.z});

        addMeshResource(std::move(mesh), "bounding box");
    }

    void Document::addCustomBoxMesh(float width,
                                    float height,
                                    float depth,
                                    float startX,
                                    float startY,
                                    float startZ)
    {
        // Create a custom box mesh with user-defined dimensions
        vdb::TriangleMesh mesh;

        float minX = startX;
        float minY = startY;
        float minZ = startZ;
        float maxX = startX + width;
        float maxY = startY + height;
        float maxZ = startZ + depth;

        // Top (z = maxZ)
        mesh.addTriangle({minX, minY, maxZ}, {maxX, minY, maxZ}, {maxX, maxY, maxZ});

        mesh.addTriangle({minX, minY, maxZ}, {maxX, maxY, maxZ}, {minX, maxY, maxZ});

        // Bottom (z = minZ)
        mesh.addTriangle({minX, minY, minZ}, {maxX, minY, minZ}, {maxX, maxY, minZ});

        mesh.addTriangle({minX, minY, minZ}, {maxX, maxY, minZ}, {minX, maxY, minZ});

        // Front (y = minY)
        mesh.addTriangle({minX, minY, minZ}, {maxX, minY, minZ}, {maxX, minY, maxZ});

        mesh.addTriangle({minX, minY, minZ}, {maxX, minY, maxZ}, {minX, minY, maxZ});

        // Back (y = maxY)
        mesh.addTriangle({minX, maxY, minZ}, {maxX, maxY, minZ}, {maxX, maxY, maxZ});

        mesh.addTriangle({minX, maxY, minZ}, {maxX, maxY, maxZ}, {minX, maxY, maxZ});

        // Left (x = minX)
        mesh.addTriangle({minX, minY, minZ}, {minX, minY, maxZ}, {minX, maxY, maxZ});

        mesh.addTriangle({minX, minY, minZ}, {minX, maxY, maxZ}, {minX, maxY, minZ});

        // Right (x = maxX)
        mesh.addTriangle({maxX, minY, minZ}, {maxX, minY, maxZ}, {maxX, maxY, maxZ});

        mesh.addTriangle({maxX, minY, minZ}, {maxX, maxY, maxZ}, {maxX, maxY, minZ});

        std::string name = fmt::format("{}x{}x{} box", width, height, depth);
        addMeshResource(std::move(mesh), name);
    }

    void Document::addMeshAsBeamLattice(std::filesystem::path const & stlFilename, float beamRadius)
    {
        // Load the STL file
        vdb::VdbImporter reader;
        auto logger = getSharedLogger();

        try
        {
            reader.loadStl(stlFilename);
        }
        catch (const std::exception & e)
        {
            if (logger)
            {
                logger->addEvent({std::string("STL load error for beam lattice: ") + e.what(),
                                  events::Severity::Error});
            }
            return;
        }

        auto const & mesh = reader.getMesh();

        // Extract unique edges from the mesh triangles
        // Use a map to store unique edges (always store with smaller vertex index first)
        std::map<std::pair<size_t, size_t>, std::pair<openvdb::Vec3s, openvdb::Vec3s>> uniqueEdges;

        for (auto const & triangle : mesh.indices)
        {
            // Get the three vertices of the triangle
            auto const & v0 = mesh.vertices[triangle.x()];
            auto const & v1 = mesh.vertices[triangle.y()];
            auto const & v2 = mesh.vertices[triangle.z()];

            // Add the three edges of the triangle
            auto addEdge =
              [&](size_t idx1, size_t idx2, openvdb::Vec3s const & p1, openvdb::Vec3s const & p2)
            {
                // Always store edge with smaller index first to ensure uniqueness
                if (idx1 > idx2)
                {
                    std::swap(idx1, idx2);
                }
                uniqueEdges[{idx1, idx2}] = {p1, p2};
            };

            addEdge(triangle.x(), triangle.y(), v0, v1);
            addEdge(triangle.y(), triangle.z(), v1, v2);
            addEdge(triangle.z(), triangle.x(), v2, v0);
        }

        // Create beam data from unique edges
        std::vector<BeamData> beams;
        beams.reserve(uniqueEdges.size());

        for (auto const & [edgeIndices, edgePoints] : uniqueEdges)
        {
            BeamData beam{};

            // Set start and end positions
            beam.startPos =
              float4{{edgePoints.first.x(), edgePoints.first.y(), edgePoints.first.z(), 0.0f}};
            beam.endPos =
              float4{{edgePoints.second.x(), edgePoints.second.y(), edgePoints.second.z(), 0.0f}};

            // Set uniform radius for both ends
            beam.startRadius = beamRadius;
            beam.endRadius = beamRadius;

            // Set default cap styles (hemisphere = 0)
            beam.startCapStyle = 0;
            beam.endCapStyle = 0;

            // Default material ID
            beam.materialId = 0;
            beam.padding = 0;

            beams.push_back(beam);
        }

        // Create empty ball vector (no balls at vertices)
        std::vector<BallData> balls;

        // Configure ball mode as None
        BeamLatticeBallConfig ballConfig;
        ballConfig.mode = BallMode::None;
        ballConfig.defaultRadius = 0.0f;

        // Create a 3MF mesh object to hold the beam lattice
        if (!m_3mfmodel)
        {
            if (logger)
            {
                logger->addEvent({"No 3mf model loaded", events::Severity::Error});
            }
            return;
        }

        auto const new3mfMesh = m_3mfmodel->AddMeshObject();
        std::string resourceName =
          fmt::format("{} (beam lattice)", stlFilename.filename().string());
        new3mfMesh->SetName(resourceName);

        // Use the BeamLatticeExporter to properly export beam data to 3MF format
        io::BeamLatticeExporter exporter(logger);
        if (!exporter.exportToMeshObject(new3mfMesh, beams, balls, ballConfig))
        {
            if (logger)
            {
                logger->addEvent(
                  {"Failed to export beam lattice to 3MF format", events::Severity::Error});
            }
            return;
        }

        // Create resource key using the 3MF object's ID
        auto & resourceManager = getGeneratorContext().resourceManager;
        auto key = ResourceKey{new3mfMesh->GetModelResourceID(), ResourceType::BeamLattice};
        key.setDisplayName(resourceName);

        // Create beam lattice resource with BVH acceleration
        auto beamLatticeResource = std::make_unique<BeamLatticeResource>(
          key, std::move(beams), std::move(balls), ballConfig, BeamLatticeAcceleration::BVH);

        resourceManager.addResource(key, std::move(beamLatticeResource));
        resourceManager.loadResources();

        if (logger)
        {
            logger->addEvent({fmt::format("Created beam lattice with {} beams from {}",
                                          uniqueEdges.size(),
                                          stlFilename.filename().string()),
                              events::Severity::Info});
        }
    }

    ResourceKey Document::addImageStackResource(std::filesystem::path const & path)
    {
        auto result = addImageStackResourceWithPadding(path);
        return result.imageStack
                   ? ResourceKey{result.imageStack->GetModelResourceID(), ResourceType::ImageStack}
                   : ResourceKey{0, ResourceType::Unknown};
    }

    io::ImportResult Document::addImageStackResourceWithPadding(std::filesystem::path const & path)
    {
        io::ImageStackCreator creator;
        auto importResult = creator.importDirectoryWithPadding(get3mfModel(), path);

        if (!importResult.imageStack)
        {
            auto logger = getSharedLogger();
            if (logger)
            {
                logger->addEvent(
                    {fmt::format("Failed to import image stack from directory: {}", path.string()),
                     events::Severity::Error});
            }
            return importResult;
        }

        auto & resourceManager = getGeneratorContext().resourceManager;
        auto const key =
            ResourceKey{importResult.imageStack->GetModelResourceID(), ResourceType::ImageStack};

        io::ImageExtractor extractor;
        auto const files = creator.getFiles(path);

        // Detect pixel format from first file to choose import method
        io::PixelFormat pixelFormat = io::PixelFormat::GRAYSCALE_8BIT;
        if (!files.empty())
        {
            pixelFormat = extractor.determinePixelFormatFromFile(files.front());
        }

        if (pixelFormat == io::PixelFormat::GRAYSCALE_8BIT)
        {
            // Use VDB grid for grayscale 8-bit images
            auto grid = extractor.loadAsVdbGrid(files, io::FileLoaderType::Filesystem);
            resourceManager.addResource(key, std::move(grid));
        }
        else
        {
            // Use 3D texture for other formats (RGBA, RGB, etc.)
            auto stack = extractor.loadImageStack(files, io::FileLoaderType::Filesystem);
            resourceManager.addResource(key, std::move(stack));
        }

        resourceManager.loadResources();
        return importResult;
    }

    void Document::update3mfModel()
    {
        io::Writer3mf writer(getSharedLogger());
        writer.updateModel(*this);
    }

    void Document::updateDocumentFrom3mfModel(bool skipImplicitFunctions)
    {
        if (!m_3mfmodel)
        {
            throw std::runtime_error("No 3MF model available to update the document.");
        }

        io::Importer3mf importer{getSharedLogger()};
        importer.setMeshRepairConfig(m_meshRepairConfig);
        importer.setMeshSdfEvaluationConfig(m_meshSdfEvaluationConfig);
        importer.setNanoVdbBuildPolicy(getNanoVdbBuildPolicy());

        // Load build items from the 3MF model
        clearBuildItems();
        importer.loadBuildItems(m_3mfmodel, *this);

        // Load implicit functions from the 3MF model
        if (!skipImplicitFunctions)
        {
            importer.loadImplicitFunctions(m_3mfmodel, *this);

            m_assembly->updateInputsAndOutputs();
        }

        // Update the assembly inputs and outputs
    }

    void Document::rebuildResourceDependencyGraph()
    {

        if (!m_3mfmodel)
        {
            return;
        }

        m_resourceDependencyGraph =
          std::make_unique<io::ResourceDependencyGraph>(m_3mfmodel, getSharedLogger());
        m_resourceDependencyGraph->buildGraph();
    }

    io::CanResourceBeRemovedResult Document::isItSafeToDeleteResource(ResourceKey key)
    {
        io::CanResourceBeRemovedResult result;
        result.canBeRemoved = true;

        if (!m_3mfmodel)
        {
            return result;
        }

        auto modelResIdOpt = key.getResourceId();
        if (!modelResIdOpt)
        {
            return result;
        }

        // map model ResourceId to UniqueResourceID for graph lookup
        Lib3MF_uint32 uniqueResId =
          io::resourceIdToUniqueResourceId(m_3mfmodel, modelResIdOpt.value());
        try
        {
            auto resource = m_3mfmodel->GetResourceByID(uniqueResId);
            result = m_resourceDependencyGraph->checkResourceRemoval(resource);
            return result;
        }
        catch (const Lib3MF::ELib3MFException & e)
        {
            // Resource not found, return empty result
            auto logger = getSharedLogger();
            if (logger)
            {
                logger->addEvent(
                  {fmt::format("Resource not found: {}", e.what()), events::Severity::Error});
            }
        }
        catch (const std::exception & e)
        {

            // Resource not found, return empty result
            auto logger = getSharedLogger();
            if (logger)
            {
                logger->addEvent(
                  {fmt::format("Exception occurred: {}", e.what()), events::Severity::Error});
            }
            return result;
        }

        return result;
    }

    std::size_t Document::removeUnusedResources()
    {
        if (!m_3mfmodel || !m_resourceDependencyGraph)
        {
            auto logger = getSharedLogger();
            if (logger)
            {
                logger->addEvent({"Cannot remove unused resources: Model or resource dependency "
                                  "graph not available",
                                  events::Severity::Warning});
            }
            return 0;
        }

        // Sync internal node graph → 3MF model so the dependency graph reflects
        // any recent edits (e.g. new FunctionCall nodes created via MCP tools).
        update3mfModel();

        // Ensure the resource dependency graph is up-to-date
        rebuildResourceDependencyGraph();

        // Find all unused resources
        std::vector<Lib3MF::PResource> unusedResources =
          m_resourceDependencyGraph->findUnusedResources();

        if (unusedResources.empty())
        {
            auto logger = getSharedLogger();
            if (logger)
            {
                logger->addEvent(
                  {"No unused resources found in the model", events::Severity::Info});
            }
            return 0;
        }

        std::size_t removedCount = 0;
        auto & resourceManager = getGeneratorContext().resourceManager;

        // Remove each unused resource
        for (auto const & resource : unusedResources)
        {
            try
            {
                // Get the model resource ID for this resource
                Lib3MF_uint32 modelResourceId = resource->GetModelResourceID();
                ResourceKey key{modelResourceId, ResourceType::Unknown};

                // Check if this is actually a function (need to handle differently)
                bool isFunction = false;
                try
                {
                    auto function = std::dynamic_pointer_cast<Lib3MF::CFunction>(resource);
                    if (function)
                    {
                        isFunction = true;
                        deleteFunction(modelResourceId);
                    }
                }
                catch (const std::exception &)
                {
                    // Not a function, continue with normal resource deletion
                }

                if (!isFunction)
                {
                    // Delete from resource manager if it exists there
                    if (resourceManager.hasResource(key))
                    {
                        resourceManager.deleteResource(key);
                    }

                    // Delete the resource from the 3MF model
                    m_3mfmodel->RemoveResource(resource);
                }

                removedCount++;
            }
            catch (const std::exception & e)
            {
                auto logger = getSharedLogger();
                if (logger)
                {
                    logger->addEvent({fmt::format("Failed to remove unused resource: {}", e.what()),
                                      events::Severity::Error});
                }
            }
        }

        if (removedCount > 0)
        {
            auto logger = getSharedLogger();
            if (logger)
            {
                logger->addEvent(
                  {fmt::format("Successfully removed {} unused resources", removedCount),
                   events::Severity::Info});
            }
            markFileAsChanged();

            // Rebuild the dependency graph now that resources have been removed
            rebuildResourceDependencyGraph();
        }

        return removedCount;
    }

    std::vector<Lib3MF::PResource> Document::findUnusedResources()
    {
        if (!m_3mfmodel || !m_resourceDependencyGraph)
        {
            auto logger = getSharedLogger();
            if (logger)
            {
                logger->addEvent(
                  {"Cannot find unused resources: Model or resource dependency graph not available",
                   events::Severity::Warning});
            }
            return {};
        }

        // Ensure the resource dependency graph is up-to-date
        rebuildResourceDependencyGraph();

        // Find all unused resources
        return m_resourceDependencyGraph->findUnusedResources();
    }

    const gladius::io::ResourceDependencyGraph * Document::getResourceDependencyGraph() const
    {
        return m_resourceDependencyGraph.get();
    }

    void Document::markValidationDirty()
    {
        m_validationDirty = true;
    }

    bool Document::validateAssemblyIfDirty(nodes::ValidationContext context)
    {
        if (!m_validationDirty)
        {
            return !m_issueList.hasErrors();
        }
        ZoneScopedN("ValidateAssemblyIfDirty");
        m_validationDirty = false;
        return validateAssembly(context);
    }

    bool Document::validateAssembly(nodes::ValidationContext context)
    {
        nodes::Validator validator;
        auto logger = getSharedLogger();

        m_issueList.clear();

        if (!validator.validate(*m_assembly, m_issueList))
        {
            // Only log events for non-interactive contexts (API usage, file loading)
            if (context != nodes::ValidationContext::Interactive && logger)
            {
                for (auto const & issue : m_issueList.getAll())
                {
                    logger->addEvent({fmt::format("{}: Review parameter {} of node {} in model {}",
                                                  issue.message,
                                                  issue.parameter,
                                                  issue.node,
                                                  issue.model),
                                      events::Severity::Error});
                }
            }
            return false;
        }

        return true;
    }

    nodes::IssueList& Document::getIssueList()
    {
        return m_issueList;
    }

    nodes::IssueList const& Document::getIssueList() const
    {
        return m_issueList;
    }

    BackupManager & Document::getBackupManager()
    {
        return m_backupManager;
    }

    const BackupManager & Document::getBackupManager() const
    {
        return m_backupManager;
    }

    void Document::setUiMode(bool uiMode)
    {
        m_uiMode = uiMode;
    }

    bool Document::isUiMode() const
    {
        return m_uiMode;
    }

    void Document::signalStructuralEdit()
    {
        m_structuralEditEpoch.fetch_add(1, std::memory_order_release);
        m_structuralDebouncer.pending.store(true, std::memory_order_relaxed);
        m_structuralDebouncer.lastEditTime = std::chrono::steady_clock::now();
    }

    bool Document::hasStructuralEditPending() const
    {
        return m_structuralDebouncer.pending.load(std::memory_order_relaxed);
    }

    uint64_t Document::structuralEditEpoch() const
    {
        return m_structuralEditEpoch.load(std::memory_order_acquire);
    }

    bool Document::dispatchStructuralUpdateIfReady()
    {
        if (!m_structuralDebouncer.pending.load(std::memory_order_relaxed))
        {
            return false;
        }

        auto const now = std::chrono::steady_clock::now();
        auto const elapsed = now - m_structuralDebouncer.lastEditTime;
        if (elapsed < m_structuralDebouncer.debounceDelay)
        {
            return false;
        }

        m_structuralDebouncer.pending.store(false, std::memory_order_relaxed);
        return dispatchStructuralUpdate();
    }

    bool Document::dispatchStructuralUpdate()
    {
        if (!prepareAssemblyForRefresh())
        {
            return false;
        }

        if (refreshModelIfNoCompilationIsRunning())
        {
            return true;
        }
        // A compilation is already in progress — re-arm the debouncer so we
        // retry on the next frame once the current run finishes.
        m_structuralDebouncer.pending.store(true, std::memory_order_relaxed);
        m_structuralDebouncer.lastEditTime = std::chrono::steady_clock::now();
        return false;
    }
}
