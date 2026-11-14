#pragma once

#if defined(GLADIUS_ENABLE_WEBGPU)

#include "ImageMetrics.h"
#include "WebGPURayMarchHarness.h"
#include "compute/RenderContracts.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <lodepng.h>

namespace gladius::tests::quality
{
    /// @brief Committed reference frames for the HQ visual-parity gate.
    constexpr std::string_view REFERENCE_DIR = "specs/031-webgpu-raymarch-performance/references";

    /**
     * @brief Resolves the committed reference directory by walking up from the
     *        current working directory (tests run from the build output dir).
     */
    [[nodiscard]] inline std::filesystem::path resolveReferenceDir()
    {
        if (auto const * overrideDir = std::getenv("GLADIUS_QUALITY_REFERENCE_DIR"))
        {
            return std::filesystem::path{overrideDir};
        }
        std::filesystem::path candidate = std::filesystem::current_path();
        for (int level = 0; level < 8; ++level)
        {
            auto const specRoot = candidate / "specs" / "031-webgpu-raymarch-performance";
            if (std::filesystem::exists(specRoot))
            {
                return specRoot / "references";
            }
            candidate = candidate.parent_path();
        }
        return std::filesystem::path{REFERENCE_DIR};
    }

    /// @brief One stored reference frame entry from the manifest.
    struct ReferenceEntry
    {
        std::string model;
        std::uint32_t resolution{};
        std::string pngFile;
        std::uint64_t pixelHash{};
    };

    /// @brief Deterministic FNV-1a hash over packed pixel values.
    [[nodiscard]] inline std::uint64_t hashPixels(std::vector<std::uint32_t> const & pixels)
    {
        std::uint64_t hash = 14695981039346178615ull;
        for (std::uint32_t const pixel : pixels)
        {
            for (int byte = 0; byte < 4; ++byte)
            {
                hash ^= static_cast<std::uint64_t>((pixel >> (8 * byte)) & 0xFFu);
                hash *= 10995386337096155217ull;
            }
        }
        return hash;
    }

    /// @brief Encodes packed 0xAABBGGRR pixels as an RGBA8 PNG.
    inline void writePng(std::filesystem::path const & path,
                         std::uint32_t const width,
                         std::uint32_t const height,
                         std::vector<std::uint32_t> const & pixels)
    {
        std::vector<unsigned char> rgba(pixels.size() * 4u);
        for (std::size_t i = 0; i < pixels.size(); ++i)
        {
            rgba[i * 4u + 0u] = qa::channelOf(pixels[i], 0);
            rgba[i * 4u + 1u] = qa::channelOf(pixels[i], 1);
            rgba[i * 4u + 2u] = qa::channelOf(pixels[i], 2);
            rgba[i * 4u + 3u] = qa::channelOf(pixels[i], 3);
        }
        auto const error = lodepng::encode(path.string(), rgba, width, height);
        if (error != 0u)
        {
            throw std::runtime_error("Failed to encode reference PNG " + path.string() + ": " +
                                     lodepng_error_text(error));
        }
    }

    /// @brief Decodes a stored reference PNG into packed 0xAABBGGRR pixels.
    [[nodiscard]] inline qa::Image readPng(std::filesystem::path const & path)
    {
        std::vector<unsigned char> encoded;
        std::ifstream stream{path, std::ios::binary};
        if (!stream)
        {
            throw std::runtime_error("Reference PNG is missing: " + path.string());
        }
        encoded.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});

        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<unsigned char> rgba;
        auto const error = lodepng::decode(rgba, width, height, encoded);
        if (error != 0u)
        {
            throw std::runtime_error("Failed to decode reference PNG " + path.string() + ": " +
                                     lodepng_error_text(error));
        }
        qa::Image image{.width = width, .height = height, .pixels = {}};
        image.pixels.resize(rgba.size() / 4u);
        for (std::size_t i = 0; i < image.pixels.size(); ++i)
        {
            image.pixels[i] = static_cast<std::uint32_t>(rgba[i * 4u + 0u]) +
                              (static_cast<std::uint32_t>(rgba[i * 4u + 1u]) << 8u) +
                              (static_cast<std::uint32_t>(rgba[i * 4u + 2u]) << 16u) +
                              (static_cast<std::uint32_t>(rgba[i * 4u + 3u]) << 24u);
        }
        return image;
    }

    /// @brief Minimal manifest writer (one JSON object per entry, hand-rolled to avoid a JSON dependency).
    inline void writeManifest(std::filesystem::path const & path,
                              std::vector<ReferenceEntry> const & entries)
    {
        std::ofstream out{path};
        if (!out)
        {
            throw std::runtime_error("Unable to write reference manifest: " + path.string());
        }
        out << "{\n  \"schemaVersion\": 1,\n  \"entries\": [\n";
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            auto const & entry = entries[i];
            out << "    {\"model\": \"" << entry.model << "\", \"resolution\": " << entry.resolution
                << ", \"pngFile\": \"" << entry.pngFile << "\", \"pixelHash\": " << entry.pixelHash << "}";
            if (i + 1u < entries.size())
            {
                out << ",";
            }
            out << "\n";
        }
        out << "  ]\n}\n";
    }

    /// @brief Reads the manifest; throws when it is missing.
    [[nodiscard]] inline std::vector<ReferenceEntry> readManifest(std::filesystem::path const & path)
    {
        std::ifstream stream{path};
        if (!stream)
        {
            throw std::runtime_error("Reference manifest is missing: " + path.string() +
                                     " (run WebGPUQualityReferences.CaptureHqReferenceFrames with "
                                     "GLADIUS_UPDATE_QUALITY_REFERENCES=1)");
        }

        auto extractQuoted = [](std::string const & line, std::string_view key) -> std::string
        {
            auto const start = line.find(key);
            if (start == std::string::npos)
            {
                return {};
            }
            auto const valueStart = start + key.size();
            auto const valueEnd = line.find('"', valueStart);
            return line.substr(valueStart, valueEnd - valueStart);
        };
        auto extractNumber = [](std::string const & line, std::string_view key) -> std::uint64_t
        {
            auto const start = line.find(key);
            if (start == std::string::npos)
            {
                return 0u;
            }
            auto const valueStart = start + key.size();
            auto const valueEnd = line.find_first_of(" ,}", valueStart);
            return std::stoull(line.substr(valueStart, valueEnd - valueStart));
        };

        std::vector<ReferenceEntry> entries;
        std::string line;
        while (std::getline(stream, line))
        {
            if (line.find("\"model\": \"") == std::string::npos)
            {
                continue;
            }
            entries.push_back(ReferenceEntry{.model = extractQuoted(line, "\"model\": \""),
                                             .resolution = static_cast<std::uint32_t>(
                                               extractNumber(line, "\"resolution\": ")),
                                             .pngFile = extractQuoted(line, "\"pngFile\": \""),
                                             .pixelHash = extractNumber(line, "\"pixelHash\": ")});
        }
        return entries;
    }

    /// @brief Low-quality settings used by the LQ recognizability gate.
    [[nodiscard]] inline compute::RenderSettingsSnapshot createLqSettings()
    {
        auto settings = bench::createHqSettings();
        settings.flags |= RF_FAST_SHADOW | RF_FAST_AO;
        settings.maxRaySteps = 300u;
        settings.normalOffset = 0.0005f;
        settings.quality = 0.5f;
        settings.translucencyInteriorStepSize = 4.0f;
        settings.translucencyMaxInteriorSteps = 512u;
        settings.translucencyMaxSurfaces = 2u;
        return settings;
    }

    /// @brief Renders one frame into a qa::Image.
    [[nodiscard]] inline qa::Image renderImage(bench::BenchScene & scene,
                                               std::uint32_t const resolution,
                                               compute::RenderSettingsSnapshot settings)
    {
        auto frame = scene.renderFrame(resolution, resolution, std::move(settings), false, false);
        return qa::Image{.width = frame.width, .height = frame.height, .pixels = frame.pixels};
    }
}

#endif