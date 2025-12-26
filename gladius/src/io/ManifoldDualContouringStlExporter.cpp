#include "ManifoldDualContouringStlExporter.h"

#include "3mf/FaceColorSampler.h"
#include "3mf/MeshWriter3mf.h"
#include "MeshExporter.h"
#include "MeshExporter3mf.h"

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

        struct EdgeRef
        {
            std::uint32_t a{0U};
            std::uint32_t b{0U};
            std::uint32_t triId{0U};
            std::uint8_t dir{0U}; // 0: min->max, 1: max->min
        };

        [[nodiscard]] double computeSignedVolume(
          std::vector<Eigen::Vector3f> const & positions,
          std::vector<std::uint32_t> const & indices)
        {
            if (indices.size() < 3U)
            {
                return 0.0;
            }

            double volume6 = 0.0;
            for (std::size_t i = 0U; i + 2U < indices.size(); i += 3U)
            {
                std::size_t const ia = static_cast<std::size_t>(indices[i + 0U]);
                std::size_t const ib = static_cast<std::size_t>(indices[i + 1U]);
                std::size_t const ic = static_cast<std::size_t>(indices[i + 2U]);
                if (ia >= positions.size() || ib >= positions.size() || ic >= positions.size())
                {
                    continue;
                }

                Eigen::Vector3d const a = positions[ia].cast<double>();
                Eigen::Vector3d const b = positions[ib].cast<double>();
                Eigen::Vector3d const c = positions[ic].cast<double>();
                volume6 += a.dot(b.cross(c));
            }
            return volume6 / 6.0;
        }

        struct WindingRepairStats
        {
            std::size_t triangleCount{0U};
            std::size_t adjacencyConstraints{0U};
            std::size_t components{0U};
            std::size_t flippedTriangles{0U};
            bool hadInconsistency{false};
            bool flippedGlobalOrientation{false};
        };

        /// Ensures local winding consistency across shared edges by flipping triangles.
        ///
        /// This targets the class of defects where the mesh is topologically closed in an
        /// *undirected* sense (each edge has exactly two incident triangles) but triangles
        /// have inconsistent orientation, causing oriented-edge conflicts that many slicers
        /// may report as "open" or "broken" edges.
        [[nodiscard]] WindingRepairStats repairTriangleWindingConsistency(
          std::vector<Eigen::Vector3f> const & positions,
          std::vector<std::uint32_t> & indices)
        {
            WindingRepairStats stats;
            if (indices.size() < 3U || (indices.size() % 3U) != 0U)
            {
                return stats;
            }

            std::size_t const triCount = indices.size() / 3U;
            stats.triangleCount = triCount;

            std::vector<EdgeRef> edgeRefs;
            edgeRefs.reserve(triCount * 3U);

            for (std::size_t triId = 0U; triId < triCount; ++triId)
            {
                std::uint32_t const i0 = indices[triId * 3U + 0U];
                std::uint32_t const i1 = indices[triId * 3U + 1U];
                std::uint32_t const i2 = indices[triId * 3U + 2U];
                if (i0 == i1 || i1 == i2 || i2 == i0)
                {
                    continue;
                }

                auto pushEdge = [&edgeRefs, triId](std::uint32_t from, std::uint32_t to)
                {
                    if (from == to)
                    {
                        return;
                    }

                    std::uint32_t const lo = std::min(from, to);
                    std::uint32_t const hi = std::max(from, to);
                    std::uint8_t const dir = (from == lo) ? 0U : 1U;
                    edgeRefs.push_back(EdgeRef{lo, hi, static_cast<std::uint32_t>(triId), dir});
                };

                pushEdge(i0, i1);
                pushEdge(i1, i2);
                pushEdge(i2, i0);
            }

            if (edgeRefs.empty())
            {
                return stats;
            }

            std::sort(edgeRefs.begin(),
                      edgeRefs.end(),
                      [](EdgeRef const & lhs, EdgeRef const & rhs)
                      {
                          if (lhs.a != rhs.a)
                          {
                              return lhs.a < rhs.a;
                          }
                          if (lhs.b != rhs.b)
                          {
                              return lhs.b < rhs.b;
                          }
                          return lhs.triId < rhs.triId;
                      });

            std::vector<std::array<std::uint32_t, 3>> neighbors(triCount, {
              std::numeric_limits<std::uint32_t>::max(),
              std::numeric_limits<std::uint32_t>::max(),
              std::numeric_limits<std::uint32_t>::max()});
            std::vector<std::array<std::uint8_t, 3>> neighborXor(triCount, {0U, 0U, 0U});
            std::vector<std::uint8_t> degree(triCount, 0U);

            auto addConstraint = [&neighbors, &neighborXor, &degree](std::uint32_t from,
                                                                     std::uint32_t to,
                                                                     std::uint8_t requiredXor)
            {
                std::uint8_t & d = degree[from];
                if (d >= 3U)
                {
                    return;
                }
                neighbors[from][d] = to;
                neighborXor[from][d] = requiredXor;
                ++d;
            };

            // Build triangle adjacency with XOR constraints.
            std::size_t idx = 0U;
            while (idx < edgeRefs.size())
            {
                std::size_t const start = idx;
                std::uint32_t const a = edgeRefs[idx].a;
                std::uint32_t const b = edgeRefs[idx].b;
                while (idx < edgeRefs.size() && edgeRefs[idx].a == a && edgeRefs[idx].b == b)
                {
                    ++idx;
                }
                std::size_t const count = idx - start;
                if (count != 2U)
                {
                    continue;
                }

                EdgeRef const & e0 = edgeRefs[start + 0U];
                EdgeRef const & e1 = edgeRefs[start + 1U];
                if (e0.triId == e1.triId)
                {
                    continue;
                }

                std::uint8_t const requiredXor = (e0.dir == e1.dir) ? 1U : 0U;
                addConstraint(e0.triId, e1.triId, requiredXor);
                addConstraint(e1.triId, e0.triId, requiredXor);
                ++stats.adjacencyConstraints;
            }

            // Assign flips per connected component.
            std::vector<std::int8_t> flip(triCount, static_cast<std::int8_t>(-1));
            std::queue<std::uint32_t> queue;

            for (std::uint32_t t = 0U; t < static_cast<std::uint32_t>(triCount); ++t)
            {
                if (degree[t] == 0U || flip[t] != static_cast<std::int8_t>(-1))
                {
                    continue;
                }

                ++stats.components;
                flip[t] = 0;
                queue.push(t);

                while (!queue.empty())
                {
                    std::uint32_t const cur = queue.front();
                    queue.pop();

                    std::uint8_t const d = degree[cur];
                    for (std::uint8_t n = 0U; n < d; ++n)
                    {
                        std::uint32_t const other = neighbors[cur][n];
                        if (other == std::numeric_limits<std::uint32_t>::max())
                        {
                            continue;
                        }

                        std::uint8_t const requiredXor = neighborXor[cur][n];
                        std::int8_t const desired = static_cast<std::int8_t>(flip[cur] ^ requiredXor);
                        if (flip[other] == static_cast<std::int8_t>(-1))
                        {
                            flip[other] = desired;
                            queue.push(other);
                        }
                        else if (flip[other] != desired)
                        {
                            stats.hadInconsistency = true;
                        }
                    }
                }
            }

            // Apply flips.
            for (std::size_t triId = 0U; triId < triCount; ++triId)
            {
                if (flip[triId] == 1)
                {
                    std::swap(indices[triId * 3U + 1U], indices[triId * 3U + 2U]);
                    ++stats.flippedTriangles;
                }
            }

            // Optional: enforce outward global orientation (positive signed volume) when possible.
            double const volume = computeSignedVolume(positions, indices);
            if (volume < 0.0)
            {
                for (std::size_t triId = 0U; triId < triCount; ++triId)
                {
                    std::swap(indices[triId * 3U + 1U], indices[triId * 3U + 2U]);
                }
                stats.flippedGlobalOrientation = true;
            }

            return stats;
        }
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

        m_progress = 0.25;

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
        gpuPipeline.setConfig(config);
        gpuPipeline.generateMesh();

        auto const & mesh = gpuPipeline.getMesh();
        if (mesh.indices.empty())
        {
            throw std::runtime_error("Manifold dual contouring produced an empty mesh");
        }

        m_progress = 0.7;
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
      std::vector<Eigen::Vector3f> const & normals) const
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
                        
                        writer.exportMeshWithVertexColors(m_targetFile, convertedMesh, "Mesh", vertexColors, m_document, true);
                        
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
                        
                        writer.exportMeshWithColors(m_targetFile, convertedMesh, "Mesh", faceColors, m_document, true);
                        
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
                    writer.exportMesh(m_targetFile, convertedMesh, "Mesh", m_document, true);
                }
            }
            else
            {
                writer.exportMesh(m_targetFile, convertedMesh, "Mesh", m_document, true);
            }
        }
        else
        {
            vdb::exportMeshToSTL(convertedMesh, m_targetFile);
        }
    }
}
