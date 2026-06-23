#include "ShellExporter.h"

#include "3mf/MeshWriter3mf.h"
#include "3mf/OpenVdbShellGenerator.h"
#include "3mf/ShellGenerator.h"
#include "ComputeContext.h"
#include "ComputeCore.h"
#include "Document.h"
#include "Mesh.h"
#include "WindingRepair.h"

#include <fmt/format.h>

#include <chrono>
#include <utility>

namespace gladius::io
{
    namespace
    {
        [[nodiscard]] double clampProgress(double value)
        {
            if (value < 0.0)
            {
                return 0.0;
            }
            if (value > 1.0)
            {
                return 1.0;
            }
            return value;
        }
    }

    ShellExporter::ShellExporter() = default;

    ShellExporter::ShellExporter(events::SharedLogger logger)
        : m_logger(std::move(logger))
    {
    }

    void ShellExporter::setConfig(ShellExportConfig config)
    {
        m_config = std::move(config);
    }

    void ShellExporter::setDocument(Document const* doc)
    {
        m_document = doc;
    }

    void ShellExporter::beginExport(std::filesystem::path const& fileName, ComputeCore& generator)
    {
        if (m_config.filamentStack.empty())
        {
            throw std::runtime_error("Shell export requires at least one material");
        }

        m_targetFile = fileName;
        m_state.store(State::Running, std::memory_order_release);
        m_progress.store(0.0, std::memory_order_release);
        m_errorMessage.clear();

        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = "Starting shell export...";
        }

        m_exportFuture = std::async(std::launch::async, [this, &generator]() {
            try
            {
                performExport(generator);
                if (m_state.load(std::memory_order_acquire) == State::Running)
                {
                    m_state.store(State::Completed, std::memory_order_release);
                }
            }
            catch (std::exception const& e)
            {
                m_errorMessage = e.what();
                m_state.store(State::Failed, std::memory_order_release);
                if (m_logger)
                {
                    m_logger->addEvent({fmt::format("Shell export failed: {}", e.what()),
                                        events::Severity::Error});
                }
            }
        });
    }

    bool ShellExporter::advanceExport(ComputeCore& /*generator*/)
    {
        // Check if background thread has finished
        State const state = m_state.load(std::memory_order_acquire);
        if (state == State::Completed || state == State::Failed || state == State::Idle)
        {
            return false;
        }

        // Check if future is ready (non-blocking)
        if (m_exportFuture.valid())
        {
            auto status = m_exportFuture.wait_for(std::chrono::milliseconds(0));
            if (status == std::future_status::ready)
            {
                // Get any exceptions from the future
                try
                {
                    m_exportFuture.get();
                }
                catch (...)
                {
                    // Exception already handled in the async lambda
                }
                return false; // Export finished
            }
        }

        return true; // Still running
    }

    void ShellExporter::finalize()
    {
        // Wait for background thread to complete if still running
        if (m_exportFuture.valid())
        {
            m_exportFuture.wait();
        }

        m_state.store(State::Idle, std::memory_order_release);
        m_progress.store(clampProgress(m_progress.load()), std::memory_order_release);
    }

    double ShellExporter::getProgress() const
    {
        return clampProgress(m_progress.load(std::memory_order_acquire));
    }

    bool ShellExporter::hasError() const
    {
        return m_state.load(std::memory_order_acquire) == State::Failed;
    }

    std::string const& ShellExporter::errorMessage() const
    {
        return m_errorMessage;
    }

    std::string ShellExporter::getStatusMessage() const
    {
        std::lock_guard lock(m_statusMutex);
        return m_statusMessage;
    }

    void ShellExporter::performExport(ComputeCore& generator)
    {
        // Phase 1: Initialization (0% - 5%)
        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = "Initializing shell export...";
        }
        m_progress.store(0.02, std::memory_order_release);

        if (isCancellationRequested())
        {
            m_state.store(State::Idle, std::memory_order_release);
            return;
        }

        generator.updateBBox();

        // Build thickness solution from LUTs
        std::size_t const numLayers = m_config.filamentStack.size();
        ThicknessSolution solution(numLayers);
        for (std::size_t i = 0; i < numLayers; ++i)
        {
            float thickness = m_config.thicknessConstraints.minThickness;
            if (i < m_config.precomputedLuts.size() && !m_config.precomputedLuts[i].empty())
            {
                int const res = std::max(2, m_config.lutResolution);
                std::size_t const idx =
                    (static_cast<std::size_t>(res - 1) * static_cast<std::size_t>(res) +
                     static_cast<std::size_t>(res - 1)) * static_cast<std::size_t>(res) +
                    static_cast<std::size_t>(res - 1);
                if (idx < m_config.precomputedLuts[i].size())
                {
                    thickness = m_config.precomputedLuts[i][idx];
                }
            }
            solution.thicknesses[i] = thickness;
        }

        m_progress.store(0.05, std::memory_order_release);

        auto const shellIntervals = ShellThicknessPartition::buildIntervals(solution);
        if (shellIntervals.empty())
        {
            throw std::runtime_error("Shell export requires at least one non-zero shell thickness");
        }

        if (isCancellationRequested())
        {
            m_state.store(State::Idle, std::memory_order_release);
            return;
        }

        // Phase 2: Shell generation (5% - 85%)
        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = fmt::format("Generating {} shells...", shellIntervals.size());
        }

        std::vector<ShellGenerator::ShellMesh> shells;
        if (m_config.generationMode == ShellGenerationMode::OpenVdbColorThickness)
        {
            OpenVdbShellGenerator shellGenerator(generator);
            if (m_config.useSurfaceColorSampling)
            {
                shells = shellGenerator.generateSurfaceDrivenShells(
                    m_config.filamentStack,
                    m_config.mdcOptions,
                    m_config.lutResolution,
                    m_config.thicknessConstraints,
                    [this]() { return isCancellationRequested(); });
            }
            else
            {
                shells = shellGenerator.generateUniformShells(
                    m_config.filamentStack,
                    solution,
                    m_config.mdcOptions,
                    [this]() { return isCancellationRequested(); });
            }
        }
        else
        {
            ShellGenerator shellGenerator(generator, *const_cast<Document*>(m_document));
            shells = shellGenerator.generateShells(
                m_config.filamentStack,
                solution,
                m_config.mdcOptions,
                m_config.lutResolution,
                m_config.thicknessConstraints,
                &m_config.precomputedLuts,
                m_config.useSurfaceColorSampling);
        }

        if (isCancellationRequested())
        {
            m_state.store(State::Idle, std::memory_order_release);
            return;
        }

        if (shells.empty())
        {
            throw std::runtime_error("Shell generation produced no meshes");
        }

        m_progress.store(0.85, std::memory_order_release);

        // Phase 3: Mesh construction (85% - 95%)
        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = "Building mesh data...";
        }

        ComputeContext* context = generator.getComputeContext().get();
        std::vector<std::tuple<std::shared_ptr<Mesh>, std::string, Eigen::Vector3f>> meshesWithColors;
        meshesWithColors.reserve(shells.size());

        std::size_t shellIdx = 0;
        for (auto& shell : shells)
        {
            if (isCancellationRequested())
            {
                m_state.store(State::Idle, std::memory_order_release);
                return;
            }

            if (m_logger)
            {
                m_logger->addEvent({
                    fmt::format("Processing shell {}: L{}_{}, {} vertices, {} indices",
                                shellIdx, shell.layerIndex, shell.filamentName,
                                shell.vertices.size(), shell.indices.size()),
                    events::Severity::Info});
            }

            // Apply winding repair to ensure consistent triangle orientation
            WindingRepairStats const repairStats = 
                repairTriangleWindingConsistency(shell.vertices, shell.indices);
            
            if (m_logger)
            {
                m_logger->addEvent({
                    fmt::format("Winding repair for shell {}: triangles={}, components={}, "
                                "flipped={}, globalFlip={}, inconsistency={}",
                                shellIdx,
                                repairStats.triangleCount,
                                repairStats.components,
                                repairStats.flippedTriangles,
                                repairStats.flippedGlobalOrientation ? 1 : 0,
                                repairStats.hadInconsistency ? 1 : 0),
                    events::Severity::Info});
            }

            auto mesh = std::make_shared<Mesh>(*context);
            std::size_t skippedFaces = 0;

            for (std::size_t idx = 0; idx + 2 < shell.indices.size(); idx += 3)
            {
                auto const i0 = shell.indices[idx + 0];
                auto const i1 = shell.indices[idx + 1];
                auto const i2 = shell.indices[idx + 2];

                if (i0 >= shell.vertices.size() || i1 >= shell.vertices.size() ||
                    i2 >= shell.vertices.size())
                {
                    ++skippedFaces;
                    continue;
                }

                mesh->addFace(shell.vertices[i0], shell.vertices[i1], shell.vertices[i2]);
            }

            mesh->write();
            std::string const name = fmt::format("Shell_L{}_{}", shell.layerIndex, shell.filamentName);

            if (m_logger)
            {
                m_logger->addEvent({
                    fmt::format("Built mesh '{}': {} faces, {} skipped due to invalid indices",
                                name, mesh->getNumberOfFaces(), skippedFaces),
                    events::Severity::Info});
            }

            // Lookup material color from stack
            Eigen::Vector3f color{1.0F, 1.0F, 1.0F};
            if (static_cast<std::size_t>(shell.layerIndex) < m_config.filamentStack.size())
            {
                color = m_config.filamentStack[static_cast<std::size_t>(shell.layerIndex)].reflectanceColor;
            }

            meshesWithColors.emplace_back(std::move(mesh), name, color);
            ++shellIdx;
        }

        m_progress.store(0.95, std::memory_order_release);

        if (isCancellationRequested())
        {
            m_state.store(State::Idle, std::memory_order_release);
            return;
        }

        if (m_logger)
        {
            m_logger->addEvent({
                fmt::format("Shell generation complete: {} shells generated, {} meshes to export",
                            shells.size(), meshesWithColors.size()),
                events::Severity::Info});
        }

        // Phase 4: Write 3MF (95% - 100%)
        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = "Writing 3MF file...";
        }

        MeshWriter3mf writer(m_logger);
        writer.exportMeshesWithMaterialColors(m_targetFile, meshesWithColors, m_document, true);

        m_progress.store(1.0, std::memory_order_release);

        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = "Export complete";
        }

        if (m_logger)
        {
            m_logger->addEvent({
                fmt::format("Exported {} shell meshes to {}", meshesWithColors.size(), m_targetFile.string()),
                events::Severity::Info});
        }
    }

} // namespace gladius::io
