#include "DualContouringGpuSampling.h"

#include "ComputeContext.h"
#include "EventLogger.h"
#include "ResourceContext.h"
#include "compute/ComputeCore.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include <fmt/format.h>
#include <numeric>

namespace gladius::dual_contouring
{
    namespace
    {
        /// Morton code for spatial hashing (simplified 3D version)
        [[nodiscard]] std::uint64_t mortonEncode(std::uint32_t x, std::uint32_t y, std::uint32_t z)
        {
            auto splitBy3 = [](std::uint32_t a) -> std::uint64_t
            {
                std::uint64_t x = a & 0x1fffff;
                x = (x | x << 32) & 0x1f00000000ffff;
                x = (x | x << 16) & 0x1f0000ff0000ff;
                x = (x | x << 8) & 0x100f00f00f00f00f;
                x = (x | x << 4) & 0x10c30c30c30c30c3;
                x = (x | x << 2) & 0x1249249249249249;
                return x;
            };

            return splitBy3(x) | (splitBy3(y) << 1) | (splitBy3(z) << 2);
        }

        /// Convert position to quantized Morton code for caching
        [[nodiscard]] std::uint64_t positionToMorton(Eigen::Vector3f const & pos,
                                                     Eigen::Vector3f const & gridMin,
                                                     Eigen::Vector3f const & gridSpacing)
        {
            Eigen::Vector3f const offset = (pos - gridMin).cwiseQuotient(gridSpacing);
            auto const x = static_cast<std::uint32_t>(std::max(0.0F, std::floor(offset.x())));
            auto const y = static_cast<std::uint32_t>(std::max(0.0F, std::floor(offset.y())));
            auto const z = static_cast<std::uint32_t>(std::max(0.0F, std::floor(offset.z())));
            return mortonEncode(x, y, z);
        }
    }

    /// Internal implementation (pimpl)
    struct GpuSamplingSession::Impl
    {
        std::unordered_map<std::uint64_t, float> cornerCache;
        std::unordered_map<std::uint64_t, std::pair<float, Eigen::Vector3f>> hermiteCache;

        // GPU buffers for batched operations
        cl::Buffer positionBuffer;
        cl::Buffer valueBuffer;
        cl::Buffer gradientBuffer;

        // Grid parameters for Morton encoding
        Eigen::Vector3f gridMin{Eigen::Vector3f::Zero()};
        Eigen::Vector3f gridSpacing{Eigen::Vector3f::Ones()};

        bool buffersAllocated{false};
        std::size_t allocatedBufferSize{0U};

        void ensureBuffers(ComputeContext & context, std::size_t positionCount, bool needGradients)
        {
            std::size_t const requiredSize = positionCount;
            if (buffersAllocated && requiredSize <= allocatedBufferSize)
            {
                return;
            }

            try
            {
                // Allocate with some headroom
                std::size_t const allocSize = requiredSize + (requiredSize / 4U);

                positionBuffer =
                  cl::Buffer(context.GetContext(), CL_MEM_READ_ONLY, allocSize * sizeof(cl_float4));

                valueBuffer =
                  cl::Buffer(context.GetContext(), CL_MEM_WRITE_ONLY, allocSize * sizeof(cl_float));

                if (needGradients)
                {
                    gradientBuffer = cl::Buffer(
                      context.GetContext(), CL_MEM_WRITE_ONLY, allocSize * sizeof(cl_float4));
                }

                allocatedBufferSize = allocSize;
                buffersAllocated = true;
            }
            catch (std::exception const & ex)
            {
                throw std::runtime_error(
                  fmt::format("Failed to allocate GPU buffers: {}", ex.what()));
            }
        }
    };

    GpuSamplingSession::GpuSamplingSession(ComputeCore & core, GpuSamplingConfig config)
        : m_impl(std::make_unique<Impl>())
        , m_core(&core)
        , m_config(std::move(config))
    {
        auto context = core.getComputeContext();
        if (!context || !context->isValid())
        {
            m_gpuAvailable = false;
            logError("GPU sampling session: ComputeContext not available");
            if (!m_config.fallbackToCpu)
            {
                throw std::runtime_error("GPU not available and CPU fallback disabled");
            }
            return;
        }

        m_gpuAvailable = true;
        logInfo("GPU sampling session initialized");
    }

    GpuSamplingSession::~GpuSamplingSession() = default;

    bool GpuSamplingSession::sampleCorners(std::vector<Eigen::Vector3f> const & positions,
                                           std::vector<float> & outValues)
    {
        if (positions.empty())
        {
            outValues.clear();
            return true;
        }

        if (!m_gpuAvailable)
        {
            return false;
        }

        outValues.resize(positions.size());

        try
        {
            auto context = m_core->getComputeContext();
            if (!context || !context->isValid())
            {
                return false;
            }

            // Check cache first if enabled
            std::vector<std::size_t> uncachedIndices;
            uncachedIndices.reserve(positions.size());

            if (m_config.enableCaching)
            {
                for (std::size_t i = 0U; i < positions.size(); ++i)
                {
                    auto const morton =
                      positionToMorton(positions[i], m_impl->gridMin, m_impl->gridSpacing);
                    auto it = m_impl->cornerCache.find(morton);
                    if (it != m_impl->cornerCache.end())
                    {
                        outValues[i] = it->second;
                        ++m_stats.cacheHits;
                    }
                    else
                    {
                        uncachedIndices.push_back(i);
                        ++m_stats.cacheMisses;
                    }
                }
            }
            else
            {
                uncachedIndices.resize(positions.size());
                std::iota(uncachedIndices.begin(), uncachedIndices.end(), 0U);
            }

            if (uncachedIndices.empty())
            {
                return true;
            }

            // Get the sampling program
            auto * program = m_core->getProgramManager().getDualContouringSamplingProgram();
            if (!program)
            {
                logError("DualContouringSamplingProgram not available");
                return false;
            }

            // Collect uncached positions for GPU batch
            std::vector<Eigen::Vector3f> batchPositions;
            batchPositions.reserve(uncachedIndices.size());
            for (auto idx : uncachedIndices)
            {
                batchPositions.push_back(positions[idx]);
            }

            // Batch GPU sampling
            std::vector<float> batchValues;
            program->sampleCorners(
              batchPositions, batchValues, *m_core->getPrimitives(), m_config.isoValue);

            // Populate output and cache
            for (std::size_t i = 0U; i < uncachedIndices.size(); ++i)
            {
                auto const idx = uncachedIndices[i];
                outValues[idx] = batchValues[i];

                if (m_config.enableCaching)
                {
                    auto const morton =
                      positionToMorton(positions[idx], m_impl->gridMin, m_impl->gridSpacing);
                    m_impl->cornerCache[morton] = batchValues[i];
                }
            }

            m_stats.cornerBatches += 1;
            m_stats.totalCornerSamples += uncachedIndices.size();

            return true;
        }
        catch (std::exception const & ex)
        {
            logError(fmt::format("Corner sampling failed: {}", ex.what()));
            return false;
        }
    }

    bool GpuSamplingSession::sampleCornersVariableThickness(std::vector<Eigen::Vector3f> const & positions,
                                           std::vector<float> & outValues,
                                           float baseIsoValue,
                                           std::vector<float> const & thicknessLUT,
                                           int lutResolution)
    {
        if (positions.empty())
        {
            outValues.clear();
            return true;
        }

        if (!m_gpuAvailable)
        {
            return false;
        }

        outValues.resize(positions.size());

        try
        {
            auto context = m_core->getComputeContext();
            if (!context || !context->isValid())
            {
                return false;
            }

            // Check cache first if enabled
            std::vector<std::size_t> uncachedIndices;
            uncachedIndices.reserve(positions.size());

            if (m_config.enableCaching)
            {
                for (std::size_t i = 0U; i < positions.size(); ++i)
                {
                    auto const morton =
                      positionToMorton(positions[i], m_impl->gridMin, m_impl->gridSpacing);
                    auto it = m_impl->cornerCache.find(morton);
                    if (it != m_impl->cornerCache.end())
                    {
                        outValues[i] = it->second;
                        ++m_stats.cacheHits;
                    }
                    else
                    {
                        uncachedIndices.push_back(i);
                        ++m_stats.cacheMisses;
                    }
                }
            }
            else
            {
                uncachedIndices.resize(positions.size());
                std::iota(uncachedIndices.begin(), uncachedIndices.end(), 0U);
            }

            if (uncachedIndices.empty())
            {
                return true;
            }

            // Get the sampling program
            auto * program = m_core->getProgramManager().getDualContouringSamplingProgram();
            if (!program)
            {
                logError("DualContouringSamplingProgram not available");
                return false;
            }

            // Collect uncached positions for GPU batch
            std::vector<Eigen::Vector3f> batchPositions;
            batchPositions.reserve(uncachedIndices.size());
            for (auto idx : uncachedIndices)
            {
                batchPositions.push_back(positions[idx]);
            }

            // Batch GPU sampling
            std::vector<float> batchValues;
            program->sampleCornersVariableThickness(
              batchPositions, batchValues, *m_core->getPrimitives(), baseIsoValue, thicknessLUT, lutResolution);

            // Populate output and cache
            for (std::size_t i = 0U; i < uncachedIndices.size(); ++i)
            {
                auto const idx = uncachedIndices[i];
                outValues[idx] = batchValues[i];

                if (m_config.enableCaching)
                {
                    auto const morton =
                      positionToMorton(positions[idx], m_impl->gridMin, m_impl->gridSpacing);
                    m_impl->cornerCache[morton] = batchValues[i];
                }
            }

            m_stats.cornerBatches += 1;
            m_stats.totalCornerSamples += uncachedIndices.size();

            return true;
        }
        catch (std::exception const & ex)
        {
            logError(fmt::format("Corner sampling failed: {}", ex.what()));
            return false;
        }
    }

    bool GpuSamplingSession::sampleCornersShellVolume(
        std::vector<Eigen::Vector3f> const & positions,
        std::vector<float> & outValues,
        std::vector<float> const & outerLUT,
        std::vector<float> const & innerLUT,
        int lutResolution,
        bool isInnermostLayer)
    {
        if (positions.empty())
        {
            outValues.clear();
            return true;
        }

        if (!m_gpuAvailable)
        {
            return false;
        }

        outValues.resize(positions.size());

        try
        {
            auto context = m_core->getComputeContext();
            if (!context || !context->isValid())
            {
                return false;
            }

            // No caching for shell volumes - LUT varies per layer
            auto * program = m_core->getProgramManager().getDualContouringSamplingProgram();
            if (!program)
            {
                logError("DualContouringSamplingProgram not available");
                return false;
            }

            // Direct GPU sampling without caching
            program->sampleCornersShellVolume(
                positions, outValues, *m_core->getPrimitives(),
                outerLUT, innerLUT, lutResolution, isInnermostLayer);

            m_stats.cornerBatches += 1;
            m_stats.totalCornerSamples += positions.size();

            return true;
        }
        catch (std::exception const & ex)
        {
            logError(fmt::format("Shell volume sampling failed: {}", ex.what()));
            return false;
        }
    }

    bool GpuSamplingSession::sampleHermite(std::vector<Eigen::Vector3f> const & positions,
                                           std::vector<float> & outValues,
                                           std::vector<Eigen::Vector3f> & outGradients,
                                           float epsilonOverride)
    {
        if (positions.empty())
        {
            outValues.clear();
            outGradients.clear();
            return true;
        }

        if (!m_gpuAvailable)
        {
            return false;
        }

        outValues.resize(positions.size());
        outGradients.resize(positions.size());

        float const epsilon = (epsilonOverride > 0.0F) ? epsilonOverride : m_config.gradientEpsilon;

        try
        {
            // Check cache first if enabled
            std::vector<std::size_t> uncachedIndices;
            uncachedIndices.reserve(positions.size());

            if (m_config.enableCaching)
            {
                for (std::size_t i = 0U; i < positions.size(); ++i)
                {
                    auto const morton =
                      positionToMorton(positions[i], m_impl->gridMin, m_impl->gridSpacing);
                    auto it = m_impl->hermiteCache.find(morton);
                    if (it != m_impl->hermiteCache.end())
                    {
                        outValues[i] = it->second.first;
                        outGradients[i] = it->second.second;
                        ++m_stats.cacheHits;
                    }
                    else
                    {
                        uncachedIndices.push_back(i);
                        ++m_stats.cacheMisses;
                    }
                }
            }
            else
            {
                uncachedIndices.resize(positions.size());
                std::iota(uncachedIndices.begin(), uncachedIndices.end(), 0U);
            }

            if (uncachedIndices.empty())
            {
                return true;
            }

            // Get the sampling program
            auto * program = m_core->getProgramManager().getDualContouringSamplingProgram();
            if (!program)
            {
                logError("DualContouringSamplingProgram not available");
                return false;
            }

            // Collect uncached positions for GPU batch
            std::vector<Eigen::Vector3f> batchPositions;
            batchPositions.reserve(uncachedIndices.size());
            for (auto idx : uncachedIndices)
            {
                batchPositions.push_back(positions[idx]);
            }

            // Batch GPU sampling
            std::vector<float> batchValues;
            std::vector<Eigen::Vector3f> batchGradients;
            program->sampleHermite(batchPositions,
                                   batchValues,
                                   batchGradients,
                                   *m_core->getPrimitives(),
                                   m_config.isoValue,
                                   epsilon);

            // Populate output and cache
            for (std::size_t i = 0U; i < uncachedIndices.size(); ++i)
            {
                auto const idx = uncachedIndices[i];
                outValues[idx] = batchValues[i];
                outGradients[idx] = batchGradients[i];

                if (m_config.enableCaching)
                {
                    auto const morton =
                      positionToMorton(positions[idx], m_impl->gridMin, m_impl->gridSpacing);
                    m_impl->hermiteCache[morton] = {batchValues[i], batchGradients[i]};
                }
            }

            m_stats.hermiteBatches += 1;
            m_stats.totalHermiteSamples += uncachedIndices.size();

            return true;
        }
        catch (std::exception const & ex)
        {
            logError(fmt::format("Hermite sampling failed: {}", ex.what()));
            return false;
        }
    }

    void GpuSamplingSession::logError(std::string const & message)
    {
        // Logger is private in ComputeCore, so we skip logging for now
        // Future: Add public SharedLogger access or pass logger in constructor
    }

    void GpuSamplingSession::logInfo(std::string const & message)
    {
        // Logger is private in ComputeCore, so we skip logging for now
        // Future: Add public SharedLogger access or pass logger in constructor
    }
}
