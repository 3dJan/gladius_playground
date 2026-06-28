/**
 * @file FrontlitThicknessSolver.cpp
 * @brief Implementation of frontlit color-to-thickness solver
 */

#include "FrontlitThicknessSolver.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <numeric>
#include <utility>

#include <nlopt.hpp>

namespace gladius::io
{
    namespace
    {
        void quantizeClampThicknesses(ThicknessConstraints const& constraints, std::vector<float>& values)
        {
            float const maxT = constraints.maxThickness;
            float const minT = constraints.minThickness;

            for (float& value : values)
            {
                if (value <= 0.0f)
                {
                    value = (minT > 0.0f) ? minT : 0.0f;
                    continue;
                }

                value = std::clamp(value, 0.0f, maxT);
                if (minT > 0.0f)
                {
                    value = std::max(value, minT);
                }

                if (constraints.layerHeight > 0.0f)
                {
                    value = std::round(value / constraints.layerHeight) * constraints.layerHeight;
                    value = std::clamp(value, 0.0f, maxT);
                    if (minT > 0.0f && value > 0.0f)
                    {
                        value = std::max(value, minT);
                    }
                }
            }

            if (constraints.totalMaxThickness > 0.0f)
            {
                float const sum = std::accumulate(values.begin(), values.end(), 0.0f);
                if (sum > constraints.totalMaxThickness && sum > 0.0f)
                {
                    float const scale = constraints.totalMaxThickness / sum;
                    for (float& value : values)
                    {
                        value *= scale;
                    }
                }
            }
        }
    } // namespace

    FrontlitThicknessSolver::FrontlitThicknessSolver(FilamentStack const& stack,
                                                     ThicknessConstraints const& constraints,
                                                     IlluminationMode mode,
                                                     std::size_t backgroundIndex)
        : m_stack(stack)
        , m_constraints(constraints)
        , m_mode(mode)
        , m_backgroundIndex(backgroundIndex)
    {
        if (m_backgroundIndex >= m_stack.size())
        {
            m_backgroundIndex = std::numeric_limits<std::size_t>::max();
        }
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
            solution.colorError = std::sqrt((solution.achievedColor - targetColor).squaredNorm());
            solution.converged = true;
            return solution;
        }

        Eigen::Vector3f const target = targetColor.cwiseMax(0.0f).cwiseMin(1.0f);
        auto evaluateCandidate = [this, &target](std::vector<float> const& values) {
            return computeError(predictColor(values), target);
        };

        auto refineWithNlopt = [this, &target, n](std::vector<float>& values, bool includeGlobalPass) {
            if (n == 0)
            {
                return false;
            }

            struct NloptContext
            {
                FrontlitThicknessSolver const* solver;
                Eigen::Vector3f targetColor;
            };

            NloptContext ctx{this, target};

            auto objective = [](std::vector<double> const& x, std::vector<double>& /*grad*/, void* data) {
                auto* c = static_cast<NloptContext*>(data);
                std::vector<float> xf;
                xf.reserve(x.size());
                std::transform(x.begin(), x.end(), std::back_inserter(xf), [](double v) { return static_cast<float>(v); });
                quantizeClampThicknesses(c->solver->m_constraints, xf);
                Eigen::Vector3f const predicted = c->solver->predictColor(xf);
                double err = static_cast<double>((predicted - c->targetColor).squaredNorm());

                // Penalty for exceeding total thickness if configured
                float const totalMax = c->solver->m_constraints.totalMaxThickness;
                if (totalMax > 0.0f)
                {
                    double sum = std::accumulate(x.begin(), x.end(), 0.0);
                    if (sum > static_cast<double>(totalMax))
                    {
                        double excess = sum - static_cast<double>(totalMax);
                        err += 10.0 * excess * excess;
                    }
                }

                return err;
            };

            try
            {
                std::vector<double> lb(n, 0.0);
                std::vector<double> ub(n, static_cast<double>(m_constraints.maxThickness));

                std::vector<double> x(values.begin(), values.end());
                double f = 0.0;

                if (includeGlobalPass)
                {
                    nlopt::opt globalOpt(nlopt::GN_DIRECT_L, static_cast<unsigned>(n));
                    globalOpt.set_lower_bounds(lb);
                    globalOpt.set_upper_bounds(ub);
                    globalOpt.set_min_objective(objective, &ctx);
                    globalOpt.set_maxeval(350);
                    globalOpt.set_maxtime(0.15);
                    globalOpt.optimize(x, f);
                }

                nlopt::opt localOpt(nlopt::LN_BOBYQA, static_cast<unsigned>(n));
                localOpt.set_lower_bounds(lb);
                localOpt.set_upper_bounds(ub);
                localOpt.set_min_objective(objective, &ctx);
                localOpt.set_ftol_rel(1e-6);
                localOpt.set_xtol_rel(1e-6);
                localOpt.set_maxeval(includeGlobalPass ? 300 : 200);
                localOpt.optimize(x, f);

                values.resize(x.size());
                std::transform(x.begin(), x.end(), values.begin(), [](double v) { return static_cast<float>(v); });
                quantizeClampThicknesses(m_constraints, values);
                return true;
            }
            catch (std::exception const&)
            {
                return false;
            }
        };

        auto refineWithGradient = [this, &target](std::vector<float>& thicknesses,
                                                  ProgressCallback const& callback) {
            float prevError = std::numeric_limits<float>::max();

            for (int iter = 0; iter < m_maxIterations; ++iter)
            {
                Eigen::Vector3f const predicted = predictColor(thicknesses);
                float const error = computeError(predicted, target);

                if (callback)
                {
                    callback(iter, error);
                }

                if (std::abs(prevError - error) < m_convergenceTolerance)
                {
                    return;
                }
                prevError = error;

                std::vector<float> gradient = computeGradient(thicknesses, target);
                float stepSize = m_stepSize;
                std::vector<float> newThicknesses = thicknesses;
                bool improved = false;

                for (int backtrack = 0; backtrack < 10; ++backtrack)
                {
                    for (std::size_t i = 0; i < thicknesses.size(); ++i)
                    {
                        newThicknesses[i] = thicknesses[i] - stepSize * gradient[i];
                    }
                    projectOntoConstraints(newThicknesses);

                    Eigen::Vector3f const newPredicted = predictColor(newThicknesses);
                    float const newError = computeError(newPredicted, target);

                    if (newError < error)
                    {
                        thicknesses = newThicknesses;
                        improved = true;
                        break;
                    }
                    stepSize *= 0.5f;
                }

                if (!improved)
                {
                    return;
                }
            }
        };

        std::vector<std::vector<float>> candidates;
        candidates.reserve(4U + 2U * n);

        auto addCandidate = [this, &candidates](std::vector<float> candidate) {
            quantizeClampThicknesses(m_constraints, candidate);
            candidates.push_back(std::move(candidate));
        };

        addCandidate(std::vector<float>(n, 0.0f));
        addCandidate(std::vector<float>(n, m_constraints.maxThickness * 0.25f));

        std::vector<float> weights(n, 0.0f);
        float weightSum = 0.0f;
        for (std::size_t i = 0; i < n; ++i)
        {
            Eigen::Vector3f const tint = m_stack[i].reflectanceColor.cwiseMax(0.0f).cwiseMin(1.0f);
            float const weight = std::max(0.0f, tint.dot(target));
            weights[i] = weight;
            weightSum += weight;
        }

        std::vector<float> colorBiased(n, 0.0f);
        for (std::size_t i = 0; i < n; ++i)
        {
            float const bias = (weightSum > 0.0f) ? (weights[i] / weightSum) : (1.0f / static_cast<float>(n));
            colorBiased[i] = bias * m_constraints.maxThickness;
        }
        addCandidate(colorBiased);

        for (std::size_t i = 0; i < n; ++i)
        {
            std::vector<float> singleLayer(n, 0.0f);
            singleLayer[i] = m_constraints.maxThickness;
            addCandidate(singleLayer);
        }

        std::vector<float> globalCandidate(n, m_constraints.maxThickness * 0.25f);
        bool const haveGlobalCandidate = refineWithNlopt(globalCandidate, true);
        if (haveGlobalCandidate)
        {
            addCandidate(globalCandidate);
        }

        std::vector<float> thicknesses = candidates.front();
        float bestError = std::numeric_limits<float>::max();

        for (std::size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
        {
            std::vector<float> candidate = candidates[candidateIndex];
            refineWithNlopt(candidate, false);
            refineWithGradient(candidate, nullptr);

            float const error = evaluateCandidate(candidate);
            if (error < bestError)
            {
                bestError = error;
                thicknesses = std::move(candidate);
            }
        }

        refineWithGradient(thicknesses, progressCallback);

        Eigen::Vector3f const predicted = predictColor(thicknesses);
        ThicknessSolution solution(n);
        solution.targetColor = targetColor;
        solution.achievedColor = predicted;
        solution.thicknesses = thicknesses;
        solution.colorError = std::sqrt(computeError(predicted, targetColor));
        solution.converged = std::isfinite(solution.colorError);
        return solution;
    }

    Eigen::Vector3f FrontlitThicknessSolver::predictColor(std::vector<float> const& thicknesses) const
    {
        std::size_t const n = m_stack.size();

        if (n == 0 || thicknesses.size() != n)
        {
            return Eigen::Vector3f::Ones(); // White (no filament)
        }

                // Background handling:
                // - Frontlit: default to an opaque black backing (so a single layer returns its own KM reflectance).
                // - Backlit: default to a white backlight.
                Eigen::Vector3f backgroundReflectance =
                    (m_mode == IlluminationMode::Backlit) ? Eigen::Vector3f::Ones() : Eigen::Vector3f::Zero();
                if (m_backgroundIndex < n)
                {
                        backgroundReflectance =
                            m_stack[m_backgroundIndex].reflectanceColor.cwiseMax(0.0f).cwiseMin(1.0f);
                }

        if (m_mode == IlluminationMode::Backlit)
        {
            // Beer–Lambert transmission per channel; approximate tint via filament reflectanceColor
            Eigen::Vector3f transmittance = Eigen::Vector3f::Ones();
            for (std::size_t i = 0; i < n; ++i)
            {
                Eigen::Vector3f const T_layer = m_stack[i].computePerChannelTransmittance(thicknesses[i]);
                Eigen::Vector3f const tint = m_stack[i].reflectanceColor.cwiseMax(0.0f).cwiseMin(1.0f);
                transmittance = transmittance.cwiseProduct(T_layer.cwiseProduct(tint));
            }
            return transmittance.cwiseProduct(backgroundReflectance)
                .cwiseMax(0.0f)
                .cwiseMin(1.0f);
        }
        else
        {
            // Frontlit: Kubelka–Munk stacking
            auto combine = [](Eigen::Vector3f const& topR,
                              Eigen::Vector3f const& topT,
                              Eigen::Vector3f const& backingR,
                              Eigen::Vector3f const& backingT) -> std::pair<Eigen::Vector3f, Eigen::Vector3f>
            {
                Eigen::Vector3f outR = Eigen::Vector3f::Zero();
                Eigen::Vector3f outT = Eigen::Vector3f::Zero();
                float const epsilon = 1e-6f;

                for (int c = 0; c < 3; ++c)
                {
                    float const denom = std::max(1.0f - topR[c] * backingR[c], epsilon);
                    outR[c] = topR[c] + (topT[c] * topT[c] * backingR[c]) / denom;
                    outT[c] = (topT[c] * backingT[c]) / denom;
                }

                return {outR, outT};
            };

            Eigen::Vector3f accumulatedR = backgroundReflectance;
            Eigen::Vector3f accumulatedT = Eigen::Vector3f::Zero(); // opaque backing

            for (std::size_t idx = 0; idx < n; ++idx)
            {
                if (thicknesses[idx] <= 0.0f)
                {
                    continue;
                }

                FilamentOpticalProperties::KubelkaMunkRT const rt = m_stack[idx].computeKubelkaMunkRT(thicknesses[idx]);
                std::tie(accumulatedR, accumulatedT) = combine(rt.reflectance, rt.transmittance, accumulatedR, accumulatedT);
            }

            return accumulatedR.cwiseMax(0.0f).cwiseMin(1.0f);
        }
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

        float remainingLight = 1.0f;

        if (m_mode == IlluminationMode::Frontlit)
        {
            // For frontlit viewing, we process from top (last) to bottom (first)
            // The top layer is fully visible based on its opacity
            // Lower layers are partially occluded by layers above them
            for (std::size_t i = n; i > 0; --i)
            {
                std::size_t const idx = i - 1;

                visibilities[idx] = opacities[idx] * remainingLight;
                remainingLight *= (1.0f - opacities[idx]);
            }
        }
        else
        {
            // Backlit: light enters from the bottom of the stack and travels upward
            for (std::size_t idx = 0; idx < n; ++idx)
            {
                visibilities[idx] = opacities[idx] * remainingLight;
                remainingLight *= (1.0f - opacities[idx]);
            }
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

        float const h = std::max(1e-4f,
                     m_constraints.layerHeight > 0.0f ? m_constraints.layerHeight * 0.25f : 1e-4f);

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
        quantizeClampThicknesses(m_constraints, thicknesses);
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

    void FrontlitThicknessSolver::setIlluminationMode(IlluminationMode mode)
    {
        m_mode = mode;
    }

    IlluminationMode FrontlitThicknessSolver::getIlluminationMode() const
    {
        return m_mode;
    }

    void FrontlitThicknessSolver::setBackgroundIndex(std::size_t index)
    {
        if (index < m_stack.size())
        {
            m_backgroundIndex = index;
        }
        else
        {
            m_backgroundIndex = std::numeric_limits<std::size_t>::max();
        }
    }

    std::size_t FrontlitThicknessSolver::getBackgroundIndex() const
    {
        return m_backgroundIndex;
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

