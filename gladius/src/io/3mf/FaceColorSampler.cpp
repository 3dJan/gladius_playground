/**
 * @file FaceColorSampler.cpp
 * @brief Implementation of GPU-accelerated face color sampling
 */

#include "FaceColorSampler.h"

#include "DualContouringSamplingProgram.h"
#include "Primitives.h"
#include "nodes/Model.h"
#include "nodes/nodesfwd.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gladius::io
{

    // Initialize static batch size
    std::size_t FaceColorSampler::s_batchSize = DefaultBatchSize;

    bool FaceColorSampler::hasVolumetricColor(nodes::Model const& model)
    {
        auto* endNode = const_cast<nodes::Model&>(model).getEndNode();
        if (endNode == nullptr)
        {
            return false;
        }

        // Check if the Color parameter has a connected source
        auto& params = endNode->parameter();
        auto colorIt = params.find(nodes::FieldNames::Color);
        if (colorIt == params.end())
        {
            return false;
        }

        // If the source is set, the color input is connected
        auto const& source = colorIt->second.getConstSource();
        return source.has_value();
    }

    std::vector<Eigen::Vector3f> FaceColorSampler::sampleFaceColors(
        std::vector<Eigen::Vector3f> const& vertices,
        std::vector<std::array<std::uint32_t, 3>> const& faces,
        DualContouringSamplingProgram& samplingProgram,
        Primitives const& primitives,
        ProgressCallback progressCallback)
    {
        if (faces.empty())
        {
            return {};
        }

        // Step 1: Compute face centroids
        auto centroids = computeCentroids(vertices, faces);
        std::size_t const numFaces = centroids.size();

        // Step 2: Allocate output
        std::vector<Eigen::Vector3f> colors(numFaces);

        // Step 3: Process in batches to avoid GPU overload
        std::size_t processed = 0;
        while (processed < numFaces)
        {
            std::size_t const batchCount = std::min(s_batchSize, numFaces - processed);

            // Extract batch of positions
            std::vector<Eigen::Vector3f> batchPositions(centroids.begin() + static_cast<std::ptrdiff_t>(processed),
                                                         centroids.begin() + static_cast<std::ptrdiff_t>(processed + batchCount));

            // Sample colors for this batch
            std::vector<Eigen::Vector3f> batchColors;
            samplingProgram.sampleColors(batchPositions, batchColors, primitives);

            // Copy results to output
            for (std::size_t i = 0; i < batchCount; ++i)
            {
                colors[processed + i] = batchColors[i];
            }

            processed += batchCount;

            if (progressCallback)
            {
                progressCallback(static_cast<float>(processed) / static_cast<float>(numFaces));
            }
        }

        // Step 4: Convert linear RGB to sRGB
        convertToSrgb(colors);

        return colors;
    }

    FaceColors FaceColorSampler::sampleFaceColorsAsColor8(
        std::vector<Eigen::Vector3f> const& vertices,
        std::vector<std::array<std::uint32_t, 3>> const& faces,
        DualContouringSamplingProgram& samplingProgram,
        Primitives const& primitives,
        ProgressCallback progressCallback)
    {
        auto const colors = sampleFaceColors(vertices, faces, samplingProgram, primitives, progressCallback);
        return FaceColors::fromVector3f(colors);
    }

    void FaceColorSampler::setBatchSize(std::size_t batchSize)
    {
        s_batchSize = batchSize > 0 ? batchSize : DefaultBatchSize;
    }

    std::size_t FaceColorSampler::getBatchSize()
    {
        return s_batchSize;
    }

    float FaceColorSampler::linearToSrgb(float linear)
    {
        // Standard sRGB transfer function
        if (linear <= 0.0031308F)
        {
            return 12.92F * linear;
        }
        return 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
    }

    Eigen::Vector3f FaceColorSampler::linearToSrgb(Eigen::Vector3f const& linear)
    {
        return {linearToSrgb(linear.x()), linearToSrgb(linear.y()), linearToSrgb(linear.z())};
    }

    std::vector<Eigen::Vector3f> FaceColorSampler::computeCentroids(
        std::vector<Eigen::Vector3f> const& vertices,
        std::vector<std::array<std::uint32_t, 3>> const& faces)
    {
        std::vector<Eigen::Vector3f> centroids;
        centroids.reserve(faces.size());

        for (auto const& face : faces)
        {
            Eigen::Vector3f const& v0 = vertices[face[0]];
            Eigen::Vector3f const& v1 = vertices[face[1]];
            Eigen::Vector3f const& v2 = vertices[face[2]];

            centroids.push_back((v0 + v1 + v2) / 3.0F);
        }

        return centroids;
    }

    void FaceColorSampler::convertToSrgb(std::vector<Eigen::Vector3f>& colors)
    {
        for (auto& color : colors)
        {
            color = linearToSrgb(color);
        }
    }

} // namespace gladius::io
