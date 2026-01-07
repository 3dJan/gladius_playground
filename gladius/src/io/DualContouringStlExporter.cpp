#include "DualContouringStlExporter.h"

#include "MeshExporter.h"

#include "ComputeCore.h"
#include "ComputeContext.h"
#include "../types.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

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

        [[nodiscard]] std::size_t estimateMaxDepth(std::size_t sdfResolution)
        {
            if (sdfResolution < 2U)
            {
                return 0U;
            }

            std::size_t cellCount = sdfResolution - 1U;
            std::size_t depth = 0U;
            while ((1ULL << depth) < cellCount)
            {
                ++depth;
            }
            return std::max<std::size_t>(depth, 1U);
        }

        [[nodiscard]] Vector3 toVector3(Eigen::Vector3f const & value)
        {
            return Vector3{value.x(), value.y(), value.z()};
        }
    }

    DualContouringStlExporter::DualContouringStlExporter()
    {
    }

    DualContouringStlExporter::DualContouringStlExporter(events::SharedLogger logger)
        : m_logger(std::move(logger))
    {
    }

    void DualContouringStlExporter::setOptions(DualContouringOptions options)
    {
        m_options = std::move(options);
    }

    void DualContouringStlExporter::beginExport(std::filesystem::path const & fileName,
                                                ComputeCore & generator)
    {
        m_targetFile = fileName;
        m_computeCore = &generator;
        m_state.store(State::Running);
        m_progress.store(0.0);
        m_errorMessage.clear();

        // Launch the export in a background thread
        m_exportFuture = std::async(std::launch::async, [this, &generator]() {
            performExport(generator);
        });
    }

    bool DualContouringStlExporter::advanceExport(ComputeCore & /*generator*/)
    {
        auto const currentState = m_state.load();
        if (currentState == State::Completed || currentState == State::Failed ||
            currentState == State::Idle)
        {
            return false;
        }

        // Check if background task has completed
        if (m_exportFuture.valid())
        {
            auto const status = m_exportFuture.wait_for(std::chrono::seconds(0));
            if (status == std::future_status::ready)
            {
                try
                {
                    m_exportFuture.get(); // Retrieve result and propagate exceptions
                    m_state.store(State::Completed);
                    m_progress.store(1.0);
                }
                catch (std::exception const & ex)
                {
                    m_state.store(State::Failed);
                    m_errorMessage = ex.what();
                    m_progress.store(1.0);
                    if (m_logger)
                    {
                        m_logger->addEvent(
                          {fmt::format("Dual contouring STL export failed: {}", ex.what()),
                           events::Severity::Error});
                    }
                }
                return false;
            }
        }

        return true; // Still running
    }

    void DualContouringStlExporter::finalize()
    {
        // Wait for background task to complete
        if (m_exportFuture.valid())
        {
            m_exportFuture.wait();
        }
        m_computeCore = nullptr;
        m_state.store(State::Idle);
    }

    double DualContouringStlExporter::getProgress() const
    {
        return clampProgress(m_progress.load());
    }

    bool DualContouringStlExporter::hasError() const
    {
        return m_state.load() == State::Failed;
    }

    std::string const & DualContouringStlExporter::errorMessage() const
    {
        return m_errorMessage;
    }

    void DualContouringStlExporter::performExport(ComputeCore & generator)
    {
        if (m_targetFile.empty())
        {
            throw std::runtime_error("No output filename specified for STL export");
        }

        if (m_options.sdfResolution < 2U)
        {
            throw std::runtime_error("Dual contouring resolution must be at least 2");
        }

        if (m_options.forceUniform && !std::has_single_bit(m_options.sdfResolution - 1U))
        {
            throw std::runtime_error("For uniform dual contouring exports, resolution - 1 must be a power of two");
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
            m_state.store(State::Idle);
            return;
        }

        generator.setPreCompSdfSize(m_options.sdfResolution);

        auto const config = makeConfig();
        dual_contouring::OctreeBuilder builder(generator, boundingBox.value(), config);
        dual_contouring::OctreeMetrics metrics{};
        auto root = builder.build(metrics);
        if (!root)
        {
            throw std::runtime_error("Failed to build dual contouring octree");
        }

        // Check for cancellation after octree build
        if (isCancellationRequested())
        {
            m_state.store(State::Idle);
            return;
        }

        m_progress.store(0.5);

        dual_contouring::DualContouringDiagnostics diagnostics{};
        auto mesh = dual_contouring::buildDualContouringMesh(builder, *root, config, &diagnostics);

        // Check for cancellation before file write
        if (isCancellationRequested())
        {
            m_state.store(State::Idle);
            return;
        }

        writeMeshToFile(generator, mesh);

        if (m_logger)
        {
            m_logger->addEvent({fmt::format("Dual contouring STL export completed. Vertices: {}, Faces: {}",
                                            diagnostics.vertexCount,
                                            diagnostics.faceCount),
                                events::Severity::Info});
        }
    }

    void DualContouringStlExporter::writeMeshToFile(ComputeCore & generator,
                                                    dual_contouring::DualContouringMesh const & mesh) const
    {
        if (mesh.faces.empty())
        {
            throw std::runtime_error("Dual contouring produced an empty mesh");
        }

        auto computeContext = generator.getComputeContext();
        if (computeContext == nullptr)
        {
            throw std::runtime_error("Compute context unavailable for STL export");
        }

        Mesh convertedMesh(*computeContext);
        for (auto const & face : mesh.faces)
        {
            auto const indexA = static_cast<std::size_t>(face.x());
            auto const indexB = static_cast<std::size_t>(face.y());
            auto const indexC = static_cast<std::size_t>(face.z());

            if (indexA >= mesh.vertices.size() || indexB >= mesh.vertices.size() ||
                indexC >= mesh.vertices.size())
            {
                throw std::runtime_error("Dual contouring mesh references out-of-range vertex indices");
            }

            auto const a = mesh.vertices.at(indexA);
            auto const b = mesh.vertices.at(indexB);
            auto const c = mesh.vertices.at(indexC);

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

        vdb::exportMeshToSTL(convertedMesh, m_targetFile);
    }

    dual_contouring::OctreeBuildConfig DualContouringStlExporter::makeConfig() const
    {
        dual_contouring::OctreeBuildConfig config{};
        config.sdfResolution = m_options.sdfResolution;
        config.isoValue = m_options.isoValue;
        config.forceUniform = m_options.forceUniform;
        config.maxDepth = m_options.maxDepth.value_or(estimateMaxDepth(m_options.sdfResolution));
        config.maxDepth = std::max<std::size_t>(config.maxDepth, 1U);
        config.enableGpuSampling = m_options.enableGpuSampling;
        config.enableCurvatureRefinement = m_options.enableCurvatureRefinement;
        config.curvatureThreshold = m_options.curvatureThreshold;
        config.enableBalancedRefinement = m_options.enableBalancedRefinement;
        return config;
    }
}
