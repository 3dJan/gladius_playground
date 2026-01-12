#include "ManifoldDualContouringStlExporter.h"

#include "3mf/FaceColorSampler.h"
#include "3mf/MeshWriter3mf.h"
#include "MeshExporter.h"
#include "MeshExporter3mf.h"
#include "WindingRepair.h"

#include "ComputeContext.h"
#include "ComputeCore.h"
#include "../compute/ManifoldDualContouringGpu.h"
#include "../compute/ProgramManager.h"
#include "../types.h"

#include <Eigen/Geometry>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

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

        [[nodiscard]] Vector3 toVector3(Eigen::Vector3f const & value)
        {
            return Vector3{value.x(), value.y(), value.z()};
        }

        // EdgeKey and EdgeKeyHash kept for the 3MF face-color export path
        struct EdgeKey
        {
            std::uint32_t a{0U};
            std::uint32_t b{0U};

            [[nodiscard]] bool operator==(EdgeKey const & other) const noexcept
            {
                return a == other.a && b == other.b;
            }
        };

        struct EdgeKeyHash
        {
            [[nodiscard]] std::size_t operator()(EdgeKey const & key) const noexcept
            {
                return (static_cast<std::size_t>(key.a) << 32U) ^ static_cast<std::size_t>(key.b);
            }
        };
    }

    ManifoldDualContouringStlExporter::ManifoldDualContouringStlExporter() = default;

    ManifoldDualContouringStlExporter::ManifoldDualContouringStlExporter(
      events::SharedLogger logger)
        : m_logger(std::move(logger))
    {
    }

    void ManifoldDualContouringStlExporter::setOptions(ManifoldDualContouringOptions options)
    {
        m_options = std::move(options);
    }

    void ManifoldDualContouringStlExporter::setOutputFormat(MeshOutputFileFormat format)
    {
        m_outputFormat = format;
    }

    void ManifoldDualContouringStlExporter::setDocument(Document const * doc)
    {
        m_document = doc;
    }
    
    void ManifoldDualContouringStlExporter::setExportWithColors(bool exportWithColors)
    {
        m_exportWithColors = exportWithColors;
    }
    
    void ManifoldDualContouringStlExporter::setConvertToSrgb(bool convertToSrgb)
    {
        m_convertToSrgb = convertToSrgb;
    }

    void ManifoldDualContouringStlExporter::setColorMode(ColorMode mode)
    {
        m_colorMode = mode;
    }

    void ManifoldDualContouringStlExporter::beginExport(std::filesystem::path const & fileName,
                                                        ComputeCore & generator)
    {
        m_targetFile = fileName;
        m_computeCore = &generator;
        m_state = State::Running;
        m_progress = 0.0;
        m_errorMessage.clear();
        
        // Launch export in background thread
        m_exportFuture = std::async(std::launch::async, [this, &generator]() {
            try
            {
                performExport(generator);
                m_state = State::Completed;
                m_progress = 1.0;
            }
            catch (std::exception const & ex)
            {
                m_state = State::Failed;
                m_errorMessage = ex.what();
                m_progress = 1.0;
                if (m_logger)
                {
                    m_logger->addEvent({fmt::format("Manifold dual contouring export failed: {}",
                                                     ex.what()),
                                        events::Severity::Error});
                }
            }
        });
    }

    bool ManifoldDualContouringStlExporter::advanceExport(ComputeCore & /*generator*/)
    {
        // Check if background thread has finished
        if (m_state == State::Completed || m_state == State::Failed || m_state == State::Idle)
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

    void ManifoldDualContouringStlExporter::finalize()
    {
        // Wait for background thread to complete if still running
        if (m_exportFuture.valid())
        {
            m_exportFuture.wait();
        }
        
        m_computeCore = nullptr;
        m_state = State::Idle;
        m_progress = clampProgress(m_progress.load());
    }

    double ManifoldDualContouringStlExporter::getProgress() const
    {
        return clampProgress(m_progress.load());
    }

    bool ManifoldDualContouringStlExporter::hasError() const
    {
        return m_state.load() == State::Failed;
    }

    std::string const & ManifoldDualContouringStlExporter::errorMessage() const
    {
        return m_errorMessage;
    }

    void ManifoldDualContouringStlExporter::performExport(ComputeCore & generator)
    {
        if (m_targetFile.empty())
        {
            throw std::runtime_error("No output filename specified for STL export");
        }

        if (!generator.updateBBox())
        {
            throw std::runtime_error(
              "Computing bounding box failed. The model has probably not been compiled yet");
        }

        auto const boundingBox = generator.getBoundingBox();
        if (!boundingBox.has_value())
        {
            throw std::runtime_error("Mesh generation failed, bounding box is empty");
        }

        // Check for cancellation after bbox computation
        if (isCancellationRequested())
        {
            m_state = State::Idle;
            return;
        }

        m_progress = 0.05;

        compute::ManifoldDualContouringGpu gpuPipeline(generator);
        compute::ManifoldDualContouringConfig config{};
        config.initialDepth = m_options.initialDepth;
        config.maxDepth = m_options.maxDepth;
        config.enableGpu = m_options.enableGpu;
        config.enableCpuFallback = m_options.enableCpuFallback;
        config.enableCaching = m_options.enableCaching;
        config.isoValue = m_options.isoValue;
        config.minFeatureSize = m_options.minFeatureSize;
        config.enableChunking = m_options.enableChunking;
        config.enableHierarchicalOctree = m_options.enableHierarchicalOctree;
        config.enableSharpFeaturePostProcess = m_options.enableSharpFeaturePostProcess;
        config.sharpFeatureAngleThreshold = m_options.sharpFeatureAngleThreshold;
        config.subdivisionIterations = m_options.subdivisionIterations;
        config.projectToSurface = m_options.projectToSurface;
        // Simplification method mapping
        switch (m_options.simplificationMethod)
        {
            case SimplificationMethod::None:
                config.simplificationMethod = compute::SimplificationMethod::None;
                break;
            case SimplificationMethod::QemSdfAware:
                config.simplificationMethod = compute::SimplificationMethod::QemSdfAware;
                break;
        }
        config.enableSimplification = m_options.enableSimplification;  // Legacy support
        config.simplificationMaxSdfError = m_options.simplificationMaxSdfError;
        config.simplificationMaxQemError = m_options.simplificationMaxQemError;
        config.simplificationMaxNormalDeviation = m_options.simplificationMaxNormalDeviation;
        config.simplificationSdfWeight = m_options.simplificationSdfWeight;
        config.simplificationQemWeight = m_options.simplificationQemWeight;
        config.simplificationNormalWeight = m_options.simplificationNormalWeight;
        config.simplificationSharpEdgeThreshold = m_options.simplificationSharpEdgeThreshold;
        config.simplificationBatchSize = m_options.simplificationBatchSize;
        config.simplificationMaxPasses = m_options.simplificationMaxPasses;
        config.simplificationTargetTriangles = m_options.simplificationTargetTriangles;
        config.simplificationTargetReduction = m_options.simplificationTargetReduction;
        gpuPipeline.setConfig(std::move(config));
        
        // Wire progress callback to update exporter's atomic progress
        // Mesh generation phase spans 5-80% of total export progress
        gpuPipeline.setMeshGenerationProgressCallback(
            [this](float meshProgress, std::string_view /*phaseName*/) {
                // Map mesh generation progress (0.0-1.0) to exporter progress (0.05-0.80)
                // This gives mesh generation 75% of the total export progress
                double const exportProgress = 0.05 + static_cast<double>(meshProgress) * 0.75;
                m_progress.store(exportProgress, std::memory_order_relaxed);
            });
        
        // Wire cancellation callback so the GPU pipeline can check frequently
        gpuPipeline.setCancellationCheckCallback(
            [this]() -> bool {
                return isCancellationRequested();
            });
        
        gpuPipeline.generateMesh();

        // Check for cancellation after mesh generation (most expensive step)
        if (isCancellationRequested())
        {
            m_state = State::Idle;
            return;
        }

        auto const & mesh = gpuPipeline.getMesh();
        if (mesh.indices.empty())
        {
            throw std::runtime_error("Manifold dual contouring produced an empty mesh");
        }

        m_progress = 0.80;

        // Check for cancellation before file write
        if (isCancellationRequested())
        {
            m_state = State::Idle;
            return;
        }

        writeMeshToFile(generator, mesh.positions, mesh.indices, mesh.normals);

        if (m_logger)
        {
            m_logger->addEvent(
              {fmt::format("Manifold dual contouring STL export completed: {}",
                           m_targetFile.string()),
               events::Severity::Info});
        }
    }

    void ManifoldDualContouringStlExporter::writeMeshToFile(
      ComputeCore & generator,
      std::vector<Eigen::Vector3f> const & positions,
      std::vector<std::uint32_t> const & indices,
      std::vector<Eigen::Vector3f> const & normals)
    {
        if (indices.size() % 3U != 0U)
        {
            throw std::runtime_error("Manifold dual contouring mesh has incomplete triangles");
        }

        if (positions.empty())
        {
            throw std::runtime_error("Manifold dual contouring produced no vertices");
        }

        auto computeContext = generator.getComputeContext();
        if (computeContext == nullptr)
        {
            throw std::runtime_error("Compute context unavailable for mesh export");
        }

        m_progress.store(0.82, std::memory_order_relaxed);

        // Repair winding inconsistencies on the indexed mesh before converting to triangle soup.
        // This prevents slicers from interpreting an otherwise watertight mesh as broken.
        std::vector<std::uint32_t> repairedIndices = indices;
        WindingRepairStats const repairStats = repairTriangleWindingConsistency(positions, repairedIndices);
        if (m_logger)
        {
            m_logger->addEvent({fmt::format(
                                  "Winding repair: triangles={}, components={}, flippedTriangles={}, globalFlip={}, inconsistency={} (constraints={})",
                                  repairStats.triangleCount,
                                  repairStats.components,
                                  repairStats.flippedTriangles,
                                  repairStats.flippedGlobalOrientation ? 1 : 0,
                                  repairStats.hadInconsistency ? 1 : 0,
                                  repairStats.adjacencyConstraints),
                                events::Severity::Info});
        }

        Mesh convertedMesh(*computeContext);

        for (std::size_t i = 0U; i + 2U < repairedIndices.size(); i += 3U)
        {
            auto const idxA = static_cast<std::size_t>(repairedIndices[i + 0U]);
            auto const idxB = static_cast<std::size_t>(repairedIndices[i + 1U]);
            auto const idxC = static_cast<std::size_t>(repairedIndices[i + 2U]);

            if (idxA >= positions.size() || idxB >= positions.size() || idxC >= positions.size())
            {
                throw std::runtime_error(
                  "Manifold dual contouring mesh references out-of-range vertex indices");
            }

            Eigen::Vector3f const a = positions[idxA];
            Eigen::Vector3f const b = positions[idxB];
            Eigen::Vector3f const c = positions[idxC];

            Eigen::Vector3f normal = (b - a).cross(c - a);
            if (normal.squaredNorm() <= 1e-12F)
            {
                if (idxA < normals.size())
                {
                    normal = normals[idxA];
                }
                else
                {
                    normal = Eigen::Vector3f{0.0F, 0.0F, 1.0F};
                }
            }
            else
            {
                normal.normalize();
            }

            Face faceData{};
            faceData.normal = toVector3(normal);
            faceData.vertices = {toVector3(a), toVector3(b), toVector3(c)};
            if (idxA < normals.size() && idxB < normals.size() && idxC < normals.size())
            {
                faceData.vertexNormals = {
                  toVector3(normals[idxA]), toVector3(normals[idxB]), toVector3(normals[idxC])};
            }
            else
            {
                faceData.vertexNormals = {faceData.normal, faceData.normal, faceData.normal};
            }

            convertedMesh.addFace(faceData);
        }

        if (convertedMesh.getNumberOfFaces() == 0U)
        {
            throw std::runtime_error("Manifold dual contouring produced no valid faces");
        }

        // Write to the appropriate format
        if (m_outputFormat == MeshOutputFileFormat::ThreeMF)
        {
            MeshWriter3mf writer(m_logger);
            
            if (m_exportWithColors)
            {
                m_progress.store(0.85, std::memory_order_relaxed);
                
                // Build faces array from indices
                std::size_t const numFaces = repairedIndices.size() / 3;
                std::vector<std::array<std::uint32_t, 3>> facesForSampling;
                facesForSampling.reserve(numFaces);
                for (std::size_t i = 0; i < repairedIndices.size(); i += 3)
                {
                    facesForSampling.push_back({repairedIndices[i], repairedIndices[i + 1], repairedIndices[i + 2]});
                }
                
                // Sample colors using GPU
                auto* samplingProgram = generator.getProgramManager().getDualContouringSamplingProgram();
                auto primitives = generator.getPrimitives();
                
                if (samplingProgram != nullptr && primitives != nullptr)
                {
                    if (m_colorMode == ColorMode::PerVertex)
                    {
                        auto vertexColors = FaceColorSampler::sampleVertexColors(
                            positions, facesForSampling, *samplingProgram, *primitives, nullptr, m_convertToSrgb);
                        
                        m_progress.store(0.90, std::memory_order_relaxed);
                        writer.exportMeshWithVertexColors(m_targetFile, convertedMesh, "Mesh", vertexColors, m_document, true);
                        m_progress.store(1.0, std::memory_order_relaxed);
                        
                        if (m_logger)
                        {
                            m_logger->addEvent(
                              {fmt::format("Exported 3MF mesh with per-vertex colors"),
                               events::Severity::Info});
                        }
                    }
                    else
                    {
                        auto faceColors = FaceColorSampler::sampleFaceColorsAsColor8(
                            positions, facesForSampling, *samplingProgram, *primitives, nullptr, m_convertToSrgb);
                        
                        m_progress.store(0.90, std::memory_order_relaxed);
                        writer.exportMeshWithColors(m_targetFile, convertedMesh, "Mesh", faceColors, m_document, true);
                        m_progress.store(1.0, std::memory_order_relaxed);
                        
                        if (m_logger)
                        {
                            m_logger->addEvent(
                              {fmt::format("Exported 3MF mesh with {} face colors", faceColors.colors.size()),
                               events::Severity::Info});
                        }
                    }
                }
                else
                {
                    // Fallback to non-colored export if sampling is unavailable
                    if (m_logger)
                    {
                        m_logger->addEvent(
                          {"Color sampling unavailable, exporting without colors",
                           events::Severity::Warning});
                    }
                    m_progress.store(0.90, std::memory_order_relaxed);
                    writer.exportMesh(m_targetFile, convertedMesh, "Mesh", m_document, true);
                    m_progress.store(1.0, std::memory_order_relaxed);
                }
            }
            else
            {
                m_progress.store(0.90, std::memory_order_relaxed);
                writer.exportMesh(m_targetFile, convertedMesh, "Mesh", m_document, true);
                m_progress.store(1.0, std::memory_order_relaxed);
            }
        }
        else
        {
            m_progress.store(0.90, std::memory_order_relaxed);
            vdb::exportMeshToSTL(convertedMesh, m_targetFile);
            m_progress.store(1.0, std::memory_order_relaxed);
        }
    }
}
