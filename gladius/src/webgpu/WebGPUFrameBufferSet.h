#pragma once

#include "webgpu/WebGPUDispatchPolicy.h"
#include "webgpu/WebGPUShaderAbi.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <webgpu/webgpu_cpp.h>

namespace gladius::webgpu
{
    /**
     * @brief Immutable GPU storage buffers for scene-scoped mesh, beam, and image payloads.
     *
     * Uploaded once when a scene is materialized and shared across all frame submissions
     * and SDF prebakes for that scene generation without per-frame CPU copies or GPU uploads.
     */
    struct WebGPUScenePayloadBuffers
    {
        std::uint64_t resourceId{0u};
        wgpu::Buffer meshPayloadBuffer;
        wgpu::Buffer meshOffsetTableBuffer;
        wgpu::Buffer beamPayloadBuffer;
        wgpu::Buffer beamOffsetTableBuffer;
        wgpu::Buffer imagePayloadBuffer;
        wgpu::Buffer imageOffsetTableBuffer;

        [[nodiscard]] bool hasMeshPayloads() const noexcept { return meshPayloadBuffer != nullptr; }
        [[nodiscard]] bool hasBeamPayloads() const noexcept { return beamPayloadBuffer != nullptr; }
        [[nodiscard]] bool hasImagePayloads() const noexcept { return imagePayloadBuffer != nullptr; }
    };

    /// Retains a completed WebGPU compute output buffer for zero-copy GPU texture presentation.
    struct WebGPUGpuOutputFrame
    {
        wgpu::Buffer outputBuffer;
        std::uint32_t bytesPerRow{0u};
        std::uint32_t rowCount{0u};
        std::shared_ptr<void> bufferLease;
    };

    /// Uploads mesh, beam-lattice, and image-stack payloads once into an immutable GPU bundle.
    [[nodiscard]] std::shared_ptr<WebGPUScenePayloadBuffers> uploadScenePayloadBuffers(
      wgpu::Device const & device,
      wgpu::Queue const & queue,
      std::uint64_t resourceId,
      std::vector<std::vector<float>> const & meshPayloadTable,
      std::vector<std::vector<float>> const & beamPayloadTable,
      std::vector<std::vector<std::uint32_t>> const & imagePayloadTable);

    /**
     * @brief Owns buffers required by one headless WebGPU frame render.
     */
    class WebGPUFrameBufferSet
    {
      public:
        struct FrameBindGroupKey
        {
            WGPUBindGroupLayout layout{};
            WGPUBuffer outputBuffer{};
            std::size_t outputSizeBytes{};
            WGPUBuffer parameterBuffer{};
            std::size_t parameterSizeBytes{};
            WGPUBuffer meshPayloadBuffer{};
            WGPUBuffer meshOffsetTableBuffer{};
            WGPUBuffer beamPayloadBuffer{};
            WGPUBuffer beamOffsetTableBuffer{};
            WGPUBuffer imagePayloadBuffer{};
            WGPUBuffer imageOffsetTableBuffer{};
            WGPUBuffer precomputedUniformBuffer{};
            WGPUTextureView precomputedTextureView{};
            WGPUSampler precomputedSampler{};

            [[nodiscard]] bool operator==(FrameBindGroupKey const &) const noexcept = default;
        };

        void resize(wgpu::Device const & device,
                    std::uint32_t width,
                    std::uint32_t height,
                    std::size_t parameterCount,
                    std::uint32_t rowStridePixels = 0u);
        void writeUniforms(wgpu::Queue const & queue, FrameUniforms const & uniforms) const;
        void writeParameters(wgpu::Queue const & queue, std::vector<float> const & parameters);
        void writeParametersIfChanged(wgpu::Queue const & queue,
                                      std::vector<float> const & parameters,
                                      std::optional<std::uint64_t> sceneResourceId,
                                      std::optional<std::uint64_t> parameterGeneration);
        [[nodiscard]] wgpu::BindGroup getOrCreateFrameBindGroup(
          wgpu::Device const & device,
          wgpu::BindGroupLayout const & layout,
          WebGPUScenePayloadBuffers const * scenePayloads,
          wgpu::Buffer const & precomputedUniformBuffer,
          wgpu::TextureView const & precomputedTextureView,
          wgpu::Sampler const & precomputedSampler = {});
        void reset();

        /// Upload mesh payloads (concatenated, one entry per resource slot).
        /// @param meshPayloadTable Flattened payload data; empty entries are zero-length.
        void setMeshPayloads(wgpu::Device const & device,
                             wgpu::Queue const & queue,
                             std::vector<std::vector<float>> const & meshPayloadTable);

        /// Upload beam lattice payloads (concatenated, one entry per resource slot).
        void setBeamPayloads(wgpu::Device const & device,
                             wgpu::Queue const & queue,
                             std::vector<std::vector<float>> const & beamPayloadTable);

        /// Upload image stack payloads (concatenated, one entry per resource slot).
        void setImagePayloads(wgpu::Device const & device,
                              wgpu::Queue const & queue,
                              std::vector<std::vector<std::uint32_t>> const & imagePayloadTable);

        [[nodiscard]] bool isValid() const noexcept;
        [[nodiscard]] std::size_t getOutputSizeBytes() const noexcept;
        [[nodiscard]] std::size_t getOutputCapacitySizeBytes() const noexcept;
        [[nodiscard]] std::size_t getParameterSizeBytes() const noexcept;
        [[nodiscard]] std::size_t getParameterCapacitySizeBytes() const noexcept;
        [[nodiscard]] bool hasMeshPayloads() const noexcept { return m_meshPayloadBuffer != nullptr; }
        [[nodiscard]] wgpu::Buffer const & getUniformBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getOutputBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getStagingBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getParameterBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getMeshPayloadBuffer() const noexcept
        {
            return m_meshPayloadBuffer;
        }
        [[nodiscard]] wgpu::Buffer const & getMeshOffsetTableBuffer() const noexcept
        {
            return m_meshOffsetTableBuffer;
        }
        [[nodiscard]] bool hasBeamPayloads() const noexcept { return m_beamPayloadBuffer != nullptr; }
        [[nodiscard]] wgpu::Buffer const & getBeamPayloadBuffer() const noexcept
        {
            return m_beamPayloadBuffer;
        }
        [[nodiscard]] wgpu::Buffer const & getBeamOffsetTableBuffer() const noexcept
        {
            return m_beamOffsetTableBuffer;
        }
        [[nodiscard]] bool hasImagePayloads() const noexcept { return m_imagePayloadBuffer != nullptr; }
        [[nodiscard]] wgpu::Buffer const & getImagePayloadBuffer() const noexcept
        {
            return m_imagePayloadBuffer;
        }
        [[nodiscard]] wgpu::Buffer const & getImageOffsetTableBuffer() const noexcept
        {
            return m_imageOffsetTableBuffer;
        }

        /// Create (once) the ray-march metric counter buffers used with FrameRequest::collectMetrics.
        void ensureMetricsBuffers(wgpu::Device const & device);
        [[nodiscard]] bool hasMetricsBuffers() const noexcept { return m_metricsBuffer != nullptr; }
        [[nodiscard]] wgpu::Buffer const & getMetricsBuffer() const noexcept { return m_metricsBuffer; }
        [[nodiscard]] wgpu::Buffer const & getMetricsStagingBuffer() const noexcept
        {
            return m_metricsStagingBuffer;
        }

        /// Create (once) the timestamp query set and resolve buffer used with
        /// FrameRequest::collectGpuTiming on devices supporting the timestamp-query feature.
        void ensureTimestampBuffers(wgpu::Device const & device);
        [[nodiscard]] bool hasTimestampBuffers() const noexcept
        {
            return m_querySet != nullptr && m_timestampResolveBuffer != nullptr;
        }
        [[nodiscard]] wgpu::QuerySet const & getQuerySet() const noexcept { return m_querySet; }
        [[nodiscard]] wgpu::Buffer const & getTimestampResolveBuffer() const noexcept
        {
            return m_timestampResolveBuffer;
        }
        [[nodiscard]] wgpu::Buffer const & getTimestampStagingBuffer() const noexcept
        {
            return m_timestampStagingBuffer;
        }

        /// Ensure the 3D precomputed SDF texture and uniform buffer exist (at least 1x1x1).
        void ensurePrecomputedSdfResources(wgpu::Device const & device,
                                         wgpu::Queue const & queue,
                                         std::uint32_t gridSize);
        void writePrecomputedSdfUniforms(wgpu::Queue const & queue,
                                         PrecomputedSdfUniforms const & uniforms) const;
        void invalidatePrecomputedSdf(wgpu::Queue const & queue);
        [[nodiscard]] bool hasPrecomputedSdfResources() const noexcept
        {
            return m_precomputedSdfUniformBuffer != nullptr && m_precomputedSdfTextureView != nullptr;
        }
        [[nodiscard]] std::uint32_t getPrecomputedSdfGridSize() const noexcept
        {
            return m_precomputedSdfGridSize;
        }
        [[nodiscard]] wgpu::Buffer const & getPrecomputedSdfUniformBuffer() const noexcept
        {
            return m_precomputedSdfUniformBuffer;
        }
        [[nodiscard]] wgpu::TextureView const & getPrecomputedSdfTextureView() const noexcept
        {
            return m_precomputedSdfTextureView;
        }

      private:
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        std::size_t m_outputSizeBytes{};
        std::size_t m_outputCapacitySizeBytes{};
        std::size_t m_parameterSizeBytes{};
        std::size_t m_parameterCapacitySizeBytes{};
        wgpu::Buffer m_uniformBuffer;
        wgpu::Buffer m_outputBuffer;
        wgpu::Buffer m_stagingBuffer;
        wgpu::Buffer m_parameterBuffer;
        wgpu::Buffer m_meshPayloadBuffer;
        wgpu::Buffer m_meshOffsetTableBuffer;
        std::size_t m_meshPayloadBufferSize{};
        wgpu::Buffer m_beamPayloadBuffer;
        wgpu::Buffer m_beamOffsetTableBuffer;
        std::size_t m_beamPayloadBufferSize{};
        wgpu::Buffer m_imagePayloadBuffer;
        wgpu::Buffer m_imageOffsetTableBuffer;
        std::size_t m_imagePayloadBufferSize{};
        wgpu::Buffer m_metricsBuffer;
        wgpu::Buffer m_metricsStagingBuffer;
        wgpu::QuerySet m_querySet;
        wgpu::Buffer m_timestampResolveBuffer;
        wgpu::Buffer m_timestampStagingBuffer;
        wgpu::Buffer m_precomputedSdfUniformBuffer;
        wgpu::Texture m_precomputedSdfTexture;
        wgpu::TextureView m_precomputedSdfTextureView;
        std::uint32_t m_precomputedSdfGridSize{0u};
        std::optional<std::uint64_t> m_uploadedParameterSceneId;
        std::optional<std::uint64_t> m_uploadedParameterGeneration;
        std::size_t m_uploadedParameterCount{0u};
        std::optional<FrameBindGroupKey> m_cachedBindGroupKey;
        wgpu::BindGroup m_cachedBindGroup;
    };
}