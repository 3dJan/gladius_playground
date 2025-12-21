#pragma once

#include "ProgramBase.h"
#include "ResourceContext.h"
#include "kernel/types.h"

#include <Eigen/Core>

#include <cstdint>
#include <vector>

namespace gladius
{
    class Primitives;

    /**
     * @brief OpenCL program wrapper for hierarchical dual contouring kernels.
     */
    class HierarchicalDCProgram : public ProgramBase
    {
      public:
        HierarchicalDCProgram(SharedComputeContext context, SharedResources const & resources);

        void evaluateOctreeLevel(std::vector<Eigen::Vector3f> const & nodeBoundsMin,
                                 std::vector<Eigen::Vector3f> const & nodeBoundsMax,
                                 std::vector<float> & outCornerValues,
                                 Primitives const & primitives,
                                 float isoValue);

        void detectIntersections(std::vector<float> const & cornerValues,
                                 std::vector<std::uint8_t> & outSubdivisionFlags);

        void estimateCurvature(std::vector<Eigen::Vector3f> const & leafCenters,
                               std::vector<float> & outCurvatureMetrics,
                               Primitives const & primitives,
                               float gradientEpsilon);

        void batchGradients(std::vector<Eigen::Vector3f> const & positions,
                            std::vector<Eigen::Vector3f> & outGradients,
                            Primitives const & primitives,
                            float gradientEpsilon);

        void refineZeroCrossings(std::vector<Eigen::Vector3f> const & edgeStarts,
               std::vector<Eigen::Vector3f> const & edgeEnds,
               std::vector<float> const & startValues,
               std::vector<float> const & endValues,
               std::vector<Eigen::Vector3f> & outPositions,
               Primitives const & primitives,
               float isoValue,
               std::uint32_t maxIterations,
               float tolerance);

      private:
        void ensureCompiled();
    };
}
