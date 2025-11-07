#include "HierarchicalDualContouring.h"

#include "BBox.h"
#include "EventLogger.h"
#include "DualContouringOctree.h"
#include "DualContouringQef.h"
#include "DualContouringSamplingProgram.h"
#include "compute/ComputeCore.h"
#include "compute/ProgramManager.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <memory>
#include <limits>

namespace gladius::hierarchical_dc
{
    namespace
    {
        /// Edge corner index pairs (12 edges of cube)
        constexpr std::array<std::pair<std::uint8_t, std::uint8_t>, 12> EDGE_CORNERS = {{
          {0, 1}, {2, 3}, {4, 5}, {6, 7}, // X-aligned
          {0, 2}, {1, 3}, {4, 6}, {5, 7}, // Y-aligned
          {0, 4}, {1, 5}, {2, 6}, {3, 7}  // Z-aligned
        }};

        [[nodiscard]] Eigen::Vector3f toEigen(cl_float4 const & value)
        {
            return Eigen::Vector3f{value.s[0], value.s[1], value.s[2]};
        }

        [[nodiscard]] Eigen::Vector3f boundingBoxCenter(BoundingBox const & bounds)
        {
            return Eigen::Vector3f{0.5F * (bounds.min.s[0] + bounds.max.s[0]),
                                   0.5F * (bounds.min.s[1] + bounds.max.s[1]),
                                   0.5F * (bounds.min.s[2] + bounds.max.s[2])};
        }

        [[nodiscard]] BoundingBox makeBoundingBox(Eigen::Vector3f const & min,
                                                  Eigen::Vector3f const & max)
        {
            cl_float4 minValue{};
            minValue.s[0] = min.x();
            minValue.s[1] = min.y();
            minValue.s[2] = min.z();
            minValue.s[3] = 0.0F;

            cl_float4 maxValue{};
            maxValue.s[0] = max.x();
            maxValue.s[1] = max.y();
            maxValue.s[2] = max.z();
            maxValue.s[3] = 0.0F;

            return BoundingBox{minValue, maxValue};
        }

        [[nodiscard]] dual_contouring::AxisAlignedBoundingBox toAxisAlignedBoundingBox(
          BoundingBox const & bounds)
        {
            dual_contouring::AxisAlignedBoundingBox result;
            result.min = toEigen(bounds.min);
            result.max = toEigen(bounds.max);
            return result;
        }

        [[nodiscard]] bool approximatelyEqual(float a, float b, float tolerance = 1e-4F)
        {
            return std::abs(a - b) <= tolerance;
        }

        [[nodiscard]] bool boundingBoxesApproximatelyEqual(BoundingBox const & lhs,
                                                           BoundingBox const & rhs,
                                                           float tolerance = 1e-4F)
        {
            return approximatelyEqual(lhs.min.s[0], rhs.min.s[0], tolerance) &&
                   approximatelyEqual(lhs.min.s[1], rhs.min.s[1], tolerance) &&
                   approximatelyEqual(lhs.min.s[2], rhs.min.s[2], tolerance) &&
                   approximatelyEqual(lhs.max.s[0], rhs.max.s[0], tolerance) &&
                   approximatelyEqual(lhs.max.s[1], rhs.max.s[1], tolerance) &&
                   approximatelyEqual(lhs.max.s[2], rhs.max.s[2], tolerance);
        }
    }

    struct HierarchicalOctreeBuilder::CpuSampler
    {
        std::vector<float> values;
        std::size_t width{0U};
        std::size_t height{0U};
        std::size_t depth{0U};
        Eigen::Vector3f min{Eigen::Vector3f::Zero()};
        Eigen::Vector3f max{Eigen::Vector3f::Zero()};
        Eigen::Vector3f spacing{Eigen::Vector3f::Ones()};
        BoundingBox bounds{};
        bool initialized{false};
        std::size_t resolution{0U};

        void reset()
        {
            values.clear();
            width = 0U;
            height = 0U;
            depth = 0U;
            min.setZero();
            max.setZero();
            spacing = Eigen::Vector3f::Ones();
            bounds = {};
            initialized = false;
            resolution = 0U;
        }

        bool initialize(ComputeCore & core,
                        BoundingBox const & requestedBounds,
                        std::size_t targetResolution)
        {
            auto resources = core.getResourceContext();
            if (!resources)
            {
                return false;
            }

            try
            {
                if (targetResolution >= 2U)
                {
                    core.setPreCompSdfSize(targetResolution);
                }

                core.precomputeSdfForBBox(requestedBounds);

                auto & sdfBuffer = resources->getPrecompSdfBuffer();
                sdfBuffer.read();

                width = std::max<std::size_t>(sdfBuffer.getWidth(), 1U);
                height = std::max<std::size_t>(sdfBuffer.getHeight(), 1U);
                depth = std::max<std::size_t>(sdfBuffer.getDepth(), 1U);

                auto const & raw = sdfBuffer.getData();
                values.assign(raw.begin(), raw.end());

                bounds = resources->getPreCompSdfBBox();
                min = Eigen::Vector3f{bounds.min.x, bounds.min.y, bounds.min.z};
                max = Eigen::Vector3f{bounds.max.x, bounds.max.y, bounds.max.z};

                spacing.x() =
                  (width > 1U) ? (max.x() - min.x()) / static_cast<float>(width - 1U) : 1.0F;
                spacing.y() =
                  (height > 1U) ? (max.y() - min.y()) / static_cast<float>(height - 1U) : 1.0F;
                spacing.z() =
                  (depth > 1U) ? (max.z() - min.z()) / static_cast<float>(depth - 1U) : 1.0F;
                spacing = spacing.cwiseMax(Eigen::Vector3f::Constant(1e-5F));

                initialized = true;
                resolution = targetResolution;
                return true;
            }
            catch (...)
            {
                reset();
                return false;
            }
        }

        [[nodiscard]] float sample(Eigen::Vector3f const & position) const
        {
            if (!initialized || values.empty())
            {
                return 0.0F;
            }

            Eigen::Vector3f const extent = max - min;
            Eigen::Vector3f const safeExtent = extent.cwiseMax(Eigen::Vector3f::Constant(1e-6F));
            Eigen::Vector3f normalized = (position - min).cwiseQuotient(safeExtent);
            normalized = normalized.cwiseMax(Eigen::Vector3f::Zero());
            normalized = normalized.cwiseMin(Eigen::Vector3f::Ones());

            float const sampleX =
              normalized.x() * static_cast<float>(std::max<std::size_t>(width, 1U) - 1U);
            float const sampleY =
              normalized.y() * static_cast<float>(std::max<std::size_t>(height, 1U) - 1U);
            float const sampleZ =
              normalized.z() * static_cast<float>(std::max<std::size_t>(depth, 1U) - 1U);

            std::size_t const x0 = static_cast<std::size_t>(std::floor(sampleX));
            std::size_t const y0 = static_cast<std::size_t>(std::floor(sampleY));
            std::size_t const z0 = static_cast<std::size_t>(std::floor(sampleZ));

            std::size_t const x1 = std::min(x0 + 1U, std::max<std::size_t>(width, 1U) - 1U);
            std::size_t const y1 = std::min(y0 + 1U, std::max<std::size_t>(height, 1U) - 1U);
            std::size_t const z1 = std::min(z0 + 1U, std::max<std::size_t>(depth, 1U) - 1U);

            float const tx = sampleX - static_cast<float>(x0);
            float const ty = sampleY - static_cast<float>(y0);
            float const tz = sampleZ - static_cast<float>(z0);

            auto index = [this](std::size_t x, std::size_t y, std::size_t z) -> std::size_t
            {
                std::size_t const safeWidth = std::max<std::size_t>(width, 1U);
                std::size_t const safeHeight = std::max<std::size_t>(height, 1U);
                std::size_t const safeDepth = std::max<std::size_t>(depth, 1U);

                x = std::min(x, safeWidth - 1U);
                y = std::min(y, safeHeight - 1U);
                z = std::min(z, safeDepth - 1U);

                return z * safeWidth * safeHeight + y * safeWidth + x;
            };

            float const c000 = values[index(x0, y0, z0)];
            float const c100 = values[index(x1, y0, z0)];
            float const c010 = values[index(x0, y1, z0)];
            float const c110 = values[index(x1, y1, z0)];
            float const c001 = values[index(x0, y0, z1)];
            float const c101 = values[index(x1, y0, z1)];
            float const c011 = values[index(x0, y1, z1)];
            float const c111 = values[index(x1, y1, z1)];

            float const c00 = c000 * (1.0F - tx) + c100 * tx;
            float const c10 = c010 * (1.0F - tx) + c110 * tx;
            float const c01 = c001 * (1.0F - tx) + c101 * tx;
            float const c11 = c011 * (1.0F - tx) + c111 * tx;

            float const c0 = c00 * (1.0F - ty) + c10 * ty;
            float const c1 = c01 * (1.0F - ty) + c11 * ty;

            return c0 * (1.0F - tz) + c1 * tz;
        }

        [[nodiscard]] Eigen::Vector3f gradient(Eigen::Vector3f const & position,
                                               float epsilon) const
        {
            float const safeEpsilon = std::max(epsilon, 1e-4F);
            Eigen::Vector3f grad{Eigen::Vector3f::Zero()};

            Eigen::Vector3f const offsetX{safeEpsilon, 0.0F, 0.0F};
            Eigen::Vector3f const offsetY{0.0F, safeEpsilon, 0.0F};
            Eigen::Vector3f const offsetZ{0.0F, 0.0F, safeEpsilon};

            float const xp = sample(position + offsetX);
            float const xn = sample(position - offsetX);
            float const yp = sample(position + offsetY);
            float const yn = sample(position - offsetY);
            float const zp = sample(position + offsetZ);
            float const zn = sample(position - offsetZ);

            float const denom = 2.0F * safeEpsilon;
            grad.x() = (xp - xn) / denom;
            grad.y() = (yp - yn) / denom;
            grad.z() = (zp - zn) / denom;
            return grad;
        }
    };

    void applyQualityPreset(HierarchicalConfig & config, HierarchicalQuality quality)
    {
        switch (quality)
        {
        case HierarchicalQuality::Draft:
            config.initialDepth = 5U;
            config.maxDepth = 5U;
            config.refinementIterations = 0U;
            config.curvatureThreshold = 1.0F; // Effectively disable adaptive
            config.enableProgressiveRefinement = false;
            config.cpuFallbackResolution = 64U;
            break;

        case HierarchicalQuality::Balanced:
            config.initialDepth = 5U;
            config.maxDepth = 7U;
            config.refinementIterations = 1U;
            config.curvatureThreshold = 0.4F;
            config.enableProgressiveRefinement = true;
            config.cpuFallbackResolution = 96U;
            break;

        case HierarchicalQuality::Fine:
            config.initialDepth = 6U;
            config.maxDepth = 8U;
            config.refinementIterations = 2U;
            config.curvatureThreshold = 0.25F;
            config.enableProgressiveRefinement = true;
            config.cpuFallbackResolution = 129U;
            break;

        case HierarchicalQuality::UltraFine:
            config.initialDepth = 7U;
            config.maxDepth = 9U;
            config.refinementIterations = 3U;
            config.curvatureThreshold = 0.15F;
            config.zeroCrossingTolerance = 1e-6F;
            config.enableProgressiveRefinement = true;
            config.cpuFallbackResolution = 192U;
            break;

        case HierarchicalQuality::Custom:
            // User provides all parameters
            break;
        }
    }

    HierarchicalOctreeBuilder::HierarchicalOctreeBuilder(ComputeCore & core,
                                                         HierarchicalConfig config)
        : m_core(&core)
        , m_config(std::move(config))
        , m_logger(core.getProgramManager().getSharedLogger())
    {
    }

    HierarchicalOctreeBuilder::~HierarchicalOctreeBuilder() = default;

    void HierarchicalOctreeBuilder::buildOctree(BoundingBox const & bounds)
    {
        auto const startTime = std::chrono::high_resolution_clock::now();

        m_stats.reset();
        m_nodes.clear();
        m_levels.clear();
        m_rootBounds = bounds;
        if (m_cpuSampler)
        {
            m_cpuSampler->reset();
        }

        logInfo("Starting hierarchical dual contouring octree construction");

        // Phase 1: Build coarse octree (breadth-first, level-by-level)
        buildInitialOctree();

        // Phase 2: Adaptive refinement (iterative improvement)
        if (m_config.enableProgressiveRefinement && m_config.refinementIterations > 0U)
        {
            refineAdaptively();
        }

        // Phase 3: High-precision zero-crossing refinement
        refineZeroCrossings();

        // Phase 4: QEF vertex solving
        solveQEFVertices();

        auto const endTime = std::chrono::high_resolution_clock::now();
        m_stats.totalConstructionTimeMs =
          std::chrono::duration<double, std::milli>(endTime - startTime).count();

        logInfo("Octree construction complete: " + std::to_string(m_stats.totalNodes) +
                " nodes, " + std::to_string(m_stats.leafNodes) + " leaves, " +
                std::to_string(m_stats.intersectingLeaves) + " intersecting, " +
                std::to_string(m_stats.totalCornerQueries) + " corner queries, " +
                std::to_string(m_stats.totalConstructionTimeMs) + " ms");
    }

    void HierarchicalOctreeBuilder::buildInitialOctree()
    {
        // Level 0: Create root node
        m_nodes.emplace_back();
        OctreeNode & root = m_nodes.back();
        root.bounds = m_rootBounds;
        root.depth = 0U;
        root.isLeaf = true;
        root.isIntersecting = false;

        m_levels.emplace_back();
        m_levels.back().nodeIndices.push_back(0U);
        m_levels.back().depth = 0U;

        m_stats.totalNodes = 1U;

        // Build levels breadth-first up to initialDepth
        for (std::size_t depth = 0U; depth < m_config.initialDepth; ++depth)
        {
            if (depth >= m_levels.size())
            {
                break; // No more nodes to process
            }

            processLevel(depth);

            // Create next level if any nodes need subdivision
            if (depth + 1U < m_config.initialDepth)
            {
                createChildLevel(depth);
            }
        }

        m_stats.deepestLevel = m_levels.size() - 1U;
    }

    void HierarchicalOctreeBuilder::processLevel(std::size_t levelIndex)
    {
        if (levelIndex >= m_levels.size())
        {
            return;
        }

        auto const & level = m_levels[levelIndex];
        if (level.nodeIndices.empty())
        {
            return;
        }

        logInfo("Processing level " + std::to_string(levelIndex) + ": " +
                std::to_string(level.nodeIndices.size()) + " nodes");

        // Step 1: Evaluate corners for all nodes at this level
        evaluateCorners(level.nodeIndices);

        // Step 2: Detect which nodes intersect the surface
        detectIntersections(level.nodeIndices);
    }

    void HierarchicalOctreeBuilder::evaluateCorners(
      std::vector<std::size_t> const & nodeIndices)
    {
        if (nodeIndices.empty())
        {
            return;
        }

        if (m_config.enableGpuAcceleration && evaluateCornersGPU(nodeIndices))
        {
            return;
        }

        if (!ensureCpuSampler())
        {
            logError("CPU corner evaluation fallback failed to initialize SDF sampler");

            for (std::size_t nodeIdx : nodeIndices)
            {
                OctreeNode & node = m_nodes[nodeIdx];
                node.isIntersecting = false;
                for (auto & value : node.cornerValues)
                {
                    value = std::numeric_limits<float>::quiet_NaN();
                }
            }
            return;
        }

        for (std::size_t nodeIdx : nodeIndices)
        {
            OctreeNode & node = m_nodes[nodeIdx];
            for (std::uint8_t c = 0U; c < 8U; ++c)
            {
                Eigen::Vector3f const position = cornerPosition(c, node.bounds);
                node.cornerValues[c] = sampleSdfCpu(position);
            }
        }
        m_stats.totalCornerQueries += nodeIndices.size() * 8U;
    }

    void HierarchicalOctreeBuilder::detectIntersections(
      std::vector<std::size_t> const & nodeIndices)
    {
        if (nodeIndices.empty())
        {
            return;
        }

        bool usedGpu = false;

        if (m_config.enableGpuAcceleration)
        {
            auto * program = m_core->getProgramManager().getHierarchicalDCProgram();
            if (program != nullptr)
            {
                try
                {
                    std::vector<float> packedCornerValues;
                    packedCornerValues.reserve(nodeIndices.size() * 8U);

                    for (std::size_t idx : nodeIndices)
                    {
                        OctreeNode const & node = m_nodes[idx];
                        packedCornerValues.insert(packedCornerValues.end(),
                                                  node.cornerValues.begin(),
                                                  node.cornerValues.end());
                    }

                    std::vector<std::uint8_t> subdivisionFlags;
                    program->detectIntersections(packedCornerValues, subdivisionFlags);

                    if (subdivisionFlags.size() == nodeIndices.size())
                    {
                        for (std::size_t i = 0U; i < nodeIndices.size(); ++i)
                        {
                            std::size_t const nodeIdx = nodeIndices[i];
                            OctreeNode & node = m_nodes[nodeIdx];

                            bool const intersects = subdivisionFlags[i] != 0U;
                            node.isIntersecting = intersects;
                            if (intersects && node.depth < m_config.initialDepth - 1U)
                            {
                                node.needsRefinement = true;
                            }
                            else if (!intersects)
                            {
                                node.needsRefinement = false;
                            }
                        }
                        usedGpu = true;
                    }
                    else
                    {
                        logError("detectIntersections GPU mismatch in flag count");
                    }
                }
                catch (std::exception const & ex)
                {
                    logError("GPU intersection detection failed: " + std::string(ex.what()));
                }
            }
            else
            {
                logError("HierarchicalDCProgram not available for intersection detection");
            }
        }

        if (!usedGpu)
        {
            for (std::size_t nodeIdx : nodeIndices)
            {
                OctreeNode & node = m_nodes[nodeIdx];
                node.isIntersecting = hasSignChange(node);

                if (node.isIntersecting && node.depth < m_config.initialDepth - 1U)
                {
                    node.needsRefinement = true;
                }
                else if (!node.isIntersecting)
                {
                    node.needsRefinement = false;
                }
            }
        }
    }

    void HierarchicalOctreeBuilder::createChildLevel(std::size_t parentLevelIndex)
    {
        if (parentLevelIndex >= m_levels.size())
        {
            return;
        }

        auto const & parentLevel = m_levels[parentLevelIndex];
        std::size_t const childDepth = parentLevel.depth + 1U;

        // Create new level
        if (m_levels.size() <= parentLevelIndex + 1U)
        {
            m_levels.emplace_back();
            m_levels.back().depth = static_cast<std::uint8_t>(childDepth);
        }

        auto & childLevel = m_levels[parentLevelIndex + 1U];
        childLevel.nodeIndices.clear();

        // Subdivide nodes that need refinement
        for (std::size_t nodeIdx : parentLevel.nodeIndices)
        {
            OctreeNode & node = m_nodes[nodeIdx];

            if (!node.needsRefinement)
            {
                continue;
            }

            // Mark as internal node
            node.isLeaf = false;

            // Create 8 children
            Eigen::Vector3f const center = boundingBoxCenter(node.bounds);

            for (std::uint8_t childIdx = 0U; childIdx < 8U; ++childIdx)
            {
                std::size_t const childNodeIdx = m_nodes.size();
                node.childIndices[childIdx] = childNodeIdx;

                OctreeNode child;
                child.depth = static_cast<std::uint8_t>(childDepth);
                child.isLeaf = true;
                child.isIntersecting = false;

                // Compute child bounds
                float const xMin =
                  (childIdx & 1) ? center.x() : node.bounds.min.s[0];
                float const xMax =
                  (childIdx & 1) ? node.bounds.max.s[0] : center.x();
                float const yMin =
                  (childIdx & 2) ? center.y() : node.bounds.min.s[1];
                float const yMax =
                  (childIdx & 2) ? node.bounds.max.s[1] : center.y();
                float const zMin =
                  (childIdx & 4) ? center.z() : node.bounds.min.s[2];
                float const zMax =
                  (childIdx & 4) ? node.bounds.max.s[2] : center.z();

                child.bounds =
                  makeBoundingBox(Eigen::Vector3f{xMin, yMin, zMin},
                                  Eigen::Vector3f{xMax, yMax, zMax});

                m_nodes.push_back(child);
                childLevel.nodeIndices.push_back(childNodeIdx);
                ++m_stats.totalNodes;
            }
        }

        logInfo("Created level " + std::to_string(childDepth) + ": " +
                std::to_string(childLevel.nodeIndices.size()) + " nodes");
    }

    void HierarchicalOctreeBuilder::refineAdaptively()
    {
        logInfo("Starting adaptive refinement: " +
                std::to_string(m_config.refinementIterations) + " passes");

        for (std::size_t pass = 0U; pass < m_config.refinementIterations; ++pass)
        {
            // Collect current leaf nodes
            std::vector<std::size_t> leafIndices = getLeafIndices();

            // Filter to intersecting leaves only
            std::vector<std::size_t> intersectingLeaves;
            for (std::size_t idx : leafIndices)
            {
                if (m_nodes[idx].isIntersecting)
                {
                    intersectingLeaves.push_back(idx);
                }
            }

            if (intersectingLeaves.empty())
            {
                break;
            }

            logInfo("Refinement pass " + std::to_string(pass + 1U) + ": " +
                    std::to_string(intersectingLeaves.size()) + " intersecting leaves");

            // Estimate curvature for all intersecting leaves
            estimateCurvature(intersectingLeaves);

            // Mark high-curvature leaves for subdivision
            std::size_t markedCount = 0U;
            for (std::size_t idx : intersectingLeaves)
            {
                OctreeNode & node = m_nodes[idx];
                if (node.curvatureMetric > m_config.curvatureThreshold &&
                    node.depth < m_config.maxDepth)
                {
                    node.needsRefinement = true;
                    ++markedCount;
                }
            }

            logInfo("Marked " + std::to_string(markedCount) + " leaves for subdivision");

            if (markedCount == 0U)
            {
                break; // No more refinement needed
            }

            // Subdivide marked leaves
            subdivideMarkedLeaves();

            // Process newly created leaves
            std::vector<std::size_t> newLeaves = getLeafIndices();
            evaluateCorners(newLeaves);
            detectIntersections(newLeaves);

            ++m_stats.refinementPasses;
        }
    }

    void HierarchicalOctreeBuilder::estimateCurvature(
      std::vector<std::size_t> const & leafIndices)
    {
        if (leafIndices.empty())
        {
            return;
        }

        if (m_config.enableGpuAcceleration && estimateCurvatureGPU(leafIndices))
        {
            return;
        }

        if (!ensureCpuSampler())
        {
            logError("CPU curvature estimation fallback failed to initialize SDF sampler");
            for (std::size_t idx : leafIndices)
            {
                OctreeNode & node = m_nodes[idx];
                node.curvatureMetric = 0.0F;
            }
            return;
        }

        std::array<Eigen::Vector3f, 6> const offsets = {Eigen::Vector3f{1.0F, 0.0F, 0.0F},
                                                        Eigen::Vector3f{-1.0F, 0.0F, 0.0F},
                                                        Eigen::Vector3f{0.0F, 1.0F, 0.0F},
                                                        Eigen::Vector3f{0.0F, -1.0F, 0.0F},
                                                        Eigen::Vector3f{0.0F, 0.0F, 1.0F},
                                                        Eigen::Vector3f{0.0F, 0.0F, -1.0F}};

        for (std::size_t idx : leafIndices)
        {
            OctreeNode & node = m_nodes[idx];
            Eigen::Vector3f const center = boundingBoxCenter(node.bounds);
            Eigen::Vector3f const centerGradient =
              sampleGradientCpu(center, m_config.gradientEpsilon);

            float curvature = 0.0F;
            for (auto const & offset : offsets)
            {
                Eigen::Vector3f const neighborPos =
                  center + offset * m_config.gradientEpsilon;
                Eigen::Vector3f const neighborGradient =
                  sampleGradientCpu(neighborPos, m_config.gradientEpsilon);
                Eigen::Vector3f const diff = centerGradient - neighborGradient;
                curvature += diff.squaredNorm();
            }

            node.curvatureMetric = curvature / static_cast<float>(offsets.size());
        }

        m_stats.totalGradientQueries += leafIndices.size() * 42U;
    }

    void HierarchicalOctreeBuilder::subdivideMarkedLeaves()
    {
        // Collect leaves that need subdivision
        std::vector<std::size_t> toSubdivide;
        for (std::size_t idx = 0U; idx < m_nodes.size(); ++idx)
        {
            if (m_nodes[idx].isLeaf && m_nodes[idx].needsRefinement)
            {
                toSubdivide.push_back(idx);
            }
        }

        for (std::size_t nodeIdx : toSubdivide)
        {
            OctreeNode & node = m_nodes[nodeIdx];
            node.isLeaf = false;
            node.needsRefinement = false;

            Eigen::Vector3f const center = boundingBoxCenter(node.bounds);
            std::uint8_t const childDepth = node.depth + 1U;

            for (std::uint8_t childIdx = 0U; childIdx < 8U; ++childIdx)
            {
                std::size_t const childNodeIdx = m_nodes.size();
                node.childIndices[childIdx] = childNodeIdx;

                OctreeNode child;
                child.depth = childDepth;
                child.isLeaf = true;

                float const xMin = (childIdx & 1) ? center.x() : node.bounds.min.s[0];
                float const xMax = (childIdx & 1) ? node.bounds.max.s[0] : center.x();
                float const yMin = (childIdx & 2) ? center.y() : node.bounds.min.s[1];
                float const yMax = (childIdx & 2) ? node.bounds.max.s[1] : center.y();
                float const zMin = (childIdx & 4) ? center.z() : node.bounds.min.s[2];
                float const zMax = (childIdx & 4) ? node.bounds.max.s[2] : center.z();

                child.bounds =
                  makeBoundingBox(Eigen::Vector3f{xMin, yMin, zMin},
                                  Eigen::Vector3f{xMax, yMax, zMax});

                m_nodes.push_back(child);
                ++m_stats.totalNodes;
            }
        }
    }

    void HierarchicalOctreeBuilder::refineZeroCrossings()
    {
        std::vector<std::size_t> const leafIndices = getLeafIndices();

        for (auto idx : leafIndices)
        {
            m_nodes[idx].hermiteSamples.clear();
        }

        if (leafIndices.empty())
        {
            logInfo("Zero-crossing refinement skipped (no leaves)");
            return;
        }

        std::vector<EdgeCrossing> const crossings = gatherEdgeCrossings(leafIndices);
        if (crossings.empty())
        {
            logInfo("Zero-crossing refinement skipped (no intersecting edges)");
            return;
        }

        auto primitives = m_core->getPrimitives();
        if (!primitives)
        {
            logError("Primitives buffer unavailable for zero-crossing refinement");
            return;
        }

        std::vector<Eigen::Vector3f> refinedPositions(crossings.size());
        bool gpuRefined = false;

        if (m_config.enableGpuAcceleration)
        {
            auto * program = m_core->getProgramManager().getHierarchicalDCProgram();
            if (program != nullptr)
            {
                try
                {
                    std::vector<Eigen::Vector3f> edgeStarts;
                    std::vector<Eigen::Vector3f> edgeEnds;
                    std::vector<float> startValues;
                    std::vector<float> endValues;
                    edgeStarts.reserve(crossings.size());
                    edgeEnds.reserve(crossings.size());
                    startValues.reserve(crossings.size());
                    endValues.reserve(crossings.size());

                    for (auto const & crossing : crossings)
                    {
                        edgeStarts.push_back(crossing.startPos);
                        edgeEnds.push_back(crossing.endPos);
                        startValues.push_back(crossing.startValue);
                        endValues.push_back(crossing.endValue);
                    }

                    program->refineZeroCrossings(edgeStarts,
                                                 edgeEnds,
                                                 startValues,
                                                 endValues,
                                                 refinedPositions,
                                                 *primitives,
                                                 m_config.isoValue,
                                                 static_cast<std::uint32_t>(m_config.maxBisectionIterations),
                                                 m_config.zeroCrossingTolerance);
                    gpuRefined = true;
                }
                catch (std::exception const & ex)
                {
                    logError("GPU zero-crossing refinement failed: " + std::string(ex.what()));
                }
            }
            else
            {
                logError("HierarchicalDCProgram not available for zero-crossing refinement");
            }
        }

        if (!gpuRefined)
        {
            for (std::size_t i = 0U; i < crossings.size(); ++i)
            {
                auto const & crossing = crossings[i];
                float const denominator = crossing.startValue - crossing.endValue;
                float t = 0.5F;
                if (std::abs(denominator) > 1e-6F)
                {
                    t = crossing.startValue / denominator;
                }
                t = std::clamp(t, 0.0F, 1.0F);
                refinedPositions[i] = crossing.startPos +
                                      (crossing.endPos - crossing.startPos) * t;
            }
        }

        std::vector<HermiteSample> const hermiteSamples =
          computeHermiteSamples(crossings, refinedPositions);

        for (std::size_t i = 0U; i < crossings.size() && i < hermiteSamples.size(); ++i)
        {
            OctreeNode & node = m_nodes[crossings[i].nodeIndex];
            if (!node.isLeaf)
            {
                continue;
            }
            node.hermiteSamples.push_back(hermiteSamples[i]);
        }

        if (!hermiteSamples.empty())
        {
            m_stats.totalGradientQueries += hermiteSamples.size();
        }
    }

    void HierarchicalOctreeBuilder::solveQEFVertices()
    {
        dual_contouring::QuadraticErrorFunction qef;

        m_stats.leafNodes = 0U;
        m_stats.intersectingLeaves = 0U;

        for (auto & node : m_nodes)
        {
            if (!node.isLeaf)
            {
                continue;
            }

            ++m_stats.leafNodes;
            node.hasVertex = false;
            node.vertexPosition.reset();
            node.vertexNormal = Eigen::Vector3f::Zero();
            node.vertexResidual = 0.0F;

            if (!node.isIntersecting || node.hermiteSamples.empty())
            {
                continue;
            }

            ++m_stats.intersectingLeaves;

            qef.reset();
            Eigen::Vector3f normalAccumulator = Eigen::Vector3f::Zero();

            for (auto const & sample : node.hermiteSamples)
            {
                Eigen::Vector3f normal = sample.gradient;
                if (!normal.allFinite() || normal.squaredNorm() <= 1e-12F)
                {
                    continue;
                }
                normal.normalize();
                qef.addSample(sample.position, normal);
                normalAccumulator += normal;
            }

            if (qef.sampleCount() < 3U)
            {
                continue;
            }

            Eigen::Vector3f vertex = Eigen::Vector3f::Zero();
            float residual = 0.0F;

            if (!qef.solveWithinBounds(toAxisAlignedBoundingBox(node.bounds), vertex, residual))
            {
                continue;
            }

            if (!vertex.allFinite())
            {
                continue;
            }

            node.vertexPosition = vertex;
            node.vertexResidual = residual;
            node.hasVertex = true;

            if (normalAccumulator.squaredNorm() > 1e-8F)
            {
                node.vertexNormal = normalAccumulator.normalized();
            }
        }
    }

    std::vector<std::size_t> HierarchicalOctreeBuilder::getLeafIndices() const
    {
        std::vector<std::size_t> leaves;
        for (std::size_t idx = 0U; idx < m_nodes.size(); ++idx)
        {
            if (m_nodes[idx].isLeaf)
            {
                leaves.push_back(idx);
            }
        }
        return leaves;
    }

    void HierarchicalOctreeBuilder::extractMesh(std::vector<Eigen::Vector3f> & outVertices,
                                                std::vector<std::uint32_t> & outIndices)
    {
        outVertices.clear();
        outIndices.clear();

        std::vector<std::size_t> const leafIndices = getLeafIndices();
        if (leafIndices.empty())
        {
            return;
        }

        for (auto idx : leafIndices)
        {
            OctreeNode const & node = m_nodes[idx];
            if (!node.hasVertex || !node.vertexPosition.has_value())
            {
                continue;
            }

            Eigen::Vector3f normal = node.vertexNormal;
            if (!normal.allFinite() || normal.squaredNorm() <= 1e-10F)
            {
                normal = Eigen::Vector3f::UnitX();
            }
            normal.normalize();

            Eigen::Vector3f tangent = normal.unitOrthogonal();
            Eigen::Vector3f bitangent = normal.cross(tangent);
            if (!bitangent.allFinite() || bitangent.squaredNorm() <= 1e-10F)
            {
                bitangent = normal.cross(Eigen::Vector3f::UnitY());
            }
            if (bitangent.squaredNorm() <= 1e-10F)
            {
                bitangent = normal.cross(Eigen::Vector3f::UnitZ());
            }
            if (bitangent.squaredNorm() <= 1e-10F)
            {
                bitangent = Eigen::Vector3f::UnitY();
            }
            tangent.normalize();
            bitangent.normalize();

            Eigen::Vector3f const extent = toEigen(node.bounds.max) - toEigen(node.bounds.min);
            float const scale = 0.25F * std::max({extent.x(), extent.y(), extent.z(), 1e-4F});

            Eigen::Vector3f const center = node.vertexPosition.value();
            Eigen::Vector3f const v0 = center + tangent * scale;
            Eigen::Vector3f const v1 = center - tangent * scale;
            Eigen::Vector3f const v2 = center + bitangent * scale;
            Eigen::Vector3f const v3 = center - bitangent * scale;

            std::uint32_t const base = static_cast<std::uint32_t>(outVertices.size());
            outVertices.push_back(v0);
            outVertices.push_back(v2);
            outVertices.push_back(v1);
            outVertices.push_back(v3);

            outIndices.push_back(base + 0U);
            outIndices.push_back(base + 1U);
            outIndices.push_back(base + 2U);

            outIndices.push_back(base + 2U);
            outIndices.push_back(base + 1U);
            outIndices.push_back(base + 3U);
        }
    }

    bool HierarchicalOctreeBuilder::evaluateCornersGPU(
      std::vector<std::size_t> const & nodeIndices)
    {
        if (nodeIndices.empty())
        {
            return true;
        }

        try
        {
            auto * program = m_core->getProgramManager().getHierarchicalDCProgram();
            if (!program)
            {
                logError("HierarchicalDCProgram not available");
                return false;
            }

            // Prepare bounds arrays
            std::vector<Eigen::Vector3f> boundsMin;
            std::vector<Eigen::Vector3f> boundsMax;
            boundsMin.reserve(nodeIndices.size());
            boundsMax.reserve(nodeIndices.size());

            for (std::size_t idx : nodeIndices)
            {
                OctreeNode const & node = m_nodes[idx];
                boundsMin.push_back(toEigen(node.bounds.min));
                boundsMax.push_back(toEigen(node.bounds.max));
            }

            std::vector<float> cornerValues;
            program->evaluateOctreeLevel(boundsMin,
                                         boundsMax,
                                         cornerValues,
                                         *m_core->getPrimitives(),
                                         m_config.isoValue);

            for (std::size_t i = 0U; i < nodeIndices.size(); ++i)
            {
                std::size_t const idx = nodeIndices[i];
                OctreeNode & node = m_nodes[idx];

                for (std::uint8_t c = 0U; c < 8U; ++c)
                {
                    node.cornerValues[c] = cornerValues[i * 8U + c];
                }
            }

            m_stats.totalCornerQueries += nodeIndices.size() * 8U;
            return true;
        }
        catch (std::exception const & ex)
        {
            logError("GPU corner evaluation failed: " + std::string(ex.what()));
            return false;
        }
    }

    bool HierarchicalOctreeBuilder::estimateCurvatureGPU(
      std::vector<std::size_t> const & leafIndices)
    {
        if (leafIndices.empty())
        {
            return true;
        }

        try
        {
            auto * program = m_core->getProgramManager().getHierarchicalDCProgram();
            if (!program)
            {
                logError("HierarchicalDCProgram not available");
                return false;
            }

            std::vector<Eigen::Vector3f> leafCenters;
            leafCenters.reserve(leafIndices.size());

            for (std::size_t idx : leafIndices)
            {
                OctreeNode const & node = m_nodes[idx];
                leafCenters.push_back(boundingBoxCenter(node.bounds));
            }

            std::vector<float> curvatureMetrics;
            program->estimateCurvature(leafCenters,
                                       curvatureMetrics,
                                       *m_core->getPrimitives(),
                                       m_config.gradientEpsilon);

            for (std::size_t i = 0U; i < leafIndices.size(); ++i)
            {
                std::size_t const idx = leafIndices[i];
                OctreeNode & node = m_nodes[idx];
                node.curvatureMetric = curvatureMetrics[i];
            }

            m_stats.totalGradientQueries += leafIndices.size() * 42U; // 7 positions × 6 samples
            return true;
        }
        catch (std::exception const & ex)
        {
            logError("GPU curvature estimation failed: " + std::string(ex.what()));
            return false;
        }
    }

    Eigen::Vector3f HierarchicalOctreeBuilder::cornerPosition(std::uint8_t cornerIndex,
                                                              BoundingBox const & bounds) const
    {
        float const x = (cornerIndex & 1) ? bounds.max.s[0] : bounds.min.s[0];
        float const y = (cornerIndex & 2) ? bounds.max.s[1] : bounds.min.s[1];
        float const z = (cornerIndex & 4) ? bounds.max.s[2] : bounds.min.s[2];
        return Eigen::Vector3f{x, y, z};
    }

    bool HierarchicalOctreeBuilder::hasSignChange(OctreeNode const & node) const
    {
        for (auto const & [c0, c1] : EDGE_CORNERS)
        {
            if (node.cornerValues[c0] * node.cornerValues[c1] < 0.0F)
            {
                return true;
            }
        }
        return false;
    }

    std::vector<EdgeCrossing> HierarchicalOctreeBuilder::gatherEdgeCrossings(
      std::vector<std::size_t> const & leafIndices) const
    {
        std::vector<EdgeCrossing> crossings;

        for (std::size_t idx : leafIndices)
        {
            OctreeNode const & node = m_nodes[idx];
            if (!node.isIntersecting)
            {
                continue;
            }

            for (std::uint8_t edgeIdx = 0U; edgeIdx < 12U; ++edgeIdx)
            {
                auto const [c0, c1] = EDGE_CORNERS[edgeIdx];
                float const v0 = node.cornerValues[c0];
                float const v1 = node.cornerValues[c1];

                if (v0 * v1 < 0.0F) // Sign change
                {
                    EdgeCrossing crossing;
                    crossing.startPos = cornerPosition(c0, node.bounds);
                    crossing.endPos = cornerPosition(c1, node.bounds);
                    crossing.startValue = v0;
                    crossing.endValue = v1;
                    crossing.nodeIndex = idx;
                    crossing.edgeIndex = edgeIdx;
                    crossings.push_back(crossing);
                }
            }
        }

        return crossings;
    }

        std::vector<HermiteSample> HierarchicalOctreeBuilder::computeHermiteSamples(
            std::vector<EdgeCrossing> const & crossings,
            std::vector<Eigen::Vector3f> const & refinedPositions)
    {
        std::vector<HermiteSample> samples;
        samples.reserve(crossings.size());

        if (crossings.empty())
        {
            return samples;
        }

        auto primitives = m_core->getPrimitives();
        if (!primitives)
        {
            logError("Primitives buffer unavailable for Hermite sampling");
            return samples;
        }

        std::vector<Eigen::Vector3f> positions = refinedPositions;
        if (positions.size() != crossings.size())
        {
            positions.resize(crossings.size());
            for (std::size_t i = 0U; i < crossings.size(); ++i)
            {
                float const denominator = crossings[i].startValue - crossings[i].endValue;
                float t = 0.5F;
                if (std::abs(denominator) > 1e-6F)
                {
                    t = crossings[i].startValue / denominator;
                }
                t = std::clamp(t, 0.0F, 1.0F);
                positions[i] = crossings[i].startPos +
                               (crossings[i].endPos - crossings[i].startPos) * t;
            }
        }

        std::vector<float> values;
        std::vector<Eigen::Vector3f> gradients;
        bool gpuSampled = false;

        if (m_config.enableGpuAcceleration)
        {
            auto * samplingProgram =
              m_core->getProgramManager().getDualContouringSamplingProgram();
            if (samplingProgram != nullptr)
            {
                try
                {
                    samplingProgram->sampleHermite(positions,
                                                   values,
                                                   gradients,
                                                   *primitives,
                                                   m_config.isoValue,
                                                   m_config.gradientEpsilon);
                    gpuSampled = values.size() == positions.size() &&
                                 gradients.size() == positions.size();
                }
                catch (std::exception const & ex)
                {
                    logError("Hermite sampling failed: " + std::string(ex.what()));
                }
            }
        }

        if (!gpuSampled)
        {
            values.clear();
            gradients.clear();

            if (ensureCpuSampler())
            {
                values.reserve(positions.size());
                gradients.reserve(positions.size());

                for (auto const & position : positions)
                {
                    values.push_back(sampleSdfCpu(position));
                    gradients.push_back(sampleGradientCpu(position, m_config.gradientEpsilon));
                }

                m_stats.totalGradientQueries += positions.size() * 6U;
            }
            else
            {
                try
                {
                    auto * samplingProgram =
                      m_core->getProgramManager().getDualContouringSamplingProgram();
                    if (samplingProgram != nullptr)
                    {
                        samplingProgram->sampleCorners(positions,
                                                       values,
                                                       *primitives,
                                                       m_config.isoValue);
                    }

                    auto * dcProgram = m_core->getProgramManager().getHierarchicalDCProgram();
                    if (dcProgram != nullptr)
                    {
                        dcProgram->batchGradients(positions,
                                                  gradients,
                                                  *primitives,
                                                  m_config.gradientEpsilon);
                    }
                }
                catch (std::exception const & ex)
                {
                    logError("Fallback Hermite sampling failed: " + std::string(ex.what()));
                }
            }
        }

        for (std::size_t i = 0U; i < positions.size(); ++i)
        {
            HermiteSample sample;
            sample.position = positions[i];
            sample.value = (i < values.size()) ? values[i] : 0.0F;
            sample.gradient = (i < gradients.size()) ? gradients[i] : Eigen::Vector3f::Zero();
            samples.push_back(sample);
        }

        return samples;
    }

    bool HierarchicalOctreeBuilder::ensureCpuSampler()
    {
        if (m_core == nullptr)
        {
            return false;
        }

        std::size_t const targetResolution = std::max<std::size_t>(m_config.cpuFallbackResolution, 2U);
        BoundingBox const requestedBounds = m_rootBounds;

        if (!m_cpuSampler)
        {
            m_cpuSampler = std::make_unique<CpuSampler>();
        }

        if (m_cpuSampler->initialized &&
            m_cpuSampler->resolution == targetResolution &&
            boundingBoxesApproximatelyEqual(m_cpuSampler->bounds, requestedBounds))
        {
            return true;
        }

        return m_cpuSampler->initialize(*m_core, requestedBounds, targetResolution);
    }

    float HierarchicalOctreeBuilder::sampleSdfCpu(Eigen::Vector3f const & position) const
    {
        if (!m_cpuSampler || !m_cpuSampler->initialized)
        {
            return 0.0F;
        }

        return m_cpuSampler->sample(position);
    }

    Eigen::Vector3f HierarchicalOctreeBuilder::sampleGradientCpu(Eigen::Vector3f const & position,
                                                                 float epsilon) const
    {
        if (!m_cpuSampler || !m_cpuSampler->initialized)
        {
            return Eigen::Vector3f::Zero();
        }

        return m_cpuSampler->gradient(position, epsilon);
    }

    void HierarchicalOctreeBuilder::logInfo(std::string const & message) const
    {
        if (m_logger)
        {
            m_logger->addEvent({"[HierarchicalDC] " + message, events::Severity::Info});
        }
    }

    void HierarchicalOctreeBuilder::logError(std::string const & message) const
    {
        if (m_logger)
        {
            m_logger->addEvent({"[HierarchicalDC] " + message, events::Severity::Error});
        }
    }
}
