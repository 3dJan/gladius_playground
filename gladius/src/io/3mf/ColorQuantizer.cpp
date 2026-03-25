/**
 * @file ColorQuantizer.cpp
 * @brief Implementation of deterministic adaptive color quantization
 */

#include "ColorQuantizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>

namespace gladius::io
{

    namespace
    {
        /// Packed RGB key for deterministic color comparison (alpha ignored)
        std::uint32_t packRgb(Color8 const& c)
        {
            return (static_cast<std::uint32_t>(c.r) << 16) |
                   (static_cast<std::uint32_t>(c.g) << 8) |
                   static_cast<std::uint32_t>(c.b);
        }

        /// L2 distance between two colors (RGB only, [0-255] range)
        float colorDistanceSq(Color8 const& a, Color8 const& b)
        {
            float const dr = static_cast<float>(a.r) - static_cast<float>(b.r);
            float const dg = static_cast<float>(a.g) - static_cast<float>(b.g);
            float const db = static_cast<float>(a.b) - static_cast<float>(b.b);
            return dr * dr + dg * dg + db * db;
        }

        /// Compute the average color of a set of indexed face colors
        Color8 averageColor(FaceColors const& faceColors, std::vector<std::size_t> const& indices)
        {
            if (indices.empty())
            {
                return {};
            }

            std::uint64_t rSum = 0;
            std::uint64_t gSum = 0;
            std::uint64_t bSum = 0;
            for (auto idx : indices)
            {
                auto const& c = faceColors[idx];
                rSum += c.r;
                gSum += c.g;
                bSum += c.b;
            }

            auto const n = static_cast<std::uint64_t>(indices.size());
            return Color8(static_cast<std::uint8_t>(rSum / n),
                          static_cast<std::uint8_t>(gSum / n),
                          static_cast<std::uint8_t>(bSum / n));
        }

        /// Deterministic median-cut: split the bucket with the widest channel range
        struct Bucket
        {
            std::vector<std::size_t> indices;

            /// Channel range (max - min) for the given channel across bucket entries
            std::uint8_t channelRange(FaceColors const& fc, int channel) const
            {
                std::uint8_t lo = 255;
                std::uint8_t hi = 0;
                for (auto idx : indices)
                {
                    auto const& c = fc[idx];
                    std::uint8_t val = 0;
                    switch (channel)
                    {
                    case 0: val = c.r; break;
                    case 1: val = c.g; break;
                    case 2: val = c.b; break;
                    }
                    lo = std::min(lo, val);
                    hi = std::max(hi, val);
                }
                return static_cast<std::uint8_t>(hi - lo);
            }

            /// Find the channel with the widest range
            int widestChannel(FaceColors const& fc) const
            {
                int best = 0;
                std::uint8_t bestRange = channelRange(fc, 0);
                for (int ch = 1; ch < 3; ++ch)
                {
                    auto const range = channelRange(fc, ch);
                    if (range > bestRange)
                    {
                        bestRange = range;
                        best = ch;
                    }
                }
                return best;
            }

            /// Sort indices by a given channel
            void sortByChannel(FaceColors const& fc, int channel)
            {
                std::sort(indices.begin(), indices.end(),
                    [&fc, channel](std::size_t a, std::size_t b)
                    {
                        auto const& ca = fc[a];
                        auto const& cb = fc[b];
                        switch (channel)
                        {
                        case 0: return ca.r < cb.r;
                        case 1: return ca.g < cb.g;
                        case 2: return ca.b < cb.b;
                        }
                        return false;
                    });
            }
        };

    } // anonymous namespace

    QuantizedPalette ColorQuantizer::quantize(FaceColors const& faceColors, std::uint32_t maxColors)
    {
        QuantizedPalette result;

        if (faceColors.empty() || maxColors == 0)
        {
            return result;
        }

        // Build initial bucket with all face indices
        Bucket initial;
        initial.indices.resize(faceColors.size());
        std::iota(initial.indices.begin(), initial.indices.end(), std::size_t{0});

        std::vector<Bucket> buckets;
        buckets.push_back(std::move(initial));

        // Median-cut: repeatedly split the bucket with the widest range
        while (buckets.size() < maxColors)
        {
            // Find the bucket with the widest channel range
            std::size_t bestBucket = 0;
            std::uint8_t bestRange = 0;
            int bestChannel = 0;

            for (std::size_t i = 0; i < buckets.size(); ++i)
            {
                if (buckets[i].indices.size() <= 1)
                {
                    continue;
                }
                int const ch = buckets[i].widestChannel(faceColors);
                auto const range = buckets[i].channelRange(faceColors, ch);
                if (range > bestRange)
                {
                    bestRange = range;
                    bestBucket = i;
                    bestChannel = ch;
                }
            }

            // If no bucket can be split further, stop
            if (bestRange == 0)
            {
                break;
            }

            // Split the best bucket at the median
            auto& bucket = buckets[bestBucket];
            bucket.sortByChannel(faceColors, bestChannel);

            auto const mid = bucket.indices.size() / 2;
            Bucket lower;
            Bucket upper;
            lower.indices.assign(bucket.indices.begin(), bucket.indices.begin() + static_cast<std::ptrdiff_t>(mid));
            upper.indices.assign(bucket.indices.begin() + static_cast<std::ptrdiff_t>(mid), bucket.indices.end());

            buckets[bestBucket] = std::move(lower);
            buckets.push_back(std::move(upper));
        }

        // Build palette and mapping
        result.colors.reserve(buckets.size());
        result.sourceToPaletteMap.resize(faceColors.size(), 0);
        result.maxApproximationError = 0.0f;

        for (std::size_t bi = 0; bi < buckets.size(); ++bi)
        {
            auto const paletteColor = averageColor(faceColors, buckets[bi].indices);
            result.colors.push_back(paletteColor);

            auto const paletteIdx = static_cast<std::uint32_t>(bi);
            for (auto faceIdx : buckets[bi].indices)
            {
                result.sourceToPaletteMap[faceIdx] = paletteIdx;

                float const distSq = colorDistanceSq(faceColors[faceIdx], paletteColor);
                float const dist = std::sqrt(distSq);
                result.maxApproximationError = std::max(result.maxApproximationError, dist);
            }
        }

        return result;
    }

    std::size_t ColorQuantizer::countUniqueOpaqueColors(FaceColors const& faceColors)
    {
        std::map<std::uint32_t, bool> seen;
        for (std::size_t i = 0; i < faceColors.size(); ++i)
        {
            seen[packRgb(faceColors[i])] = true;
        }
        return seen.size();
    }

    bool ColorQuantizer::hasTransparency(FaceColors const& faceColors)
    {
        return std::any_of(faceColors.colors.begin(), faceColors.colors.end(),
            [](Color8 const& c) { return c.a < 255; });
    }

} // namespace gladius::io
