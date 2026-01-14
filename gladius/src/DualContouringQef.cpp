#include "DualContouringQef.h"

#include "DualContouringOctree.h"

#include <algorithm>
#include <limits>

#include <Eigen/SVD>

namespace gladius::dual_contouring
{
    namespace
    {
        [[nodiscard]] Eigen::Vector3f normalizeOrZero(Eigen::Vector3f const & vector)
        {
            float const length = vector.norm();
            if (length <= std::numeric_limits<float>::epsilon())
            {
                return Eigen::Vector3f::Zero();
            }

            return vector / length;
        }
    }

    void QuadraticErrorFunction::reset()
    {
        m_samples.clear();
    }

    void QuadraticErrorFunction::addSample(Eigen::Vector3f const & position, Eigen::Vector3f const & normal)
    {
        auto const normalizedNormal = normalizeOrZero(normal);
        if (normalizedNormal.isZero())
        {
            return;
        }

        Sample sample{};
        sample.position = position;
        sample.normal = normalizedNormal;
        m_samples.emplace_back(sample);
    }

    std::size_t QuadraticErrorFunction::sampleCount() const
    {
        return m_samples.size();
    }

    bool QuadraticErrorFunction::solve(Eigen::Vector3f & outPosition, float & outResidual) const
    {
        if (!computeLeastSquaresSolution(outPosition))
        {
            outPosition = Eigen::Vector3f::Constant(std::numeric_limits<float>::quiet_NaN());
            outResidual = std::numeric_limits<float>::infinity();
            return false;
        }

        outResidual = evaluateResidual(outPosition);
        return true;
    }

    bool QuadraticErrorFunction::solveWithinBounds(AxisAlignedBoundingBox const & bounds,
                                                   Eigen::Vector3f & outPosition,
                                                   float & outResidual) const
    {
        if (!computeLeastSquaresSolution(outPosition))
        {
            outPosition = Eigen::Vector3f::Constant(std::numeric_limits<float>::quiet_NaN());
            outResidual = std::numeric_limits<float>::infinity();
            return false;
        }

        Eigen::Vector3f clamped = outPosition;
        clamped.x() = std::clamp(clamped.x(), bounds.min.x(), bounds.max.x());
        clamped.y() = std::clamp(clamped.y(), bounds.min.y(), bounds.max.y());
        clamped.z() = std::clamp(clamped.z(), bounds.min.z(), bounds.max.z());

        outPosition = clamped;
        outResidual = evaluateResidual(outPosition);
        return true;
    }

    bool QuadraticErrorFunction::hasSufficientData() const
    {
        return m_samples.size() >= 3U;
    }

    bool QuadraticErrorFunction::computeLeastSquaresSolution(Eigen::Vector3f & outPosition) const
    {
        if (!hasSufficientData())
        {
            return false;
        }

        Eigen::MatrixXf A(static_cast<Eigen::Index>(m_samples.size()), 3);
        Eigen::VectorXf b(static_cast<Eigen::Index>(m_samples.size()));

        for (std::size_t i = 0U; i < m_samples.size(); ++i)
        {
            auto const index = static_cast<Eigen::Index>(i);
            auto const & sample = m_samples.at(i);
            A.row(index) = sample.normal.transpose();
            b(index) = sample.normal.dot(sample.position);
        }

        Eigen::JacobiSVD<Eigen::MatrixXf> svd(A, Eigen::ComputeThinU | Eigen::ComputeThinV);

        auto const singularValues = svd.singularValues();
        float const tolerance = std::numeric_limits<float>::epsilon() * std::max(A.rows(), A.cols()) * singularValues.array().abs().maxCoeff();
        Eigen::Vector3f const solution = svd.solve(b);

        if (!solution.allFinite())
        {
            return false;
        }

        if ((singularValues.array() > tolerance).count() < 3)
        {
            // System is underdetermined; fall back to the centroid of sample positions.
            Eigen::Vector3f centroid = Eigen::Vector3f::Zero();
            for (auto const & sample : m_samples)
            {
                centroid += sample.position;
            }
            centroid /= static_cast<float>(m_samples.size());
            outPosition = centroid;
            return true;
        }

        outPosition = solution;
        return true;
    }

    float QuadraticErrorFunction::evaluateResidual(Eigen::Vector3f const & position) const
    {
        if (m_samples.empty())
        {
            return 0.0F;
        }

        float residual = 0.0F;
        for (auto const & sample : m_samples)
        {
            float const planeDistance = sample.normal.dot(position - sample.position);
            residual += planeDistance * planeDistance;
        }

        return residual;
    }
}
