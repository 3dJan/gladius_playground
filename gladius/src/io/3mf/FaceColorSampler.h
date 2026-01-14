/**
 * @file FaceColorSampler.h
 * @brief GPU-accelerated face color sampling for volumetric models
 *
 * This class provides batched GPU evaluation of volumetric colors at mesh face
 * centroids, with proper linear RGB to sRGB conversion for 3MF export.
 */

#pragma once

#include "FaceColors.h"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace gladius
{
    class ComputeContext;
    class Primitives;
    class DualContouringSamplingProgram;

    namespace nodes
    {
        class Model;
        class End;
    } // namespace nodes
} // namespace gladius

namespace gladius::io
{

    /**
     * @brief GPU-accelerated face color sampler using batched evaluation
     *
     * Samples volumetric colors at face centroids by calling the model() function
     * on the GPU. Colors are returned in sRGB color space, suitable for 3MF export.
     *
     * The sampling is performed in batches to avoid GPU memory overload on large meshes.
     */
    class FaceColorSampler
    {
      public:
        /// Progress callback: receives progress value in [0, 1]
        using ProgressCallback = std::function<void(float)>;

        /// Default batch size - tuned for typical GPU memory
        static constexpr std::size_t DefaultBatchSize = 100000;

        /**
         * @brief Check if a model has volumetric color output
         *
         * Returns true if the End node's Color parameter has a connected source,
         * meaning the model produces meaningful color output (not just default white).
         *
         * @param model The node graph model to check
         * @return true if the model has volumetric color output
         */
        static bool hasVolumetricColor(nodes::Model const& model);

        /**
         * @brief Sample face colors from a volumetric model
         *
         * Evaluates the volumetric color function at face centroids using batched
         * GPU evaluation.
         *
         * @param vertices Mesh vertices
         * @param faces Triangle indices (3 indices per face)
         * @param samplingProgram Compiled sampling program for GPU evaluation
         * @param primitives Compiled model primitives for evaluation
         * @param progressCallback Optional callback for progress updates
         * @param convertToSrgb If true (default), convert from linear RGB to sRGB
         * @return Per-face colors, optionally converted to sRGB
         * @throws std::runtime_error if GPU evaluation fails
         */
        static std::vector<Eigen::Vector3f> sampleFaceColors(
            std::vector<Eigen::Vector3f> const& vertices,
            std::vector<std::array<std::uint32_t, 3>> const& faces,
            DualContouringSamplingProgram& samplingProgram,
            Primitives const& primitives,
            ProgressCallback progressCallback = nullptr,
            bool convertToSrgb = true);

        /**
         * @brief Sample face colors and return as FaceColors structure
         *
         * Convenience wrapper that returns colors as Color8 values ready for export.
         *
         * @param vertices Mesh vertices
         * @param faces Triangle indices
         * @param samplingProgram Compiled sampling program
         * @param primitives Compiled model primitives
         * @param progressCallback Optional progress callback
         * @param convertToSrgb If true (default), convert from linear RGB to sRGB
         * @return FaceColors structure with Color8 values
         */
        static FaceColors sampleFaceColorsAsColor8(
            std::vector<Eigen::Vector3f> const& vertices,
            std::vector<std::array<std::uint32_t, 3>> const& faces,
            DualContouringSamplingProgram& samplingProgram,
            Primitives const& primitives,
            ProgressCallback progressCallback = nullptr,
            bool convertToSrgb = true);

    /**
     * @brief Sample colors at vertex positions for per-vertex coloring
     *
     * Evaluates the volumetric color function at each vertex position of each
     * face. This enables proper color interpolation in 3MF exports using
     * p1, p2, p3 property indices.
     *
     * @param vertices Mesh vertices
     * @param faces Triangle indices (3 indices per face)
     * @param samplingProgram Compiled sampling program for GPU evaluation
     * @param primitives Compiled model primitives for evaluation
     * @param progressCallback Optional callback for progress updates
     * @param convertToSrgb If true (default), convert from linear RGB to sRGB
     * @return Per-vertex colors for each face
     * @throws std::runtime_error if GPU evaluation fails
     */
    static VertexColors sampleVertexColors(
        std::vector<Eigen::Vector3f> const& vertices,
        std::vector<std::array<std::uint32_t, 3>> const& faces,
        DualContouringSamplingProgram& samplingProgram,
        Primitives const& primitives,
        ProgressCallback progressCallback = nullptr,
        bool convertToSrgb = true);

        /// Configure batch size for GPU evaluation (faces per dispatch)
        static void setBatchSize(std::size_t batchSize);

        /// Get current batch size
        static std::size_t getBatchSize();

        // Color conversion utilities

        /**
         * @brief Convert linear RGB to sRGB
         *
         * Applies the standard sRGB transfer function (gamma correction).
         *
         * @param linear Linear RGB value in [0, 1]
         * @return sRGB value in [0, 1]
         */
        static float linearToSrgb(float linear);

        /**
         * @brief Convert linear RGB vector to sRGB
         *
         * @param linear Linear RGB vector with components in [0, 1]
         * @return sRGB vector with components in [0, 1]
         */
        static Eigen::Vector3f linearToSrgb(Eigen::Vector3f const& linear);

      private:
        /// Compute face centroids from vertices and faces
        static std::vector<Eigen::Vector3f> computeCentroids(
            std::vector<Eigen::Vector3f> const& vertices,
            std::vector<std::array<std::uint32_t, 3>> const& faces);

        /// Convert entire color array from linear RGB to sRGB
        static void convertToSrgb(std::vector<Eigen::Vector3f>& colors);

        /// Current batch size (global setting)
        static std::size_t s_batchSize;
    };

} // namespace gladius::io
