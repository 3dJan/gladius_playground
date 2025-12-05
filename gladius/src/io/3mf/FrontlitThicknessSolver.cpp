/**
 * @file FrontlitThicknessSolver.cpp
 * @brief Implementation of frontlit color-to-thickness solver
 */

#include "FrontlitThicknessSolver.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gladius::io
{

    FrontlitThicknessSolver::FrontlitThicknessSolver(FilamentStack const& stack,
                                                     ThicknessConstraints const& constraints)
        : m_stack(stack)
        , m_constraints(constraints)
    {
    }

    ThicknessSolution FrontlitThicknessSolver::solve(Eigen::Vector3f const& targetColor,
                                                     ProgressCallback progressCallback) const
    {
        std::size_t const n = m_stack.size();

        if (n == 0)
        {
            ThicknessSolution solution;
            solution.targetColor = targetColor;
            solution.achievedColor = Eigen::Vector3f::Ones(); // White (no filament)
            solution.colorError = (solution.achievedColor - targetColor).squaredNorm();
            solution.converged = true;
            return solution;
        }

        // Initialize thicknesses to mid-range
        std::vector<float> thicknesses(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            float const minT = std::max(m_constraints.minThickness, m_stack[i].minThickness);
            float const maxT = std::min(m_constraints.maxThickness, m_stack[i].maxThickness);
            thicknesses[i] = (minT + maxT) * 0.5f;
        }

        // Projected gradient descent
        float prevError = std::numeric_limits<float>::max();

        for (int iter = 0; iter < m_maxIterations; ++iter)
        {
            Eigen::Vector3f const predicted = predictColor(thicknesses);
            float const error = computeError(predicted, targetColor);

            if (progressCallback)
            {
                progressCallback(iter, error);
            }

            // Check convergence
            if (std::abs(prevError - error) < m_convergenceTolerance)
            {
                ThicknessSolution solution(n);
                solution.targetColor = targetColor;
                solution.achievedColor = predicted;
                solution.thicknesses = thicknesses;
                solution.colorError = std::sqrt(error); // Return RMS error
                solution.converged = true;
                return solution;
            }
            prevError = error;

            // Compute gradient and take step
            std::vector<float> gradient = computeGradient(thicknesses, targetColor);

            // Adaptive step size with backtracking line search
            float stepSize = m_stepSize;
            std::vector<float> newThicknesses = thicknesses;

            for (int backtrack = 0; backtrack < 10; ++backtrack)
            {
                for (std::size_t i = 0; i < n; ++i)
                {
                    newThicknesses[i] = thicknesses[i] - stepSize * gradient[i];
                }
                projectOntoConstraints(newThicknesses);

                Eigen::Vector3f const newPredicted = predictColor(newThicknesses);
                float const newError = computeError(newPredicted, targetColor);

                if (newError < error)
                {
                    thicknesses = newThicknesses;
                    break;
                }
                stepSize *= 0.5f;
            }
        }

        // Max iterations reached
        Eigen::Vector3f const predicted = predictColor(thicknesses);
        ThicknessSolution solution(n);
        solution.targetColor = targetColor;
        solution.achievedColor = predicted;
        solution.thicknesses = thicknesses;
        solution.colorError = std::sqrt(computeError(predicted, targetColor));
        solution.converged = false;
        return solution;
    }

    Eigen::Vector3f FrontlitThicknessSolver::predictColor(std::vector<float> const& thicknesses) const
    {
        std::size_t const n = m_stack.size();

        if (n == 0 || thicknesses.size() != n)
        {
            return Eigen::Vector3f::Ones(); // White (no filament)
        }

        std::vector<float> const visibilities = computeVisibilities(thicknesses);

        Eigen::Vector3f result = Eigen::Vector3f::Zero();

        for (std::size_t i = 0; i < n; ++i)
        {
            result += m_stack[i].reflectanceColor * visibilities[i];
        }

        // Clamp to valid range
        return result.cwiseMax(0.0f).cwiseMin(1.0f);
    }

    std::vector<float> FrontlitThicknessSolver::computeVisibilities(
        std::vector<float> const& thicknesses) const
    {
        std::size_t const n = m_stack.size();
        std::vector<float> visibilities(n, 0.0f);

        if (n == 0 || thicknesses.size() != n)
        {
            return visibilities;
        }

        std::vector<float> const opacities = computeOpacities(thicknesses);

        // For frontlit viewing, we process from top (last) to bottom (first)
        // The top layer is fully visible based on its opacity
        // Lower layers are partially occluded by layers above them

        float remainingLight = 1.0f;

        // Process from top to bottom
        for (std::size_t i = n; i > 0; --i)
        {
            std::size_t const idx = i - 1;

            // This layer absorbs some of the remaining light proportional to its opacity
            // The "visible" portion of this layer's color is: opacity * remainingLight
            visibilities[idx] = opacities[idx] * remainingLight;

            // Light that passes through this layer to lower layers
            remainingLight *= (1.0f - opacities[idx]);
        }

        return visibilities;
    }

    std::vector<float> FrontlitThicknessSolver::computeOpacities(
        std::vector<float> const& thicknesses) const
    {
        std::size_t const n = m_stack.size();
        std::vector<float> opacities(n, 0.0f);

        for (std::size_t i = 0; i < n; ++i)
        {
            opacities[i] = m_stack[i].computeEffectiveOpacity(thicknesses[i]);
        }

        return opacities;
    }

    std::vector<float> FrontlitThicknessSolver::computeGradient(
        std::vector<float> const& thicknesses,
        Eigen::Vector3f const& targetColor) const
    {
        std::size_t const n = m_stack.size();
        std::vector<float> gradient(n, 0.0f);

        if (n == 0)
        {
            return gradient;
        }

        // Numerical gradient using central differences
        float const h = 1e-4f;

        for (std::size_t i = 0; i < n; ++i)
        {
            std::vector<float> thicknessesPlus = thicknesses;
            std::vector<float> thicknessesMinus = thicknesses;

            thicknessesPlus[i] += h;
            thicknessesMinus[i] -= h;

            Eigen::Vector3f const colorPlus = predictColor(thicknessesPlus);
            Eigen::Vector3f const colorMinus = predictColor(thicknessesMinus);

            float const errorPlus = computeError(colorPlus, targetColor);
            float const errorMinus = computeError(colorMinus, targetColor);

            gradient[i] = (errorPlus - errorMinus) / (2.0f * h);
        }

        return gradient;
    }

    void FrontlitThicknessSolver::projectOntoConstraints(std::vector<float>& thicknesses) const
    {
        float totalThickness = 0.0f;

        for (std::size_t i = 0; i < thicknesses.size(); ++i)
        {
            float const minT = std::max(m_constraints.minThickness, m_stack[i].minThickness);
            float const maxT = std::min(m_constraints.maxThickness, m_stack[i].maxThickness);

            thicknesses[i] = m_constraints.constrain(thicknesses[i]);
            thicknesses[i] = std::clamp(thicknesses[i], minT, maxT);

            totalThickness += thicknesses[i];
        }

        // Handle total thickness constraint by scaling if needed
        if (m_constraints.totalMaxThickness > 0.0f && totalThickness > m_constraints.totalMaxThickness)
        {
            float const scale = m_constraints.totalMaxThickness / totalThickness;
            for (float& t : thicknesses)
            {
                t *= scale;
                t = m_constraints.constrain(t);
            }
        }
    }

    float FrontlitThicknessSolver::computeError(Eigen::Vector3f const& predicted,
                                                Eigen::Vector3f const& target) const
    {
        return (predicted - target).squaredNorm();
    }

    void FrontlitThicknessSolver::setMaxIterations(int maxIter)
    {
        m_maxIterations = maxIter;
    }

    void FrontlitThicknessSolver::setConvergenceTolerance(float tolerance)
    {
        m_convergenceTolerance = tolerance;
    }

    FilamentStack const& FrontlitThicknessSolver::getFilamentStack() const
    {
        return m_stack;
    }

    ThicknessConstraints const& FrontlitThicknessSolver::getConstraints() const
    {
        return m_constraints;
    }

} // namespace gladius::io

