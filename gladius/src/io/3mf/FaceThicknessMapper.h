/**
 * @file FaceThicknessMapper.h
 * @brief Maps per-face colors to per-face layer thicknesses for HueForge-style export
 *
 * This class takes sampled face colors and uses the FrontlitThicknessSolver to compute
 * the optimal thickness for each filament layer at each face. The result is a per-face
 * thickness array that can be used to generate offset geometry for multi-material export.
 */

#pragma once

#include "FilamentOpticalProperties.h"
#include "FrontlitThicknessSolver.h"

#include <eigen3/Eigen/Core>

#include <functional>
#include <limits>
#include <vector>

namespace gladius::io
{

    /**
     * @brief Result of mapping face colors to thicknesses
     */
    struct FaceThicknessResult
    {
        /// Per-face thicknesses for each layer
        /// Outer vector: one entry per filament layer (bottom to top)
        /// Inner vector: one thickness value per face
        std::vector<std::vector<float>> layerThicknesses;

        /// Per-face achieved colors (what the thickness combination produces)
        std::vector<Eigen::Vector3f> achievedColors;

        /// Per-face color errors (distance between target and achieved)
        std::vector<float> colorErrors;

        /// Fraction of faces that converged to acceptable error
        float convergenceRate = 0.0f;

        /// Average color error across all faces
        float averageError = 0.0f;

        /// Maximum color error across all faces
        float maxError = 0.0f;

        [[nodiscard]] std::size_t numFaces() const
        {
            return colorErrors.size();
        }

        [[nodiscard]] std::size_t numLayers() const
        {
            return layerThicknesses.size();
        }
    };

    /**
     * @brief Maps per-face colors to per-face layer thicknesses
     *
     * Uses FrontlitThicknessSolver to compute optimal thicknesses for each face,
     * with optional spatial smoothing to reduce visible seams between adjacent faces.
     */
    class FaceThicknessMapper
    {
      public:
        /// Progress callback: receives (completed faces, total faces)
        using ProgressCallback = std::function<void(std::size_t completed, std::size_t total)>;

        /**
         * @brief Construct mapper with a filament stack and constraints
         *
         * @param stack Filament stack (bottom to top order)
         * @param constraints Thickness constraints for optimization
         */
        explicit FaceThicknessMapper(FilamentStack const& stack,
                                     ThicknessConstraints const& constraints = {},
                                     std::size_t backgroundIndex = std::numeric_limits<std::size_t>::max());

        /**
         * @brief Map per-face colors to per-face thicknesses
         *
         * For each face, solves the inverse problem to find the layer thicknesses
         * that best reproduce the target color.
         *
         * @param faceColors Per-face colors in linear RGB [0,1]
         * @param progressCallback Optional callback for progress reporting
         * @return Result with per-face thicknesses for each layer
         */
        [[nodiscard]] FaceThicknessResult mapColors(
            std::vector<Eigen::Vector3f> const& faceColors,
            ProgressCallback progressCallback = nullptr) const;

        /**
         * @brief Map per-face colors with optional spatial smoothing
         *
         * After initial per-face solve, applies Laplacian smoothing to the thickness
         * field to reduce visible seams at face boundaries.
         *
         * @param faceColors Per-face colors in linear RGB [0,1]
         * @param faceAdjacency Adjacency list: for each face, list of neighboring face indices
         * @param smoothingIterations Number of Laplacian smoothing iterations (0 = none)
         * @param smoothingWeight Blend weight for smoothing [0,1] (0 = no change, 1 = full smooth)
         * @param progressCallback Optional callback for progress reporting
         * @return Result with per-face thicknesses for each layer
         */
        [[nodiscard]] FaceThicknessResult mapColorsWithSmoothing(
            std::vector<Eigen::Vector3f> const& faceColors,
            std::vector<std::vector<std::size_t>> const& faceAdjacency,
            int smoothingIterations = 3,
            float smoothingWeight = 0.3f,
            ProgressCallback progressCallback = nullptr) const;

        /// Set acceptable color error threshold for convergence reporting
        void setAcceptableError(float error);

        /// Set background material index (in stack order)
        void setBackgroundIndex(std::size_t index);

        /// Get background material index
        [[nodiscard]] std::size_t getBackgroundIndex() const;

        /// Get the filament stack
        [[nodiscard]] FilamentStack const& getFilamentStack() const;

        /// Get the constraints
        [[nodiscard]] ThicknessConstraints const& getConstraints() const;

      private:
        /// Apply one iteration of Laplacian smoothing to thickness values
        void applySmoothingIteration(std::vector<float>& thicknesses,
                                     std::vector<std::vector<std::size_t>> const& adjacency,
                                     float weight) const;

        FilamentStack m_stack;
        ThicknessConstraints m_constraints;
        std::size_t m_backgroundIndex{std::numeric_limits<std::size_t>::max()};
        float m_acceptableError = 0.05f; // 5% color error considered acceptable
    };

    /**
     * @brief Utility to convert sRGB colors to linear RGB
     *
     * Face colors from FaceColorSampler may be in sRGB; this solver needs linear RGB.
     *
     * @param srgb sRGB color [0,1]
     * @return Linear RGB color [0,1]
     */
    [[nodiscard]] inline float srgbToLinear(float srgb)
    {
        if (srgb <= 0.04045f)
        {
            return srgb / 12.92f;
        }
        return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    }

    [[nodiscard]] inline Eigen::Vector3f srgbToLinear(Eigen::Vector3f const& srgb)
    {
        return {srgbToLinear(srgb.x()), srgbToLinear(srgb.y()), srgbToLinear(srgb.z())};
    }

    /**
     * @brief Convert linear RGB to sRGB
     *
     * @param linear Linear RGB color [0,1]
     * @return sRGB color [0,1]
     */
    [[nodiscard]] inline float linearToSrgb(float linear)
    {
        if (linear <= 0.0031308f)
        {
            return linear * 12.92f;
        }
        return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    }

    [[nodiscard]] inline Eigen::Vector3f linearToSrgb(Eigen::Vector3f const& linear)
    {
        return {linearToSrgb(linear.x()), linearToSrgb(linear.y()), linearToSrgb(linear.z())};
    }

} // namespace gladius::io

