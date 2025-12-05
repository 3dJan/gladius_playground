/**
 * @file FilamentOpticalProperties.h
 * @brief Optical property definitions for filaments in frontlit (HueForge-style) color reproduction
 *
 * This header defines data structures for modeling how stacked translucent filament layers
 * produce colors when viewed frontlit (light reflecting off the surface). Uses a layered
 * visibility model where top layers partially occlude bottom layers based on opacity.
 *
 * Physical model:
 * - Light enters the surface, scatters through layers, and exits back toward the viewer
 * - Each layer has a reflectance color (what you see when it's thick) and an opacity
 * - The perceived color is: C = Σ(Cᵢ × Vᵢ) where Vᵢ is the visibility of layer i
 * - Visibility depends on opacity of all layers above it
 */

#pragma once

#include <eigen3/Eigen/Core>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace gladius::io
{

    /**
     * @brief Optical properties of a single filament for frontlit viewing
     *
     * Models how a filament appears when viewed with light reflecting off the front surface.
     * The key properties are:
     * - reflectanceColor: The color you see when the layer is thick (opaque)
     * - opacity: How much the layer blocks light from layers beneath it
     */
    struct FilamentOpticalProperties
    {
        std::string name; ///< Human-readable name (e.g., "Prusament Galaxy Black")

        /// RGB reflectance color in linear space [0,1]
        /// This is the color of an infinitely thick layer (what you see when it's opaque)
        Eigen::Vector3f reflectanceColor = Eigen::Vector3f::Ones();

        /// Opacity at reference thickness [0,1]
        /// 0 = completely transparent, 1 = completely opaque
        float opacity = 0.5f;

        /// Reference thickness (mm) at which opacity is measured
        /// Typically 0.4mm (one layer) or 1.0mm (a few layers)
        float referenceThickness = 0.4f;

        /// Minimum allowed thickness (mm), typically one layer height
        float minThickness = 0.0f;

        /// Maximum allowed thickness (mm)
        float maxThickness = 10.0f;

        FilamentOpticalProperties() = default;

        FilamentOpticalProperties(std::string filamentName,
                                  Eigen::Vector3f const& color,
                                  float filamentOpacity,
                                  float refThickness = 0.4f)
            : name(std::move(filamentName))
            , reflectanceColor(color)
            , opacity(filamentOpacity)
            , referenceThickness(refThickness)
        {
        }

        /**
         * @brief Compute effective opacity for a given thickness
         *
         * Uses exponential falloff: effectiveOpacity = 1 - exp(-opacity * t / refThickness)
         * This gives opacity=0 at t=0 and approaches 1 as t → ∞
         *
         * @param thickness Layer thickness in mm
         * @return Effective opacity in [0, 1]
         */
        [[nodiscard]] float computeEffectiveOpacity(float thickness) const
        {
            if (thickness <= 0.0f || opacity <= 0.0f)
            {
                return 0.0f;
            }
            if (opacity >= 1.0f)
            {
                return 1.0f;
            }

            // Convert opacity at reference thickness to absorption coefficient
            // opacity = 1 - exp(-alpha * refThickness), so alpha = -ln(1 - opacity) / refThickness
            float const oneMinusOpacity = std::max(1.0f - opacity, 1e-6f);
            float const alpha = -std::log(oneMinusOpacity) / referenceThickness;

            return 1.0f - std::exp(-alpha * thickness);
        }
    };

    /**
     * @brief Configuration for thickness constraints during optimization
     */
    struct ThicknessConstraints
    {
        float minThickness = 0.0f;   ///< Minimum layer thickness (mm)
        float maxThickness = 10.0f;  ///< Maximum layer thickness (mm)
        float layerHeight = 0.0f;    ///< If > 0, quantize to multiples of this (mm)
        float totalMaxThickness = 0.0f; ///< If > 0, limit sum of all thicknesses (mm)

        /**
         * @brief Clamp and optionally quantize a thickness value
         *
         * @param thickness Raw thickness value
         * @return Constrained thickness value
         */
        [[nodiscard]] float constrain(float thickness) const
        {
            thickness = std::clamp(thickness, minThickness, maxThickness);

            if (layerHeight > 0.0f)
            {
                thickness = std::round(thickness / layerHeight) * layerHeight;
                thickness = std::clamp(thickness, minThickness, maxThickness);
            }

            return thickness;
        }
    };

    /**
     * @brief A stack of filaments in order from bottom to top
     *
     * The order matters for frontlit viewing: the bottom layers are occluded
     * by the layers above them.
     */
    struct FilamentStack
    {
        std::vector<FilamentOpticalProperties> filaments; ///< Ordered from bottom to top

        FilamentStack() = default;

        explicit FilamentStack(std::vector<FilamentOpticalProperties> stack)
            : filaments(std::move(stack))
        {
        }

        [[nodiscard]] std::size_t size() const
        {
            return filaments.size();
        }

        [[nodiscard]] bool empty() const
        {
            return filaments.empty();
        }

        FilamentOpticalProperties& operator[](std::size_t index)
        {
            return filaments[index];
        }

        FilamentOpticalProperties const& operator[](std::size_t index) const
        {
            return filaments[index];
        }

        void push_back(FilamentOpticalProperties const& filament)
        {
            filaments.push_back(filament);
        }
    };

    /**
     * @brief Result of thickness solving for a single target color
     */
    struct ThicknessSolution
    {
        Eigen::Vector3f targetColor;   ///< The requested color (linear RGB)
        Eigen::Vector3f achievedColor; ///< The actually achievable color (linear RGB)
        std::vector<float> thicknesses; ///< Thickness per filament (in stack order)
        float colorError = 0.0f;       ///< Color difference (Delta-E or similar)
        bool converged = false;        ///< True if solver converged to acceptable error

        ThicknessSolution() = default;

        explicit ThicknessSolution(std::size_t numFilaments)
            : thicknesses(numFilaments, 0.0f)
        {
        }

        [[nodiscard]] float totalThickness() const
        {
            float sum = 0.0f;
            for (float t : thicknesses)
            {
                sum += t;
            }
            return sum;
        }
    };

} // namespace gladius::io

