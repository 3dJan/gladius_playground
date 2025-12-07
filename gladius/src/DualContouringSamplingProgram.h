#pragma once

#include "ProgramBase.h"
#include "ResourceContext.h"

#include <Eigen/Core>

#include <vector>

namespace gladius
{
    class Primitives;

    /// OpenCL program for GPU-accelerated dual contouring sampling
    class DualContouringSamplingProgram : public ProgramBase
    {
      public:
        explicit DualContouringSamplingProgram(SharedComputeContext context,
                                               SharedResources const & resources);

        /// Sample SDF values at corner positions
        /// @param positions Input positions to sample
        /// @param outValues Output SDF values (must be pre-sized)
        /// @param primitives Model primitives for SDF evaluation
        /// @param isoValue ISO surface value
        void sampleCorners(std::vector<Eigen::Vector3f> const & positions,
                          std::vector<float> & outValues,
                          Primitives const & primitives,
                          float isoValue);

        /// Sample SDF values at corner positions with variable thickness from LUT
        /// @param positions Input positions to sample
        /// @param outValues Output SDF values (must be pre-sized)
        /// @param primitives Model primitives for SDF evaluation
        /// @param baseIsoValue Base ISO value
        /// @param thicknessLUT 3D LUT data (RGB -> Thickness)
        /// @param lutResolution Resolution of the LUT (e.g. 32)
        void sampleCornersVariableThickness(std::vector<Eigen::Vector3f> const & positions,
                          std::vector<float> & outValues,
                          Primitives const & primitives,
                          float baseIsoValue,
                          std::vector<float> const & thicknessLUT,
                          int lutResolution);

        /// Sample SDF values and gradients at Hermite positions
        /// @param positions Input positions to sample
        /// @param outValues Output SDF values (must be pre-sized)
        /// @param outGradients Output gradient vectors (must be pre-sized)
        /// @param primitives Model primitives for SDF evaluation
        /// @param isoValue ISO surface value
        /// @param gradientEpsilon Finite difference step size for gradient
        void sampleHermite(std::vector<Eigen::Vector3f> const & positions,
                          std::vector<float> & outValues,
                          std::vector<Eigen::Vector3f> & outGradients,
                          Primitives const & primitives,
                          float isoValue,
                          float gradientEpsilon = 0.001F);

        /// Sample volumetric colors at positions
        /// @param positions Input positions to sample
        /// @param outColors Output RGB colors in linear sRGB [0,1]
        /// @param primitives Model primitives for color evaluation
        void sampleColors(std::vector<Eigen::Vector3f> const & positions,
                         std::vector<Eigen::Vector3f> & outColors,
                         Primitives const & primitives);

      private:
        void ensureCompiled();
    };
}
