/**
 * @file FrontlitThicknessSolver.h
 * @brief Solver for computing filament layer thicknesses to reproduce target colors
 *
 * This class solves the inverse problem: given a target RGB color and a stack of
 * filaments with known optical properties, compute the thickness of each layer
 * that best approximates the target color when viewed frontlit.
 *
 * Physical model (layered visibility):
 *   C = Σᵢ (Cᵢ × Vᵢ)
 *
 * Where:
 *   - Cᵢ is the reflectance color of layer i
 *   - Vᵢ is the visibility of layer i through all layers above it
 *   - Vᵢ = (1 - Oᵢ) × Π_{j>i}(1 - Oⱼ) for layers below the top
 *   - Oᵢ = 1 - exp(-αᵢ × tᵢ) is the effective opacity of layer i
 *
 * The solver uses projected gradient descent to minimize the color error
 * subject to thickness constraints (min, max, layer height quantization).
 */

#pragma once

#include "FilamentOpticalProperties.h"

#include <eigen3/Eigen/Core>

#include <limits>
#include <functional>
#include <vector>

namespace gladius::io
{
  enum class IlluminationMode
  {
    Frontlit,
    Backlit
  };

  /**
   * @brief Solver for frontlit color-to-thickness inverse problem
   *
   * Given a target color and a stack of filaments, computes optimal thicknesses
   * for each layer to best reproduce the target color.
   */
  class FrontlitThicknessSolver
    {
      public:
        /// Progress callback: receives iteration count and current error
        using ProgressCallback = std::function<void(int iteration, float error)>;

        /**
         * @brief Construct solver for a given filament stack
         *
         * @param stack Filament stack (bottom to top order)
         * @param constraints Thickness constraints for optimization
         */
        explicit FrontlitThicknessSolver(FilamentStack const& stack,
           ThicknessConstraints const& constraints = {},
           IlluminationMode mode = IlluminationMode::Frontlit,
           std::size_t backgroundIndex = std::numeric_limits<std::size_t>::max());

        /**
         * @brief Solve for thicknesses to match a target color
         *
         * Uses projected gradient descent to find the thickness vector that
         * minimizes the squared error between predicted and target color.
         *
         * @param targetColor Target color in linear RGB [0,1]
         * @param progressCallback Optional callback for monitoring convergence
         * @return Solution with thicknesses and achieved color
         */
        [[nodiscard]] ThicknessSolution solve(Eigen::Vector3f const& targetColor,
                                              ProgressCallback progressCallback = nullptr) const;

        /**
         * @brief Predict the resulting color for given thicknesses
         *
         * Forward model: compute what color results from the given layer thicknesses.
         *
         * @param thicknesses Thickness for each filament layer (in stack order)
         * @return Predicted color in linear RGB [0,1]
         */
        [[nodiscard]] Eigen::Vector3f predictColor(std::vector<float> const& thicknesses) const;

        /**
         * @brief Compute visibility factors for each layer
         *
         * The visibility of layer i is the fraction of its color that reaches
         * the viewer, accounting for occlusion by all layers above it.
         *
         * @param thicknesses Thickness for each filament layer
         * @return Visibility factor for each layer [0,1]
         */
        [[nodiscard]] std::vector<float> computeVisibilities(std::vector<float> const& thicknesses) const;

        /// Set maximum number of optimization iterations
        void setMaxIterations(int maxIter);

        /// Set convergence tolerance (stops when error change < tolerance)
        void setConvergenceTolerance(float tolerance);

        /// Set illumination mode (frontlit/backlit)
        void setIlluminationMode(IlluminationMode mode);

        /// Get illumination mode
        [[nodiscard]] IlluminationMode getIlluminationMode() const;

        /// Set background material index (in stack order); if out of range, background is ignored
        void setBackgroundIndex(std::size_t index);

        /// Get background material index (or max if none)
        [[nodiscard]] std::size_t getBackgroundIndex() const;

        /// Get the filament stack being used
        [[nodiscard]] FilamentStack const& getFilamentStack() const;

        /// Get the constraints being used
        [[nodiscard]] ThicknessConstraints const& getConstraints() const;

      private:
        /// Compute gradient of color error with respect to thicknesses
        [[nodiscard]] std::vector<float> computeGradient(std::vector<float> const& thicknesses,
                                                         Eigen::Vector3f const& targetColor) const;

        /// Project thicknesses onto feasible set (apply constraints)
        void projectOntoConstraints(std::vector<float>& thicknesses) const;

        /// Compute squared color error
        [[nodiscard]] float computeError(Eigen::Vector3f const& predicted,
                                         Eigen::Vector3f const& target) const;

        /// Compute effective opacities for all layers
        [[nodiscard]] std::vector<float> computeOpacities(std::vector<float> const& thicknesses) const;

        FilamentStack m_stack;
        ThicknessConstraints m_constraints;
        IlluminationMode m_mode{IlluminationMode::Frontlit};
        std::size_t m_backgroundIndex{std::numeric_limits<std::size_t>::max()};
        int m_maxIterations = 500;
        float m_convergenceTolerance = 1e-6f;
        float m_stepSize = 0.1f;
    };

} // namespace gladius::io

