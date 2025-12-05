/**
 * @file FaceThicknessMapper.cpp
 * @brief Implementation of per-face color to thickness mapping
 */

#include "FaceThicknessMapper.h"

#include <algorithm>
#include <cmath>

namespace gladius::io
{

    FaceThicknessMapper::FaceThicknessMapper(FilamentStack const& stack,
                                             ThicknessConstraints const& constraints)
        : m_stack(stack)
        , m_constraints(constraints)
    {
    }

    FaceThicknessResult FaceThicknessMapper::mapColors(
        std::vector<Eigen::Vector3f> const& faceColors,
        ProgressCallback progressCallback) const
    {
        std::size_t const numFaces = faceColors.size();
        std::size_t const numLayers = m_stack.size();

        FaceThicknessResult result;
        result.layerThicknesses.resize(numLayers);
        for (auto& layer : result.layerThicknesses)
        {
            layer.resize(numFaces, 0.0f);
        }
        result.achievedColors.resize(numFaces);
        result.colorErrors.resize(numFaces);

        if (numFaces == 0 || numLayers == 0)
        {
            result.convergenceRate = 1.0f;
            return result;
        }

        FrontlitThicknessSolver solver(m_stack, m_constraints);

        std::size_t convergedCount = 0;
        float totalError = 0.0f;
        float maxError = 0.0f;

        for (std::size_t faceIdx = 0; faceIdx < numFaces; ++faceIdx)
        {
            ThicknessSolution const solution = solver.solve(faceColors[faceIdx]);

            // Store results
            for (std::size_t layerIdx = 0; layerIdx < numLayers; ++layerIdx)
            {
                result.layerThicknesses[layerIdx][faceIdx] = solution.thicknesses[layerIdx];
            }
            result.achievedColors[faceIdx] = solution.achievedColor;
            result.colorErrors[faceIdx] = solution.colorError;

            // Update statistics
            if (solution.converged || solution.colorError <= m_acceptableError)
            {
                ++convergedCount;
            }
            totalError += solution.colorError;
            maxError = std::max(maxError, solution.colorError);

            // Report progress
            if (progressCallback && (faceIdx % 1000 == 0 || faceIdx == numFaces - 1))
            {
                progressCallback(faceIdx + 1, numFaces);
            }
        }

        result.convergenceRate = static_cast<float>(convergedCount) / static_cast<float>(numFaces);
        result.averageError = totalError / static_cast<float>(numFaces);
        result.maxError = maxError;

        return result;
    }

    FaceThicknessResult FaceThicknessMapper::mapColorsWithSmoothing(
        std::vector<Eigen::Vector3f> const& faceColors,
        std::vector<std::vector<std::size_t>> const& faceAdjacency,
        int smoothingIterations,
        float smoothingWeight,
        ProgressCallback progressCallback) const
    {
        // First, do the per-face solve
        FaceThicknessResult result = mapColors(faceColors, progressCallback);

        if (smoothingIterations <= 0 || smoothingWeight <= 0.0f || faceAdjacency.empty())
        {
            return result;
        }

        // Apply Laplacian smoothing to each layer's thickness field
        for (auto& layerThicknesses : result.layerThicknesses)
        {
            for (int iter = 0; iter < smoothingIterations; ++iter)
            {
                applySmoothingIteration(layerThicknesses, faceAdjacency, smoothingWeight);
            }
        }

        // Re-apply constraints after smoothing
        for (std::size_t layerIdx = 0; layerIdx < result.numLayers(); ++layerIdx)
        {
            float const minT = std::max(m_constraints.minThickness, m_stack[layerIdx].minThickness);
            float const maxT = std::min(m_constraints.maxThickness, m_stack[layerIdx].maxThickness);

            for (float& t : result.layerThicknesses[layerIdx])
            {
                t = m_constraints.constrain(t);
                t = std::clamp(t, minT, maxT);
            }
        }

        // Recompute achieved colors and errors after smoothing
        FrontlitThicknessSolver solver(m_stack, m_constraints);

        float totalError = 0.0f;
        float maxError = 0.0f;
        std::size_t convergedCount = 0;

        for (std::size_t faceIdx = 0; faceIdx < result.numFaces(); ++faceIdx)
        {
            std::vector<float> thicknesses(result.numLayers());
            for (std::size_t layerIdx = 0; layerIdx < result.numLayers(); ++layerIdx)
            {
                thicknesses[layerIdx] = result.layerThicknesses[layerIdx][faceIdx];
            }

            Eigen::Vector3f const predicted = solver.predictColor(thicknesses);
            float const error = (predicted - faceColors[faceIdx]).norm();

            result.achievedColors[faceIdx] = predicted;
            result.colorErrors[faceIdx] = error;

            totalError += error;
            maxError = std::max(maxError, error);

            if (error <= m_acceptableError)
            {
                ++convergedCount;
            }
        }

        result.convergenceRate = static_cast<float>(convergedCount) / static_cast<float>(result.numFaces());
        result.averageError = totalError / static_cast<float>(result.numFaces());
        result.maxError = maxError;

        return result;
    }

    void FaceThicknessMapper::applySmoothingIteration(
        std::vector<float>& thicknesses,
        std::vector<std::vector<std::size_t>> const& adjacency,
        float weight) const
    {
        if (thicknesses.empty() || adjacency.size() != thicknesses.size())
        {
            return;
        }

        std::vector<float> smoothed = thicknesses;

        for (std::size_t i = 0; i < thicknesses.size(); ++i)
        {
            if (adjacency[i].empty())
            {
                continue;
            }

            // Compute average of neighbors
            float sum = 0.0f;
            for (std::size_t neighborIdx : adjacency[i])
            {
                if (neighborIdx < thicknesses.size())
                {
                    sum += thicknesses[neighborIdx];
                }
            }
            float const avg = sum / static_cast<float>(adjacency[i].size());

            // Blend toward average
            smoothed[i] = thicknesses[i] * (1.0f - weight) + avg * weight;
        }

        thicknesses = std::move(smoothed);
    }

    void FaceThicknessMapper::setAcceptableError(float error)
    {
        m_acceptableError = error;
    }

    FilamentStack const& FaceThicknessMapper::getFilamentStack() const
    {
        return m_stack;
    }

    ThicknessConstraints const& FaceThicknessMapper::getConstraints() const
    {
        return m_constraints;
    }

} // namespace gladius::io

