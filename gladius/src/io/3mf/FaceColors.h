/**
 * @file FaceColors.h
 * @brief Data structures for per-face color information in 3MF export
 *
 * This header defines structures for storing and passing per-face color data
 * to the 3MF mesh exporter, enabling colored mesh exports with per-triangle
 * material/color assignments.
 */

#pragma once

#include <eigen3/Eigen/Core>

#include <cstdint>
#include <vector>

namespace gladius::io
{

    /**
     * @brief sRGB color with 8-bit components
     *
     * Used for per-face color assignment in 3MF export. Colors are in sRGB color
     * space with 8-bit per channel precision, matching the 3MF color specification.
     */
    struct Color8
    {
        std::uint8_t r = 255; ///< Red component [0-255]
        std::uint8_t g = 255; ///< Green component [0-255]
        std::uint8_t b = 255; ///< Blue component [0-255]
        std::uint8_t a = 255; ///< Alpha component [0-255]

        constexpr Color8() = default;

        constexpr Color8(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha = 255)
            : r(red)
            , g(green)
            , b(blue)
            , a(alpha)
        {
        }

        /// Create from floating point sRGB values [0, 1]
        static Color8 fromFloat(float red, float green, float blue, float alpha = 1.0f)
        {
            auto clamp = [](float v) -> std::uint8_t
            {
                if (v <= 0.0f)
                {
                    return 0;
                }
                if (v >= 1.0f)
                {
                    return 255;
                }
                return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
            };
            return {clamp(red), clamp(green), clamp(blue), clamp(alpha)};
        }

        /// Create from Eigen::Vector3f (sRGB, range [0, 1])
        static Color8 fromVector3f(Eigen::Vector3f const& rgb, float alpha = 1.0f)
        {
            return fromFloat(rgb.x(), rgb.y(), rgb.z(), alpha);
        }

        /// Create from Eigen::Vector4f (sRGBA, range [0, 1])
        static Color8 fromVector4f(Eigen::Vector4f const& rgba)
        {
            return fromFloat(rgba.x(), rgba.y(), rgba.z(), rgba.w());
        }

        /// Convert to Eigen::Vector3f (sRGB, range [0, 1])
        [[nodiscard]] Eigen::Vector3f toVector3f() const
        {
            return {r / 255.0f, g / 255.0f, b / 255.0f};
        }

        /// Convert to Eigen::Vector4f (sRGBA, range [0, 1])
        [[nodiscard]] Eigen::Vector4f toVector4f() const
        {
            return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        /// Equality comparison
        bool operator==(Color8 const& other) const
        {
            return r == other.r && g == other.g && b == other.b && a == other.a;
        }

        bool operator!=(Color8 const& other) const
        {
            return !(*this == other);
        }
    };

    /**
     * @brief Per-face color data for mesh export
     *
     * Stores a color for each face (triangle) of a mesh. The number of colors
     * must match the number of faces in the mesh being exported.
     */
    struct FaceColors
    {
        std::vector<Color8> colors; ///< One color per face

        FaceColors() = default;

        explicit FaceColors(std::size_t numFaces)
            : colors(numFaces)
        {
        }

        explicit FaceColors(std::vector<Color8> faceColors)
            : colors(std::move(faceColors))
        {
        }

        /// Create from floating point colors (Eigen vectors)
        static FaceColors fromVector3f(std::vector<Eigen::Vector3f> const& rgbColors)
        {
            FaceColors result;
            result.colors.reserve(rgbColors.size());
            for (auto const& c : rgbColors)
            {
                result.colors.push_back(Color8::fromVector3f(c));
            }
            return result;
        }

        [[nodiscard]] std::size_t size() const
        {
            return colors.size();
        }

        [[nodiscard]] bool empty() const
        {
            return colors.empty();
        }

        Color8& operator[](std::size_t index)
        {
            return colors[index];
        }

        Color8 const& operator[](std::size_t index) const
        {
            return colors[index];
        }
    };

} // namespace gladius::io

