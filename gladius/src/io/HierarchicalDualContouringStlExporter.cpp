#include "HierarchicalDualContouringStlExporter.h"

#include "MeshExporter.h"

#include "ComputeCore.h"
#include "ComputeContext.h"
#include "../types.h"

#include <chrono>
#include <stdexcept>
#include <utility>
#include <cstdlib>
#include <mutex>
#include <iostream>

#include <Eigen/Geometry>

#include <fmt/format.h>

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
    }

    HierarchicalDualContouringStlExporter::HierarchicalDualContouringStlExporter() = default;

    HierarchicalDualContouringStlExporter::HierarchicalDualContouringStlExporter(
      events::SharedLogger logger)
        : m_logger(std::move(logger))
    {
    }

    void HierarchicalDualContouringStlExporter::setOptions(
    hierarchical_dc::HierarchicalConfig options)
    {
        m_options = std::move(options);
    }

    void HierarchicalDualContouringStlExporter::beginExport(std::filesystem::path const & fileName,
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
                    m_logger->addEvent(
                      {fmt::format("Hierarchical dual contouring STL export failed: {}", ex.what()),
                       events::Severity::Error});
                }
            }
        });
    }

    bool HierarchicalDualContouringStlExporter::advanceExport(ComputeCore & /*generator*/)
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

    void HierarchicalDualContouringStlExporter::finalize()
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

    double HierarchicalDualContouringStlExporter::getProgress() const
    {
        return clampProgress(m_progress.load());
    }

    bool HierarchicalDualContouringStlExporter::hasError() const
    {
        return m_state.load() == State::Failed;
    }

    std::string const & HierarchicalDualContouringStlExporter::errorMessage() const
    {
        return m_errorMessage;
    }

    void HierarchicalDualContouringStlExporter::performExport(ComputeCore & generator)
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

        hierarchical_dc::HierarchicalConfig config = m_options;

        hierarchical_dc::HierarchicalOctreeBuilder builder(generator, config);
        builder.buildOctree(boundingBox.value());

        m_progress = 0.6;

        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::uint32_t> indices;
        builder.extractMesh(vertices, indices);

        if (indices.empty())
        {
            throw std::runtime_error("Hierarchical dual contouring produced an empty mesh");
        }

        writeMeshToFile(generator, vertices, indices);

        if (m_logger)
        {
            auto const & stats = builder.getStats();
            m_logger->addEvent({fmt::format(
                                   "Hierarchical dual contouring STL export completed. Leaves: {}, "
                                   "Refinement passes: {}, Total nodes: {}",
                                   stats.leafNodes,
                                   stats.refinementPasses,
                                   stats.totalNodes),
                                events::Severity::Info});
        }
    }

    void HierarchicalDualContouringStlExporter::writeMeshToFile(
      ComputeCore & generator,
      std::vector<Eigen::Vector3f> const & vertices,
      std::vector<std::uint32_t> const & indices) const
    {
        if (indices.size() % 3U != 0U)
        {
            throw std::runtime_error("Hierarchical dual contouring mesh has incomplete triangles");
        }

        auto computeContext = generator.getComputeContext();
        if (computeContext == nullptr)
        {
            throw std::runtime_error("Compute context unavailable for STL export");
        }

        Mesh convertedMesh(*computeContext);

        for (std::size_t i = 0U; i + 2U < indices.size(); i += 3U)
        {
            auto const indexA = static_cast<std::size_t>(indices[i + 0U]);
            auto const indexB = static_cast<std::size_t>(indices[i + 1U]);
            auto const indexC = static_cast<std::size_t>(indices[i + 2U]);

            if (indexA >= vertices.size() || indexB >= vertices.size() || indexC >= vertices.size())
            {
                throw std::runtime_error(
                  "Hierarchical dual contouring mesh references out-of-range vertex indices");
            }

            auto const a = vertices[indexA];
            auto const b = vertices[indexB];
            auto const c = vertices[indexC];

            Eigen::Vector3f normal = (b - a).cross(c - a);
            if (normal.squaredNorm() <= 1e-12F)
            {
                normal = Eigen::Vector3f{0.0F, 0.0F, 1.0F};
            }
            else
            {
                normal.normalize();
            }

            Face faceData{};
            faceData.normal = toVector3(normal);
            faceData.vertices = {toVector3(a), toVector3(b), toVector3(c)};
            faceData.vertexNormals = {faceData.normal, faceData.normal, faceData.normal};

            convertedMesh.addFace(faceData);
        }

        if (convertedMesh.getNumberOfFaces() == 0U)
        {
            throw std::runtime_error("Hierarchical dual contouring produced no valid faces");
        }

        vdb::exportMeshToSTL(convertedMesh, m_targetFile);
    }

}
