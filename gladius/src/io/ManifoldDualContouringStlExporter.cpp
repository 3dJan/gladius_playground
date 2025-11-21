#include "ManifoldDualContouringStlExporter.h"

#include "MeshExporter.h"

#include "ComputeContext.h"
#include "ComputeCore.h"
#include "../compute/ManifoldDualContouringGpu.h"
#include "../types.h"

#include <Eigen/Geometry>
#include <fmt/format.h>

#include <stdexcept>
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

        [[nodiscard]] Vector3 toVector3(Eigen::Vector3f const & value)
        {
            return Vector3{value.x(), value.y(), value.z()};
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

    void ManifoldDualContouringStlExporter::beginExport(std::filesystem::path const & fileName,
                                                        ComputeCore & generator)
    {
        m_targetFile = fileName;
        m_computeCore = &generator;
        m_state = State::Running;
        m_progress = 0.0;
        m_errorMessage.clear();
    }

    bool ManifoldDualContouringStlExporter::advanceExport(ComputeCore & generator)
    {
        if (m_state == State::Completed || m_state == State::Failed || m_state == State::Idle)
        {
            return false;
        }

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

        return false;
    }

    void ManifoldDualContouringStlExporter::finalize()
    {
        m_computeCore = nullptr;
        m_state = State::Idle;
        m_progress = clampProgress(m_progress);
    }

    double ManifoldDualContouringStlExporter::getProgress() const
    {
        return clampProgress(m_progress);
    }

    bool ManifoldDualContouringStlExporter::hasError() const
    {
        return m_state == State::Failed;
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
            throw std::runtime_error("Compute context unavailable for STL export");
        }

        Mesh convertedMesh(*computeContext);

        for (std::size_t i = 0U; i + 2U < indices.size(); i += 3U)
        {
            auto const idxA = static_cast<std::size_t>(indices[i + 0U]);
            auto const idxB = static_cast<std::size_t>(indices[i + 1U]);
            auto const idxC = static_cast<std::size_t>(indices[i + 2U]);

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

        vdb::exportMeshToSTL(convertedMesh, m_targetFile);
    }
}
