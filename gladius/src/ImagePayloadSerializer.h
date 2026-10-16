#pragma once

/// @file ImagePayloadSerializer.h
/// @brief Serializes image stacks into the backend-neutral WebGPU payload.
///
/// Layout:
///   Header (4 words): width, height, depth, encoding
///   RGBA32F encoding: bit-preserved RGBA float values per voxel.
///   PackedGrayscale8 encoding: four normalized grayscale bytes per word.
///
/// Rows are emitted bottom-to-top to match ImageStackResource and the OpenCL
/// primitives payload.

#include "io/3mf/ImageStack.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gladius::io
{
    inline constexpr std::size_t IMAGE_PAYLOAD_HEADER_WORDS = 4u;
    inline constexpr std::uint32_t IMAGE_PAYLOAD_ENCODING_RGBA32F = 0u;
    inline constexpr std::uint32_t IMAGE_PAYLOAD_ENCODING_PACKED_GRAYSCALE8 = 1u;

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

    [[nodiscard]] inline std::vector<std::uint32_t>
    serializeImageStackPayload(ImageStack const & stack)
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
        auto constexpr maxWord = std::numeric_limits<std::uint32_t>::max();
        auto constexpr maxSize = std::numeric_limits<std::size_t>::max();
        if (width == 0u || height == 0u || width > maxWord || height > maxWord || depth > maxWord)
        {
            throw std::runtime_error("Image stack dimensions must be non-zero and fit in 32 bits");
        }

        for (auto const & image : stack)
        {
            if (image.getWidth() != width || image.getHeight() != height ||
                image.getFormat() != format)
            {
                throw std::runtime_error("All image stack layers must have matching dimensions and format");
            }

            auto const & data = image.getData();
            if (width > maxSize / height)
            {
                throw std::length_error("Image stack dimensions exceed addressable storage");
            }
            auto const sliceSize = width * height;
            if (sliceSize > maxSize / channelCount || data.size() != sliceSize * channelCount)
            {
                throw std::runtime_error("Image stack layer data does not match its dimensions and format");
            }
        }

        if (width > maxSize / height)
        {
            throw std::length_error("Image stack dimensions exceed addressable storage");
        }
        auto const sliceSize = width * height;
        if (depth > maxSize / sliceSize)
        {
            throw std::length_error("Image stack dimensions exceed addressable storage");
        }
        auto const voxelCount = sliceSize * depth;

        auto const isOpaqueGrayscale = [](Image const & image)
        {
            if (image.getFormat() == PixelFormat::GRAYSCALE_8BIT)
            {
                return true;
            }
            if (image.getFormat() != PixelFormat::RGBA_8BIT)
            {
                return false;
            }

            auto const & data = image.getData();
            for (std::size_t index = 0u; index < data.size(); index += 4u)
            {
                if (data[index] != data[index + 1u] || data[index] != data[index + 2u] ||
                    data[index + 3u] != 255u)
                {
                    return false;
                }
            }
            return true;
        };
        auto const packGrayscale = (format == PixelFormat::GRAYSCALE_8BIT ||
                                    format == PixelFormat::RGBA_8BIT) &&
                                   std::all_of(stack.begin(), stack.end(), isOpaqueGrayscale);

        if (!packGrayscale && voxelCount > maxSize / 4u)
        {
            throw std::length_error("Image stack payload exceeds addressable storage");
        }
        auto const payloadWords = packGrayscale ? voxelCount / 4u + (voxelCount % 4u != 0u)
                                                : voxelCount * 4u;
        if (payloadWords > maxSize - IMAGE_PAYLOAD_HEADER_WORDS)
        {
            throw std::length_error("Image stack payload exceeds addressable storage");
        }

        std::vector<std::uint32_t> payload;
        payload.reserve(IMAGE_PAYLOAD_HEADER_WORDS + payloadWords);
        payload.push_back(static_cast<std::uint32_t>(width));
        payload.push_back(static_cast<std::uint32_t>(height));
        payload.push_back(static_cast<std::uint32_t>(depth));
        payload.push_back(packGrayscale ? IMAGE_PAYLOAD_ENCODING_PACKED_GRAYSCALE8
                                        : IMAGE_PAYLOAD_ENCODING_RGBA32F);

        if (packGrayscale)
        {
            std::uint32_t packedWord = 0u;
            std::uint32_t packedBytes = 0u;
            for (auto const & image : stack)
            {
                auto const & data = image.getData();
                for (std::size_t y = height; y > 0u; --y)
                {
                    for (std::size_t x = 0u; x < width; ++x)
                    {
                        auto const index = ((y - 1u) * width + x) * channelCount;
                        packedWord |= static_cast<std::uint32_t>(data[index]) << (packedBytes * 8u);
                        ++packedBytes;
                        if (packedBytes == 4u)
                        {
                            payload.push_back(packedWord);
                            packedWord = 0u;
                            packedBytes = 0u;
                        }
                    }
                }
            }
            if (packedBytes != 0u)
            {
                payload.push_back(packedWord);
            }
            return payload;
        }

        auto const appendFloat = [&payload](float const value)
        {
            payload.push_back(std::bit_cast<std::uint32_t>(value));
        };
        for (auto const & image : stack)
        {
            auto const & data = image.getData();
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
                        appendFloat(red);
                        appendFloat(normalize(data[index + 1u]));
                        appendFloat(normalize(data[index + 2u]));
                        appendFloat(normalize(data[index + 3u]));
                        break;
                    case PixelFormat::RGB_16BIT:
                    case PixelFormat::RGB_8BIT:
                        appendFloat(red);
                        appendFloat(normalize(data[index + 1u]));
                        appendFloat(normalize(data[index + 2u]));
                        appendFloat(1.0f);
                        break;
                    case PixelFormat::GRAYSCALE_1BIT:
                    {
                        auto const value = data[index] > 0u ? 1.0f : 0.0f;
                        appendFloat(red);
                        appendFloat(value);
                        appendFloat(value);
                        appendFloat(value);
                        break;
                    }
                    case PixelFormat::GRAYSCALE_16BIT:
                    case PixelFormat::GRAYSCALE_8BIT:
                        appendFloat(red);
                        appendFloat(red);
                        appendFloat(red);
                        appendFloat(1.0f);
                        break;
                    case PixelFormat::GRAYSCALE_ALPHA_16BIT:
                    case PixelFormat::GRAYSCALE_ALPHA_8BIT:
                        appendFloat(red);
                        appendFloat(red);
                        appendFloat(red);
                        appendFloat(normalize(data[index + 1u]));
                        break;
                    }
                }
            }
        }

        return payload;
    }
}
