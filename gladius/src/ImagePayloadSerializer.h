#pragma once

/// @file ImagePayloadSerializer.h
/// @brief Serializes image stacks into the backend-neutral WebGPU payload.
///
/// Layout:
///   Header (4 floats): width, height, depth, reserved
///   Voxels: RGBA float values in X-major, then Y, then Z order.
///
/// Rows are emitted bottom-to-top to match ImageStackResource and the OpenCL
/// primitives payload.

#include "io/3mf/ImageStack.h"

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace gladius::io
{
    inline constexpr std::size_t IMAGE_PAYLOAD_HEADER_FLOATS = 4u;

    [[nodiscard]] inline std::size_t imageChannelCount(PixelFormat const format)
    {
        switch (format)
        {
        case PixelFormat::RGBA_16BIT:
        case PixelFormat::RGBA_8BIT:
            return 4u;
        case PixelFormat::RGB_16BIT:
        case PixelFormat::RGB_8BIT:
            return 3u;
        case PixelFormat::GRAYSCALE_1BIT:
        case PixelFormat::GRAYSCALE_16BIT:
        case PixelFormat::GRAYSCALE_8BIT:
            return 1u;
        case PixelFormat::GRAYSCALE_ALPHA_16BIT:
        case PixelFormat::GRAYSCALE_ALPHA_8BIT:
            return 2u;
        }
        throw std::runtime_error("Unknown image stack pixel format");
    }

    [[nodiscard]] inline std::vector<float> serializeImageStackPayload(ImageStack const & stack)
    {
        if (stack.empty())
        {
            return {};
        }

        auto const & firstImage = stack.front();
        auto const width = static_cast<std::size_t>(firstImage.getWidth());
        auto const height = static_cast<std::size_t>(firstImage.getHeight());
        auto const depth = stack.size();
        auto const format = firstImage.getFormat();
        auto const channelCount = imageChannelCount(format);
        if (width == 0u || height == 0u)
        {
            throw std::runtime_error("Image stack dimensions must be non-zero");
        }

        std::vector<float> payload;
        payload.reserve(IMAGE_PAYLOAD_HEADER_FLOATS + width * height * depth * 4u);
        payload.push_back(static_cast<float>(width));
        payload.push_back(static_cast<float>(height));
        payload.push_back(static_cast<float>(depth));
        payload.push_back(0.0f);

        for (auto const & image : stack)
        {
            if (image.getWidth() != width || image.getHeight() != height ||
                image.getFormat() != format)
            {
                throw std::runtime_error("All image stack layers must have matching dimensions and format");
            }

            auto const & data = image.getData();
            if (data.size() != width * height * channelCount)
            {
                throw std::runtime_error("Image stack layer data does not match its dimensions and format");
            }

            for (std::size_t y = height; y > 0u; --y)
            {
                for (std::size_t x = 0u; x < width; ++x)
                {
                    auto const index = ((y - 1u) * width + x) * channelCount;
                    auto const normalize = [](unsigned char const value)
                    {
                        return static_cast<float>(value) / 255.0f;
                    };
                    auto const red = normalize(data[index]);

                    switch (format)
                    {
                    case PixelFormat::RGBA_16BIT:
                    case PixelFormat::RGBA_8BIT:
                        payload.push_back(red);
                        payload.push_back(normalize(data[index + 1u]));
                        payload.push_back(normalize(data[index + 2u]));
                        payload.push_back(normalize(data[index + 3u]));
                        break;
                    case PixelFormat::RGB_16BIT:
                    case PixelFormat::RGB_8BIT:
                        payload.push_back(red);
                        payload.push_back(normalize(data[index + 1u]));
                        payload.push_back(normalize(data[index + 2u]));
                        payload.push_back(1.0f);
                        break;
                    case PixelFormat::GRAYSCALE_1BIT:
                    {
                        auto const value = data[index] > 0u ? 1.0f : 0.0f;
                        payload.push_back(red);
                        payload.push_back(value);
                        payload.push_back(value);
                        payload.push_back(value);
                        break;
                    }
                    case PixelFormat::GRAYSCALE_16BIT:
                    case PixelFormat::GRAYSCALE_8BIT:
                        payload.push_back(red);
                        payload.push_back(red);
                        payload.push_back(red);
                        payload.push_back(1.0f);
                        break;
                    case PixelFormat::GRAYSCALE_ALPHA_16BIT:
                    case PixelFormat::GRAYSCALE_ALPHA_8BIT:
                        payload.push_back(red);
                        payload.push_back(red);
                        payload.push_back(red);
                        payload.push_back(normalize(data[index + 1u]));
                        break;
                    }
                }
            }
        }

        return payload;
    }
}
