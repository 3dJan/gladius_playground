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
        struct KubelkaMunkRT
        {
            Eigen::Vector3f reflectance = Eigen::Vector3f::Zero();
            Eigen::Vector3f transmittance = Eigen::Vector3f::Ones();
        };

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

        /// Per-channel Transmission Distance (TD) in mm for backlit modeling
        /// If any component <= 0, TD is considered unknown and backlit uses opacity mapping.
        Eigen::Vector3f transmissionDistance = Eigen::Vector3f::Zero();

        /// Minimum allowed thickness (mm), typically one layer height
        float minThickness = 0.0f;

        /// Maximum allowed thickness (mm)
        float maxThickness = 10.0f;

        FilamentOpticalProperties() = default;

        FilamentOpticalProperties(std::string filamentName,
                                  Eigen::Vector3f const& color,
                                  float filamentOpacity,
                                  float refThickness = 0.4f,
                                  Eigen::Vector3f const& td = Eigen::Vector3f::Zero())
            : name(std::move(filamentName))
            , reflectanceColor(color)
            , opacity(filamentOpacity)
            , referenceThickness(refThickness)
            , transmissionDistance(td)
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

        /// Compute per-channel transmittance T_c for a given thickness using TD
        /// Returns vector where each component is in [0,1]
        [[nodiscard]] Eigen::Vector3f computePerChannelTransmittance(float thickness) const
        {
            if (thickness <= 0.0f)
            {
                return Eigen::Vector3f::Ones();
            }
            Eigen::Vector3f T = Eigen::Vector3f::Ones();
            for (int c = 0; c < 3; ++c)
            {
                float const td = transmissionDistance[c];
                if (td > 0.0f)
                {
                    T[c] = std::exp(-thickness / td);
                }
                else
                {
                    // Fallback: derive approximate transmittance from opacity and referenceThickness
                    float const oneMinusOpacity = std::max(1.0f - opacity, 1e-6f);
                    float const alpha = -std::log(oneMinusOpacity) / std::max(referenceThickness, 1e-6f);
                    float const effOpacity = 1.0f - std::exp(-alpha * thickness);
                    T[c] = 1.0f - effOpacity;
                }
            }
            return T.cwiseMax(0.0f).cwiseMin(1.0f);
        }

        /**
         * @brief Compute Kubelka–Munk reflectance/transmittance for a finite thickness
         *
         * Uses per-channel reflectance color as R∞ and derives K/S from it. Absorption K is
         * approximated from transmissionDistance (TD) when available; scattering S is then
         * inferred via K/S. If TD is unavailable, falls back to the opacity-based model.
         */
        [[nodiscard]] KubelkaMunkRT computeKubelkaMunkRT(float thickness) const
        {
            KubelkaMunkRT result;

            if (thickness <= 0.0f)
            {
                result.reflectance = Eigen::Vector3f::Zero();
                result.transmittance = Eigen::Vector3f::Ones();
                return result;
            }

            float const epsilon = 1e-6f;
            float const effectiveOpacity = computeEffectiveOpacity(thickness);

            for (int c = 0; c < 3; ++c)
            {
                float const Rinf = std::clamp(reflectanceColor[c], epsilon, 1.0f - epsilon);
                float const td = transmissionDistance[c];

                if (td <= 0.0f)
                {
                    // Fallback: approximate with opacity-based model
                    result.reflectance[c] = std::clamp(Rinf * effectiveOpacity, 0.0f, 1.0f);
                    result.transmittance[c] = std::clamp(1.0f - effectiveOpacity, 0.0f, 1.0f);
                    continue;
                }

                // Derive K/S from R∞ and K from TD
                float const kOverS = std::max(((1.0f - Rinf) * (1.0f - Rinf)) / std::max(2.0f * Rinf, epsilon), epsilon);
                float const K = 1.0f / std::max(td, epsilon);
                float const S = std::max(K / kOverS, epsilon);

                float const alpha = std::sqrt(std::max(kOverS * (kOverS + 2.0f), 0.0f));
                float const expTerm = std::exp(-alpha * S * thickness);
                float const expSquared = expTerm * expTerm;

                float const denom = std::max(1.0f - (Rinf * Rinf * expSquared), epsilon);
                float const R_t = Rinf * (1.0f - expSquared) / denom;
                float const T_t = (1.0f - Rinf * Rinf) * expTerm / denom;

                result.reflectance[c] = std::clamp(R_t, 0.0f, 1.0f);
                result.transmittance[c] = std::clamp(T_t, 0.0f, 1.0f);
            }

            return result;
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
            if (thickness <= 0.0f)
            {
                return 0.0f;
            }

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

