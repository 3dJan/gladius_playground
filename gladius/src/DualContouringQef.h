#pragma once

#include <Eigen/Core>

#include <cstddef>
#include <vector>

namespace gladius::dual_contouring
{
    struct AxisAlignedBoundingBox;

    /**
     * @brief Quadratic error function solver for dual contouring vertex placement.
     */
    class QuadraticErrorFunction
    {
      public:
        void reset();
        void addSample(Eigen::Vector3f const & position, Eigen::Vector3f const & normal);

        [[nodiscard]] std::size_t sampleCount() const;

        [[nodiscard]] bool solve(Eigen::Vector3f & outPosition, float & outResidual) const;
        [[nodiscard]] bool solveWithinBounds(AxisAlignedBoundingBox const & bounds,
                                             Eigen::Vector3f & outPosition,
                                             float & outResidual) const;

      private:
        struct Sample
        {
            Eigen::Vector3f position{Eigen::Vector3f::Zero()};
            Eigen::Vector3f normal{Eigen::Vector3f::Zero()};
        };

        [[nodiscard]] bool hasSufficientData() const;
        [[nodiscard]] bool computeLeastSquaresSolution(Eigen::Vector3f & outPosition) const;
        [[nodiscard]] float evaluateResidual(Eigen::Vector3f const & position) const;

        std::vector<Sample> m_samples{};
    };
}
