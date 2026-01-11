#pragma once

#include "DualContouringOctree.h"
#include "gpgpu.h"

#include <Eigen/Core>

#include <cstddef>
#include <memory>
#include <vector>

namespace gladius
{
    class ComputeCore;
    class ComputeContext;
    namespace events
    {
        class Logger;
        using SharedLogger = std::shared_ptr<Logger>;
    }
}

namespace gladius::dual_contouring
{
    /// Configuration for GPU-accelerated sampling
    struct GpuSamplingConfig
    {
        std::size_t cornerBatchSize{8192U};      ///< Max corners per batch
        std::size_t hermiteBatchSize{16384U};    ///< Max Hermite samples per batch
        float gradientEpsilon{0.0001F};          ///< Finite difference step for gradients (smaller = smoother)
        float isoValue{0.0F};                    ///< ISO surface value
        bool enableCaching{true};                ///< Enable sample result caching
        bool fallbackToCpu{true};                ///< Fall back to CPU on GPU failure
    };

    /// Statistics for GPU sampling operations
    struct GpuSamplingStats
    {
        std::size_t cornerBatches{0U};
        std::size_t hermiteBatches{0U};
        std::size_t totalCornerSamples{0U};
        std::size_t totalHermiteSamples{0U};
        std::size_t cacheHits{0U};
        std::size_t cacheMisses{0U};
        double totalKernelTimeMs{0.0};

        void reset()
        {
            *this = GpuSamplingStats{};
        }
    };

    /// RAII wrapper for managing GPU sampling session with batching and caching
    class GpuSamplingSession
    {
      public:
        GpuSamplingSession(ComputeCore & core, GpuSamplingConfig config);
        ~GpuSamplingSession();

        // Disable copy, enable move
        GpuSamplingSession(GpuSamplingSession const &) = delete;
        GpuSamplingSession & operator=(GpuSamplingSession const &) = delete;
        GpuSamplingSession(GpuSamplingSession &&) noexcept = default;
        GpuSamplingSession & operator=(GpuSamplingSession &&) noexcept = default;

        /// Sample SDF values at corner positions
        /// Returns false if GPU sampling fails and CPU fallback should be used
        [[nodiscard]] bool sampleCorners(std::vector<Eigen::Vector3f> const & positions,
                                         std::vector<float> & outValues);

        /// Sample SDF values at corner positions with variable thickness
        /// Returns false if GPU sampling fails and CPU fallback should be used
        [[nodiscard]] bool sampleCornersVariableThickness(std::vector<Eigen::Vector3f> const & positions,
                                         std::vector<float> & outValues,
                                         float baseIsoValue,
                                         std::vector<float> const & thicknessLUT,
                                         int lutResolution);

        /// Sample SDF values for shell volumes (material bands between two depth boundaries)
        /// Returns false if GPU sampling fails and CPU fallback should be used
        [[nodiscard]] bool sampleCornersShellVolume(std::vector<Eigen::Vector3f> const & positions,
                                         std::vector<float> & outValues,
                                         std::vector<float> const & outerLUT,
                                         std::vector<float> const & innerLUT,
                                         int lutResolution,
                                         bool isInnermostLayer);

        /// Sample SDF values using precomputed surface-aligned thickness field
        /// This is the surface-color-corrected version that samples from spatial position
        /// instead of color→LUT lookup at interior points
        /// @param outerField 3D grid of outer boundary thickness (position → thickness)
        /// @param innerField 3D grid of inner boundary thickness (position → thickness), empty for single boundary
        /// @param fieldResolution Resolution of thickness field grids (e.g., 128)
        /// @param worldToField 4x4 transform from world coordinates to field grid coordinates
        /// @param isInnermostLayer True if this is the innermost material layer (no inner boundary)
        /// @return False if GPU sampling fails
        [[nodiscard]] bool sampleCornersWithThicknessField(
            std::vector<Eigen::Vector3f> const & positions,
            std::vector<float> & outValues,
            std::vector<float> const & outerField,
            std::vector<float> const & innerField,
            int fieldResolution,
            Eigen::Matrix4f const & worldToField,
            bool isInnermostLayer);

        /// Sample SDF values and gradients at Hermite positions
        /// Returns false if GPU sampling fails and CPU fallback should be used
        [[nodiscard]] bool sampleHermite(std::vector<Eigen::Vector3f> const & positions,
                         std::vector<float> & outValues,
                         std::vector<Eigen::Vector3f> & outGradients,
                         float epsilonOverride = -1.0F);

        /// Get accumulated statistics
        [[nodiscard]] GpuSamplingStats const & getStats() const
        {
            return m_stats;
        }

        /// Check if GPU sampling is available
        [[nodiscard]] bool isGpuAvailable() const
        {
            return m_gpuAvailable;
        }

      private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;

        ComputeCore * m_core{nullptr};
        GpuSamplingConfig m_config{};
        GpuSamplingStats m_stats{};
        bool m_gpuAvailable{false};

        void logError(std::string const & message);
        void logInfo(std::string const & message);
    };
}
