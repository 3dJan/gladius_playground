#include "PaletteExtractor.h"

#include "io/3mf/FaceColorSampler.h"
#include "io/3mf/FaceColors.h"
#include "io/MeshExporter.h"

#include "compute/ManifoldDualContouringGpu.h"
#include "compute/ProgramManager.h"
#include "compute/ComputeCore.h"

#include <DualContouringSamplingProgram.h>

#include <stdexcept>
#include <set>
#include <tuple>
#include <array>

namespace gladius::io
{
    namespace
    {
        using Triangle = std::array<std::uint32_t, 3>;

        struct PaletteMesh
        {
            std::vector<Eigen::Vector3f> vertices;
            std::vector<Triangle> faces;
        };

        [[nodiscard]] double alignToLayer(double value, double increment)
        {
            return std::floor(value / increment) * increment;
        }

        [[nodiscard]] PaletteMesh toPaletteMesh(Mesh & mesh)
        {
            PaletteMesh result;
            std::size_t const faceCount = mesh.getNumberOfFaces();
            result.vertices.reserve(faceCount * 3U);
            result.faces.reserve(faceCount);

            for (std::size_t faceIndex = 0; faceIndex < faceCount; ++faceIndex)
            {
                auto const face = mesh.getFace(faceIndex);
                std::uint32_t const baseIndex = static_cast<std::uint32_t>(result.vertices.size());
                result.faces.push_back({baseIndex, baseIndex + 1U, baseIndex + 2U});
                for (int vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
                {
                    auto const & vertex = face.vertices[vertexIndex];
                    result.vertices.emplace_back(vertex.x(), vertex.y(), vertex.z());
                }
            }

            return result;
        }

        [[nodiscard]] PaletteMesh buildManifoldPaletteMesh(
            gladius::ComputeCore & core,
            PaletteExtractionOptions const & options)
        {
            compute::ManifoldDualContouringConfig config{};
            auto const & opt = options.manifoldOptions;
            config.initialDepth = opt.initialDepth;
            config.maxDepth = opt.maxDepth;
            config.enableGpu = opt.enableGpu;
            config.enableCpuFallback = opt.enableCpuFallback;
            config.enableCaching = opt.enableCaching;
            config.isoValue = opt.isoValue;
            config.minFeatureSize = opt.minFeatureSize;
            config.enableChunking = opt.enableChunking;
            config.enableHierarchicalOctree = opt.enableHierarchicalOctree;
            config.enableSharpFeaturePostProcess = opt.enableSharpFeaturePostProcess;
            config.sharpFeatureAngleThreshold = opt.sharpFeatureAngleThreshold;
            config.subdivisionIterations = opt.subdivisionIterations;
            config.projectToSurface = opt.projectToSurface;
            config.simplificationMethod = (opt.simplificationMethod == SimplificationMethod::QemSdfAware)
                                            ? compute::SimplificationMethod::QemSdfAware
                                            : compute::SimplificationMethod::None;
            config.enableSimplification = opt.enableSimplification;
            config.simplificationMaxSdfError = opt.simplificationMaxSdfError;
            config.simplificationMaxQemError = opt.simplificationMaxQemError;
            config.simplificationMaxNormalDeviation = opt.simplificationMaxNormalDeviation;
            config.simplificationSdfWeight = opt.simplificationSdfWeight;
            config.simplificationQemWeight = opt.simplificationQemWeight;
            config.simplificationNormalWeight = opt.simplificationNormalWeight;
            config.simplificationSharpEdgeThreshold = opt.simplificationSharpEdgeThreshold;
            config.simplificationBatchSize = opt.simplificationBatchSize;
            config.simplificationMaxPasses = opt.simplificationMaxPasses;
            config.simplificationTargetTriangles = opt.simplificationTargetTriangles;
            config.simplificationTargetReduction = opt.simplificationTargetReduction;

            compute::ManifoldDualContouringGpu pipeline(core);
            pipeline.setConfig(config);
            pipeline.generateMesh();
            auto const & mesh = pipeline.getMesh();
            if (mesh.indices.empty() || mesh.positions.empty())
            {
                throw std::runtime_error("Mesh extraction produced no geometry");
            }

            PaletteMesh result;
            result.vertices = mesh.positions;
            result.faces.reserve(mesh.indices.size() / 3U);
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
            {
                result.faces.push_back({mesh.indices[i + 0], mesh.indices[i + 1], mesh.indices[i + 2]});
            }
            return result;
        }

        [[nodiscard]] PaletteMesh buildLayeredMarchingCubesPaletteMesh(
            gladius::ComputeCore & core,
            std::size_t qualityLevel)
        {
            core.updateClippingAreaWithPadding();

            auto resources = core.getResourceContext();
            if (!resources)
            {
                throw std::runtime_error("No resource context available for OpenVDB palette derivation");
            }

            resources->requestDistanceMaps();
            auto & distanceMipMaps = resources->getDistanceMipMaps();
            if (distanceMipMaps.empty())
            {
                throw std::runtime_error("No distance maps available for OpenVDB palette derivation");
            }

            qualityLevel = std::min<std::size_t>(qualityLevel, distanceMipMaps.size() - 1U);

            auto const clippingArea = resources->getClippingArea();
            auto const resX = distanceMipMaps[qualityLevel]->getWidth();
            auto const widthMm = clippingArea.z - clippingArea.x;
            auto const voxelSize = widthMm / static_cast<float>(resX);
            auto const layerIncrementMm = static_cast<double>(voxelSize);
            auto const bandWidthMm = voxelSize * 2.0F;

            auto const bbox = core.getBoundingBox();
            if (!bbox.has_value())
            {
                throw std::runtime_error("Bounding box missing for OpenVDB palette derivation");
            }

            double startHeightMm = alignToLayer(bbox->min.z - layerIncrementMm, layerIncrementMm);
            double currentHeightMm = startHeightMm;
            core.setSliceHeight(currentHeightMm);

            auto grid = openvdb::FloatGrid::create(bandWidthMm);
            grid->setGridClass(openvdb::GRID_LEVEL_SET);
            grid->setName("Palette extraction grid");

            auto transformation = openvdb::math::Transform::createLinearTransform();
            transformation->preScale(static_cast<double>(voxelSize));
            transformation->postTranslate(openvdb::Vec3d{
                static_cast<double>(clippingArea.x),
                static_cast<double>(clippingArea.y),
                0.0});
            grid->setTransform(transformation);

            while (core.getSliceHeight() < bbox->max.z + layerIncrementMm)
            {
                core.generateSdfSlice();

                auto & distMap = *distanceMipMaps[qualityLevel];
                distMap.read();

                auto accessor = grid->getAccessor();
                int const z = static_cast<int>(std::floor(currentHeightMm / layerIncrementMm));

                for (int y = 0; y < static_cast<int>(distMap.getHeight()); ++y)
                {
                    for (int x = 0; x < static_cast<int>(distMap.getWidth()); ++x)
                    {
                        openvdb::Coord xyz{x, y, z};
                        float const value = std::clamp<float>(
                            distMap.getValue(x, y).x,
                            -bandWidthMm,
                            bandWidthMm);
                        accessor.setValue(xyz, value);
                    }
                }

                grid->pruneGrid();
                currentHeightMm = alignToLayer(currentHeightMm + layerIncrementMm, layerIncrementMm);
                core.setSliceHeight(currentHeightMm);
            }

            auto mesh = vdb::gridToMesh(grid, *core.getComputeContext());
            if (mesh.getNumberOfFaces() == 0U)
            {
                throw std::runtime_error("OpenVDB mesh extraction produced no geometry");
            }

            return toPaletteMesh(mesh);
        }
    }

    std::vector<Eigen::Vector3f> derivePaletteFromMesh(gladius::ComputeCore & core,
                                                       PaletteExtractionOptions const & options)
    {
        if (!core.updateBBox())
        {
            throw std::runtime_error("Bounding box update failed before palette derivation");
        }

        auto const bbox = core.getBoundingBox();
        if (!bbox.has_value())
        {
            throw std::runtime_error("Mesh generation failed, bounding box is empty");
        }

        PaletteMesh paletteMesh;
        switch (options.method)
        {
        case SurfaceExtractionMethod::LayeredMarchingCubes:
            paletteMesh = buildLayeredMarchingCubesPaletteMesh(core, options.marchingCubesQualityLevel);
            break;
        case SurfaceExtractionMethod::ManifoldDualContouring:
            paletteMesh = buildManifoldPaletteMesh(core, options);
            break;
        case SurfaceExtractionMethod::DualContouring:
            throw std::runtime_error("Palette derivation is not implemented for dual contouring.");
        default:
            throw std::runtime_error("Unsupported extraction method for palette derivation.");
        }

        if (paletteMesh.faces.empty() || paletteMesh.vertices.empty())
        {
            throw std::runtime_error("Mesh extraction produced no geometry");
        }

        auto * samplingProgram = core.getProgramManager().getDualContouringSamplingProgram();
        if (samplingProgram == nullptr)
        {
            throw std::runtime_error("No sampling program available for palette derivation");
        }
        auto primitives = core.getPrimitives();
        if (primitives == nullptr)
        {
            throw std::runtime_error("No primitives available for palette derivation");
        }

        auto vertexColors = FaceColorSampler::sampleVertexColors(
                    paletteMesh.vertices,
                    paletteMesh.faces,
                    *samplingProgram,
                    *primitives,
                    nullptr,
                    options.convertToSrgb);

        std::set<std::tuple<std::uint8_t, std::uint8_t, std::uint8_t>> unique;
        for (auto const & face : vertexColors.faceVertexColors)
        {
            for (auto const & c : face.colors)
            {
                unique.emplace(c.r, c.g, c.b);
            }
        }

        std::vector<Eigen::Vector3f> palette;
        palette.reserve(unique.size());
        for (auto const & [r, g, b] : unique)
        {
            palette.emplace_back(static_cast<float>(r) / 255.0F,
                                 static_cast<float>(g) / 255.0F,
                                 static_cast<float>(b) / 255.0F);
        }

        if (palette.empty())
        {
            throw std::runtime_error("No colors found in extracted mesh");
        }

        return palette;
    }
}
