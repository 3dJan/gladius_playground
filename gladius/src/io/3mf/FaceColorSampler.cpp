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
#include <unordered_map>

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
        ProgressCallback progressCallback,
        bool convertToSrgbFlag)
    {
        if (faces.empty())
        {
            return {};
        }

        for (std::size_t i = 0; i < faces.size(); ++i)
        {
            auto const& face = faces[i];
            if (face[0] >= vertices.size() || face[1] >= vertices.size() || face[2] >= vertices.size())
            {
                throw std::runtime_error(
                  "FaceColorSampler::sampleFaceColors: face index out of bounds for vertices array");
            }
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

                        if (batchColors.size() != batchPositions.size())
                        {
                                throw std::runtime_error(
                                    "FaceColorSampler::sampleFaceColors: GPU sampling returned unexpected number of colors");
                        }

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

        // Step 4: Optionally convert linear RGB to sRGB
        if (convertToSrgbFlag)
        {
            convertToSrgb(colors);
        }

        return colors;
    }

    FaceColors FaceColorSampler::sampleFaceColorsAsColor8(
        std::vector<Eigen::Vector3f> const& vertices,
        std::vector<std::array<std::uint32_t, 3>> const& faces,
        DualContouringSamplingProgram& samplingProgram,
        Primitives const& primitives,
        ProgressCallback progressCallback,
        bool convertToSrgb)
    {
        auto const colors = sampleFaceColors(vertices, faces, samplingProgram, primitives, progressCallback, convertToSrgb);
        return FaceColors::fromVector3f(colors);
    }

    VertexColors FaceColorSampler::sampleVertexColors(
        std::vector<Eigen::Vector3f> const& vertices,
        std::vector<std::array<std::uint32_t, 3>> const& faces,
        DualContouringSamplingProgram& samplingProgram,
        Primitives const& primitives,
        ProgressCallback progressCallback,
        bool convertToSrgbFlag)
    {
        if (faces.empty())
        {
            return {};
        }

        for (std::size_t i = 0; i < faces.size(); ++i)
        {
            auto const& face = faces[i];
            if (face[0] >= vertices.size() || face[1] >= vertices.size() || face[2] >= vertices.size())
            {
                throw std::runtime_error(
                  "FaceColorSampler::sampleVertexColors: face index out of bounds for vertices array");
            }
        }

        std::size_t const numFaces = faces.size();

        // Collect all unique vertex indices that we need to sample
        // Use a map to track which vertices we need and deduplicate
        std::vector<std::uint32_t> uniqueVertexIndices;
        std::unordered_map<std::uint32_t, std::size_t> vertexIndexToSampleIndex;

        for (auto const& face : faces)
        {
            for (std::uint32_t idx : face)
            {
                if (vertexIndexToSampleIndex.find(idx) == vertexIndexToSampleIndex.end())
                {
                    vertexIndexToSampleIndex[idx] = uniqueVertexIndices.size();
                    uniqueVertexIndices.push_back(idx);
                }
            }
        }

        // Build positions for unique vertices
        std::vector<Eigen::Vector3f> positions;
        positions.reserve(uniqueVertexIndices.size());
        for (std::uint32_t idx : uniqueVertexIndices)
        {
            positions.push_back(vertices[idx]);
        }

        // Sample colors at all unique vertex positions in batches
        std::size_t const numVertices = positions.size();
        std::vector<Eigen::Vector3f> vertexColors(numVertices);

        std::size_t processed = 0;
        while (processed < numVertices)
        {
            std::size_t const batchCount = std::min(s_batchSize, numVertices - processed);

            std::vector<Eigen::Vector3f> batchPositions(
                positions.begin() + static_cast<std::ptrdiff_t>(processed),
                positions.begin() + static_cast<std::ptrdiff_t>(processed + batchCount));

            std::vector<Eigen::Vector3f> batchColors;
            samplingProgram.sampleColors(batchPositions, batchColors, primitives);

                        if (batchColors.size() != batchPositions.size())
                        {
                                throw std::runtime_error(
                                    "FaceColorSampler::sampleVertexColors: GPU sampling returned unexpected number of colors");
                        }

            for (std::size_t i = 0; i < batchCount; ++i)
            {
                vertexColors[processed + i] = batchColors[i];
            }

            processed += batchCount;

            if (progressCallback)
            {
                progressCallback(static_cast<float>(processed) / static_cast<float>(numVertices));
            }
        }

        // Optionally convert linear RGB to sRGB
        if (convertToSrgbFlag)
        {
            convertToSrgb(vertexColors);
        }

        // Build per-face vertex colors from sampled data
        VertexColors result(numFaces);
        for (std::size_t faceIdx = 0; faceIdx < numFaces; ++faceIdx)
        {
            auto const& face = faces[faceIdx];
            for (std::size_t vertIdx = 0; vertIdx < 3; ++vertIdx)
            {
                std::size_t sampleIdx = vertexIndexToSampleIndex[face[vertIdx]];
                result[faceIdx][vertIdx] = Color8::fromVector3f(vertexColors[sampleIdx]);
            }
        }

        return result;
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
