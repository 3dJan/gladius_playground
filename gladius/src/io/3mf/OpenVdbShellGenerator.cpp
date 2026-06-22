#include "OpenVdbShellGenerator.h"

#include "../MeshExporter.h"
#include "compute/ComputeCore.h"

#include <algorithm>

namespace gladius::io
{
    namespace
    {
        [[nodiscard]] openvdb::math::Transform::Ptr createGridTransform(
            BoundingBox const& bbox,
            std::size_t width,
            std::size_t height,
            std::size_t depth)
        {
            double const voxelX = width > 1U
                ? static_cast<double>(bbox.max.x - bbox.min.x) / static_cast<double>(width - 1U)
                : 1.0;
            double const voxelY = height > 1U
                ? static_cast<double>(bbox.max.y - bbox.min.y) / static_cast<double>(height - 1U)
                : 1.0;
            double const voxelZ = depth > 1U
                ? static_cast<double>(bbox.max.z - bbox.min.z) / static_cast<double>(depth - 1U)
                : 1.0;

            auto transform = openvdb::math::Transform::createLinearTransform();
            transform->preScale(openvdb::Vec3d{voxelX, voxelY, voxelZ});
            transform->postTranslate(openvdb::Vec3d{
                static_cast<double>(bbox.min.x),
                static_cast<double>(bbox.min.y),
                static_cast<double>(bbox.min.z)});
            return transform;
        }
    }

    OpenVdbShellGenerator::OpenVdbShellGenerator(ComputeCore& core)
        : m_core(core)
    {
    }

    std::vector<ShellGenerator::ShellMesh> OpenVdbShellGenerator::generateUniformShells(
        FilamentStack const& stack,
        ThicknessSolution const& solution,
        ManifoldDualContouringOptions const& options,
        std::function<bool()> cancellationCheck)
    {
        std::vector<ShellGenerator::ShellMesh> shells;

        if (stack.size() != solution.thicknesses.size())
        {
            return shells;
        }

        if (!m_core.updateBBox())
        {
            return shells;
        }

        auto const bbox = m_core.getBoundingBox();
        if (!bbox.has_value())
        {
            return shells;
        }

        auto const intervals = ShellThicknessPartition::buildIntervals(solution);
        if (intervals.empty())
        {
            return shells;
        }

        std::size_t const sdfResolution = selectSdfResolution(options);
        m_core.setPreCompSdfSize(sdfResolution);
        m_core.precomputeSdfForBBox(*bbox);

        auto resources = m_core.getResourceContext();
        if (!resources)
        {
            return shells;
        }

        auto& sdfBuffer = resources->getPrecompSdfBuffer();

        float const totalDepth = ShellThicknessPartition::computeMaxDepth(solution.thicknesses);
        float const voxelX = sdfResolution > 1U
            ? (bbox->max.x - bbox->min.x) / static_cast<float>(sdfResolution - 1U)
            : 1.0F;
        float const voxelY = sdfResolution > 1U
            ? (bbox->max.y - bbox->min.y) / static_cast<float>(sdfResolution - 1U)
            : 1.0F;
        float const voxelZ = sdfResolution > 1U
            ? (bbox->max.z - bbox->min.z) / static_cast<float>(sdfResolution - 1U)
            : 1.0F;
        float const voxelSize = std::max({voxelX, voxelY, voxelZ, 1e-4F});
        float const narrowBandWidth = std::max(totalDepth + voxelSize * 2.0F, voxelSize * 4.0F);

        for (auto const& interval : intervals)
        {
            if (cancellationCheck && cancellationCheck())
            {
                shells.clear();
                break;
            }

            auto shellGrid = createShellGrid(sdfBuffer, *bbox, interval, narrowBandWidth);
            if (!shellGrid)
            {
                continue;
            }

            auto mesh = vdb::gridToMesh(shellGrid, *m_core.getComputeContext());
            if (mesh.getNumberOfFaces() == 0U)
            {
                continue;
            }

            shells.push_back(meshToShellMesh(
                mesh,
                stack[interval.layerIndex].name,
                static_cast<int>(interval.layerIndex)));
        }

        resources->releasePreComputedSdf();
        return shells;
    }

    float OpenVdbShellGenerator::evaluateShellSignedDistance(
        float modelSdf,
        ShellLayerDepthInterval const& interval) noexcept
    {
        float const outerConstraint = modelSdf + interval.outerDepth;
        float const innerConstraint = -(modelSdf + interval.innerDepth);
        return std::max(outerConstraint, innerConstraint);
    }

    openvdb::FloatGrid::Ptr OpenVdbShellGenerator::createShellGrid(
        PreComputedSdf& sdf,
        BoundingBox const& bbox,
        ShellLayerDepthInterval const& interval,
        float narrowBandWidth) const
    {
        using namespace openvdb;

        sdf.read();

        FloatGrid::Ptr grid = FloatGrid::create(narrowBandWidth);
        grid->setGridClass(GRID_LEVEL_SET);
        grid->setName("OpenVDB shell band");

        FloatGrid::Accessor accessor = grid->getAccessor();

        auto const width = sdf.getWidth();
        auto const height = sdf.getHeight();
        auto const depth = sdf.getDepth();

        for (int z = 0; z < static_cast<int>(depth); ++z)
        {
            for (int y = 0; y < static_cast<int>(height); ++y)
            {
                for (int x = 0; x < static_cast<int>(width); ++x)
                {
                    float const modelSdf = sdf.getValue(x, y, z).s[3];
                    float const shellSdf = evaluateShellSignedDistance(modelSdf, interval);

                    if (shellSdf < narrowBandWidth)
                    {
                        accessor.setValue(Coord{x, y, z}, std::max(shellSdf, -narrowBandWidth));
                    }
                }
            }
            grid->pruneGrid();
        }

        grid->setTransform(createGridTransform(bbox, width, height, depth));
        return grid;
    }

    ShellGenerator::ShellMesh OpenVdbShellGenerator::meshToShellMesh(
        Mesh& mesh,
        std::string filamentName,
        int layerIndex) const
    {
        ShellGenerator::ShellMesh shell;
        shell.filamentName = std::move(filamentName);
        shell.layerIndex = layerIndex;

        std::size_t const faceCount = mesh.getNumberOfFaces();
        shell.vertices.reserve(faceCount * 3U);
        shell.indices.reserve(faceCount * 3U);

        for (std::size_t faceIndex = 0; faceIndex < faceCount; ++faceIndex)
        {
            auto const face = mesh.getFace(faceIndex);
            for (int vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
            {
                shell.indices.push_back(static_cast<std::uint32_t>(shell.vertices.size()));
                shell.vertices.push_back(face.vertices[vertexIndex]);
            }
        }

        return shell;
    }

    std::size_t OpenVdbShellGenerator::selectSdfResolution(
        ManifoldDualContouringOptions const& options) noexcept
    {
        std::size_t const requested = std::size_t{1} << std::min<std::size_t>(options.maxDepth + 1U, 8U);
        return std::clamp<std::size_t>(requested, 64U, 256U);
    }
} // namespace gladius::io