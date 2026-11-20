#include "webgpu/WebGPUFrameBufferSet.h"
#include "webgpu/WebGPUBufferCapacity.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gladius::webgpu
{
    void WebGPUFrameBufferSet::resize(wgpu::Device const & device,
                                       std::uint32_t const width,
                                       std::uint32_t const height,
                                       std::size_t const parameterCount,
                                       std::uint32_t const rowStridePixels)
    {
        auto const effectiveRowStride = std::max(width, rowStridePixels);
        auto const dispatchSize = calculateSliceDispatchSize(effectiveRowStride, height);
        if (!dispatchSize.has_value())
        {
            throw std::invalid_argument("Invalid WebGPU frame dimensions");
        }

        auto const parameterSizeBytes = std::max(sizeof(float), parameterCount * sizeof(float));
        if (m_width == width && m_height == height &&
            m_outputSizeBytes == dispatchSize->outputSizeBytes &&
            m_parameterSizeBytes == parameterSizeBytes && isValid())
        {
            return;
        }

        if (!m_uniformBuffer)
        {
            wgpu::BufferDescriptor uniformDescriptor;
            uniformDescriptor.label = "Gladius frame uniforms";
            uniformDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            uniformDescriptor.size = sizeof(FrameUniforms);
            auto uniformBuffer = device.CreateBuffer(&uniformDescriptor);
            if (!uniformBuffer)
            {
                throw std::runtime_error("Unable to allocate WebGPU frame uniforms");
            }
            m_uniformBuffer = std::move(uniformBuffer);
        }

        if (!m_outputBuffer || !m_stagingBuffer ||
            m_outputCapacitySizeBytes < dispatchSize->outputSizeBytes)
        {
            wgpu::BufferDescriptor outputDescriptor;
            outputDescriptor.label = "Gladius frame output";
            outputDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
            auto const outputCapacitySizeBytes =
              detail::growBufferCapacity(m_outputCapacitySizeBytes, dispatchSize->outputSizeBytes);
            outputDescriptor.size = outputCapacitySizeBytes;
            auto outputBuffer = device.CreateBuffer(&outputDescriptor);

            wgpu::BufferDescriptor stagingDescriptor;
            stagingDescriptor.label = "Gladius frame readback";
            stagingDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
            stagingDescriptor.size = outputCapacitySizeBytes;
            auto stagingBuffer = device.CreateBuffer(&stagingDescriptor);
            if (!outputBuffer || !stagingBuffer)
            {
                throw std::runtime_error("Unable to allocate WebGPU frame output buffers");
            }

            m_outputBuffer = std::move(outputBuffer);
            m_stagingBuffer = std::move(stagingBuffer);
            m_outputCapacitySizeBytes = outputCapacitySizeBytes;
        }

        if (!m_parameterBuffer || m_parameterCapacitySizeBytes < parameterSizeBytes)
        {
            wgpu::BufferDescriptor parameterDescriptor;
            parameterDescriptor.label = "Gladius frame parameters";
            parameterDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            auto const parameterCapacitySizeBytes =
              detail::growBufferCapacity(m_parameterCapacitySizeBytes, parameterSizeBytes);
            parameterDescriptor.size = parameterCapacitySizeBytes;
            auto parameterBuffer = device.CreateBuffer(&parameterDescriptor);
            if (!parameterBuffer)
            {
                throw std::runtime_error("Unable to allocate WebGPU frame parameters");
            }
            m_parameterBuffer = std::move(parameterBuffer);
            m_parameterCapacitySizeBytes = parameterCapacitySizeBytes;
            m_uploadedParameterSceneId.reset();
            m_uploadedParameterGeneration.reset();
            m_uploadedParameterCount = 0u;
        }

        if (!isValid())
        {
            throw std::runtime_error("Unable to allocate WebGPU frame buffers");
        }

        m_width = width;
        m_height = height;
        m_outputSizeBytes = dispatchSize->outputSizeBytes;
        m_parameterSizeBytes = parameterSizeBytes;
    }

    void WebGPUFrameBufferSet::writeUniforms(wgpu::Queue const & queue, FrameUniforms const & uniforms) const
    {
        queue.WriteBuffer(m_uniformBuffer, 0u, &uniforms, sizeof(uniforms));
    }

    void WebGPUFrameBufferSet::writeParameters(wgpu::Queue const & queue, std::vector<float> const & parameters)
    {
        float const zero{};
        auto const * data = parameters.empty() ? &zero : parameters.data();
        auto const size = parameters.empty() ? sizeof(zero) : parameters.size() * sizeof(float);
        queue.WriteBuffer(m_parameterBuffer, 0u, data, size);
        m_uploadedParameterSceneId.reset();
        m_uploadedParameterGeneration.reset();
        m_uploadedParameterCount = parameters.size();
    }

    void WebGPUFrameBufferSet::writeParametersIfChanged(
      wgpu::Queue const & queue,
      std::vector<float> const & parameters,
      std::optional<std::uint64_t> const sceneResourceId,
      std::optional<std::uint64_t> const parameterGeneration)
    {
        if (sceneResourceId.has_value() && parameterGeneration.has_value() &&
            m_uploadedParameterSceneId == sceneResourceId &&
            m_uploadedParameterGeneration == parameterGeneration &&
            m_uploadedParameterCount == parameters.size())
        {
            return;
        }

        writeParameters(queue, parameters);
        m_uploadedParameterSceneId = sceneResourceId;
        m_uploadedParameterGeneration = parameterGeneration;
    }

    wgpu::BindGroup WebGPUFrameBufferSet::getOrCreateFrameBindGroup(
      wgpu::Device const & device,
      wgpu::BindGroupLayout const & layout,
      WebGPUScenePayloadBuffers const * const scenePayloads,
      wgpu::Buffer const & precomputedUniformBuffer,
      wgpu::TextureView const & precomputedTextureView,
      wgpu::Sampler const & precomputedSampler)
    {
        wgpu::Buffer const & meshPayload =
          scenePayloads != nullptr ? scenePayloads->meshPayloadBuffer : m_meshPayloadBuffer;
        wgpu::Buffer const & meshOffsets =
          scenePayloads != nullptr ? scenePayloads->meshOffsetTableBuffer : m_meshOffsetTableBuffer;
        wgpu::Buffer const & beamPayload =
          scenePayloads != nullptr ? scenePayloads->beamPayloadBuffer : m_beamPayloadBuffer;
        wgpu::Buffer const & beamOffsets =
          scenePayloads != nullptr ? scenePayloads->beamOffsetTableBuffer : m_beamOffsetTableBuffer;
        wgpu::Buffer const & imagePayload =
          scenePayloads != nullptr ? scenePayloads->imagePayloadBuffer : m_imagePayloadBuffer;
        wgpu::Buffer const & imageOffsets =
          scenePayloads != nullptr ? scenePayloads->imageOffsetTableBuffer : m_imageOffsetTableBuffer;

        FrameBindGroupKey const key{
          .layout = layout.Get(),
          .outputBuffer = m_outputBuffer.Get(),
          .outputSizeBytes = m_outputSizeBytes,
          .parameterBuffer = m_parameterBuffer.Get(),
          .parameterSizeBytes = m_parameterSizeBytes,
          .meshPayloadBuffer = meshPayload.Get(),
          .meshOffsetTableBuffer = meshOffsets.Get(),
          .beamPayloadBuffer = beamPayload.Get(),
          .beamOffsetTableBuffer = beamOffsets.Get(),
          .imagePayloadBuffer = imagePayload.Get(),
          .imageOffsetTableBuffer = imageOffsets.Get(),
          .precomputedUniformBuffer = precomputedUniformBuffer.Get(),
          .precomputedTextureView = precomputedTextureView.Get(),
          .precomputedSampler = precomputedSampler.Get()};

        if (m_cachedBindGroup && m_cachedBindGroupKey.has_value() && *m_cachedBindGroupKey == key)
        {
            return m_cachedBindGroup;
        }

        wgpu::BindGroupEntry bindGroupEntries[13]{};
        bindGroupEntries[0].binding = 0u;
        bindGroupEntries[0].buffer = m_uniformBuffer;
        bindGroupEntries[0].size = sizeof(FrameUniforms);
        bindGroupEntries[1].binding = 1u;
        bindGroupEntries[1].buffer = m_outputBuffer;
        bindGroupEntries[1].size = m_outputSizeBytes;
        bindGroupEntries[2].binding = 2u;
        bindGroupEntries[2].buffer = m_parameterBuffer;
        bindGroupEntries[2].size = m_parameterSizeBytes;
        std::size_t entryCount = 3u;

        if (meshPayload)
        {
            bindGroupEntries[entryCount].binding = 4u;
            bindGroupEntries[entryCount].buffer = meshPayload;
            bindGroupEntries[entryCount].size = meshPayload.GetSize();
            ++entryCount;
            bindGroupEntries[entryCount].binding = 5u;
            bindGroupEntries[entryCount].buffer = meshOffsets;
            bindGroupEntries[entryCount].size = meshOffsets.GetSize();
            ++entryCount;
        }
        if (beamPayload)
        {
            bindGroupEntries[entryCount].binding = 6u;
            bindGroupEntries[entryCount].buffer = beamPayload;
            bindGroupEntries[entryCount].size = beamPayload.GetSize();
            ++entryCount;
            bindGroupEntries[entryCount].binding = 7u;
            bindGroupEntries[entryCount].buffer = beamOffsets;
            bindGroupEntries[entryCount].size = beamOffsets.GetSize();
            ++entryCount;
        }
        if (imagePayload)
        {
            bindGroupEntries[entryCount].binding = 8u;
            bindGroupEntries[entryCount].buffer = imagePayload;
            bindGroupEntries[entryCount].size = imagePayload.GetSize();
            ++entryCount;
            bindGroupEntries[entryCount].binding = 9u;
            bindGroupEntries[entryCount].buffer = imageOffsets;
            bindGroupEntries[entryCount].size = imageOffsets.GetSize();
            ++entryCount;
        }

        bindGroupEntries[entryCount].binding = 10u;
        bindGroupEntries[entryCount].buffer = m_metricsBuffer;
        bindGroupEntries[entryCount].size = FRAME_METRICS_SIZE_BYTES;
        ++entryCount;

        bindGroupEntries[entryCount].binding = 11u;
        bindGroupEntries[entryCount].buffer = precomputedUniformBuffer;
        bindGroupEntries[entryCount].size = sizeof(PrecomputedSdfUniforms);
        ++entryCount;

        bindGroupEntries[entryCount].binding = 12u;
        bindGroupEntries[entryCount].textureView = precomputedTextureView;
        ++entryCount;

        if (precomputedSampler)
        {
            bindGroupEntries[entryCount].binding = 13u;
            bindGroupEntries[entryCount].sampler = precomputedSampler;
            ++entryCount;
        }

        wgpu::BindGroupDescriptor bindGroupDescriptor;
        bindGroupDescriptor.layout = layout;
        bindGroupDescriptor.entryCount = entryCount;
        bindGroupDescriptor.entries = bindGroupEntries;
        m_cachedBindGroup = device.CreateBindGroup(&bindGroupDescriptor);
        m_cachedBindGroupKey = key;
        return m_cachedBindGroup;
    }

    std::shared_ptr<WebGPUScenePayloadBuffers> uploadScenePayloadBuffers(
      wgpu::Device const & device,
      wgpu::Queue const & queue,
      std::uint64_t const resourceId,
      std::vector<std::vector<float>> const & meshPayloadTable,
      std::vector<std::vector<float>> const & beamPayloadTable,
      std::vector<std::vector<std::uint32_t>> const & imagePayloadTable)
    {
        auto bundle = std::make_shared<WebGPUScenePayloadBuffers>();
        bundle->resourceId = resourceId;

        auto const uploadFloatPayloads =
          [&](std::vector<std::vector<float>> const & table,
              char const * const dataLabel,
              char const * const offsetLabel,
              char const * const overflowMessage,
              wgpu::Buffer & dataBuffer,
              wgpu::Buffer & offsetBuffer)
        {
            if (table.empty())
            {
                return;
            }
            std::vector<std::uint32_t> offsetTable(table.size() * 2u, 0u);
            std::size_t totalFloats = 0u;
            for (std::size_t slot = 0u; slot < table.size(); ++slot)
            {
                auto const & payload = table[slot];
                if (payload.empty())
                {
                    continue;
                }
                if (totalFloats > std::numeric_limits<std::uint32_t>::max() ||
                    payload.size() > std::numeric_limits<std::uint32_t>::max() - totalFloats)
                {
                    throw std::length_error(overflowMessage);
                }
                offsetTable[slot * 2u] = static_cast<std::uint32_t>(totalFloats);
                offsetTable[slot * 2u + 1u] = static_cast<std::uint32_t>(payload.size());
                totalFloats += payload.size();
            }
            if (totalFloats == 0u)
            {
                return;
            }
            wgpu::BufferDescriptor dataDesc;
            dataDesc.label = dataLabel;
            dataDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            dataDesc.size = totalFloats * sizeof(float);
            dataBuffer = device.CreateBuffer(&dataDesc);

            auto const offsetTableBytes = offsetTable.size() * sizeof(std::uint32_t);
            wgpu::BufferDescriptor offsetDesc;
            offsetDesc.label = offsetLabel;
            offsetDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            offsetDesc.size = offsetTableBytes;
            offsetBuffer = device.CreateBuffer(&offsetDesc);
            queue.WriteBuffer(offsetBuffer, 0u, offsetTable.data(), offsetTableBytes);

            std::size_t byteOffset = 0u;
            for (auto const & payload : table)
            {
                if (payload.empty())
                {
                    continue;
                }
                auto const bytes = payload.size() * sizeof(float);
                queue.WriteBuffer(dataBuffer, byteOffset, payload.data(), bytes);
                byteOffset += bytes;
            }
        };

        uploadFloatPayloads(meshPayloadTable,
                            "Gladius scene mesh payloads",
                            "Gladius scene mesh offset table",
                            "WebGPU mesh payload table exceeds 32-bit addressing",
                            bundle->meshPayloadBuffer,
                            bundle->meshOffsetTableBuffer);
        uploadFloatPayloads(beamPayloadTable,
                            "Gladius scene beam payloads",
                            "Gladius scene beam offset table",
                            "WebGPU beam payload table exceeds 32-bit addressing",
                            bundle->beamPayloadBuffer,
                            bundle->beamOffsetTableBuffer);

        if (!imagePayloadTable.empty())
        {
            std::vector<std::uint32_t> offsetTable(imagePayloadTable.size() * 2u, 0u);
            std::size_t totalWords = 0u;
            for (std::size_t slot = 0u; slot < imagePayloadTable.size(); ++slot)
            {
                auto const & payload = imagePayloadTable[slot];
                if (payload.empty())
                {
                    continue;
                }
                if (totalWords > std::numeric_limits<std::uint32_t>::max() ||
                    payload.size() > std::numeric_limits<std::uint32_t>::max() - totalWords)
                {
                    throw std::length_error("WebGPU image payload table exceeds 32-bit addressing");
                }
                offsetTable[slot * 2u] = static_cast<std::uint32_t>(totalWords);
                offsetTable[slot * 2u + 1u] = static_cast<std::uint32_t>(payload.size());
                totalWords += payload.size();
            }
            if (totalWords > 0u)
            {
                wgpu::BufferDescriptor dataDesc;
                dataDesc.label = "Gladius scene image payloads";
                dataDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                dataDesc.size = totalWords * sizeof(std::uint32_t);
                bundle->imagePayloadBuffer = device.CreateBuffer(&dataDesc);

                auto const offsetTableBytes = offsetTable.size() * sizeof(std::uint32_t);
                wgpu::BufferDescriptor offsetDesc;
                offsetDesc.label = "Gladius scene image offset table";
                offsetDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                offsetDesc.size = offsetTableBytes;
                bundle->imageOffsetTableBuffer = device.CreateBuffer(&offsetDesc);
                queue.WriteBuffer(
                  bundle->imageOffsetTableBuffer, 0u, offsetTable.data(), offsetTableBytes);

                std::size_t byteOffset = 0u;
                for (auto const & payload : imagePayloadTable)
                {
                    if (payload.empty())
                    {
                        continue;
                    }
                    auto const bytes = payload.size() * sizeof(std::uint32_t);
                    queue.WriteBuffer(bundle->imagePayloadBuffer, byteOffset, payload.data(), bytes);
                    byteOffset += bytes;
                }
            }
        }

        return bundle;
    }

    void WebGPUFrameBufferSet::setMeshPayloads(wgpu::Device const & device,
                                               wgpu::Queue const & queue,
                                               std::vector<std::vector<float>> const & meshPayloadTable)
    {
        std::vector<std::uint32_t> offsetTable(meshPayloadTable.size() * 2u, 0u);
        std::size_t totalFloats = 0u;
        for (std::size_t slot = 0u; slot < meshPayloadTable.size(); ++slot)
        {
            auto const & payload = meshPayloadTable[slot];
            if (payload.empty())
            {
                continue;
            }
            if (totalFloats > std::numeric_limits<std::uint32_t>::max() ||
                payload.size() > std::numeric_limits<std::uint32_t>::max() - totalFloats)
            {
                throw std::length_error("WebGPU mesh payload table exceeds 32-bit addressing");
            }
            offsetTable[slot * 2u] = static_cast<std::uint32_t>(totalFloats);
            offsetTable[slot * 2u + 1u] = static_cast<std::uint32_t>(payload.size());
            totalFloats += payload.size();
        }

        if (totalFloats == 0u)
        {
            m_meshPayloadBuffer = nullptr;
            m_meshOffsetTableBuffer = nullptr;
            return;
        }

        auto const totalBytes = totalFloats * sizeof(float);
        if (!m_meshPayloadBuffer || m_meshPayloadBufferSize < totalBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius mesh payloads";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = totalBytes;
            m_meshPayloadBuffer = device.CreateBuffer(&descriptor);
            m_meshPayloadBufferSize = totalBytes;
        }

        auto const offsetTableBytes = offsetTable.size() * sizeof(std::uint32_t);
        if (!m_meshOffsetTableBuffer || m_meshOffsetTableBuffer.GetSize() < offsetTableBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius mesh offset table";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = offsetTableBytes;
            m_meshOffsetTableBuffer = device.CreateBuffer(&descriptor);
        }
        queue.WriteBuffer(m_meshOffsetTableBuffer,
                          0u,
                          offsetTable.data(),
                          offsetTableBytes);

        std::size_t offset = 0u;
        for (auto const & payload : meshPayloadTable)
        {
            if (payload.empty())
            {
                continue;
            }
            auto const bytes = payload.size() * sizeof(float);
            queue.WriteBuffer(m_meshPayloadBuffer, offset, payload.data(), bytes);
            offset += bytes;
        }
    }

    void WebGPUFrameBufferSet::setBeamPayloads(wgpu::Device const & device,
                                               wgpu::Queue const & queue,
                                               std::vector<std::vector<float>> const & beamPayloadTable)
    {
        std::vector<std::uint32_t> offsetTable(beamPayloadTable.size() * 2u, 0u);
        std::size_t totalFloats = 0u;
        for (std::size_t slot = 0u; slot < beamPayloadTable.size(); ++slot)
        {
            auto const & payload = beamPayloadTable[slot];
            if (payload.empty())
            {
                continue;
            }
            if (totalFloats > std::numeric_limits<std::uint32_t>::max() ||
                payload.size() > std::numeric_limits<std::uint32_t>::max() - totalFloats)
            {
                throw std::length_error("WebGPU beam payload table exceeds 32-bit addressing");
            }
            offsetTable[slot * 2u] = static_cast<std::uint32_t>(totalFloats);
            offsetTable[slot * 2u + 1u] = static_cast<std::uint32_t>(payload.size());
            totalFloats += payload.size();
        }

        if (totalFloats == 0u)
        {
            m_beamPayloadBuffer = nullptr;
            m_beamOffsetTableBuffer = nullptr;
            return;
        }

        auto const totalBytes = totalFloats * sizeof(float);
        if (!m_beamPayloadBuffer || m_beamPayloadBufferSize < totalBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius beam payloads";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = totalBytes;
            m_beamPayloadBuffer = device.CreateBuffer(&descriptor);
            m_beamPayloadBufferSize = totalBytes;
        }

        auto const offsetTableBytes = offsetTable.size() * sizeof(std::uint32_t);
        if (!m_beamOffsetTableBuffer || m_beamOffsetTableBuffer.GetSize() < offsetTableBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius beam offset table";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = offsetTableBytes;
            m_beamOffsetTableBuffer = device.CreateBuffer(&descriptor);
        }
        queue.WriteBuffer(m_beamOffsetTableBuffer,
                          0u,
                          offsetTable.data(),
                          offsetTableBytes);

        std::size_t beamOffset = 0u;
        for (auto const & payload : beamPayloadTable)
        {
            if (payload.empty())
            {
                continue;
            }
            auto const bytes = payload.size() * sizeof(float);
            queue.WriteBuffer(m_beamPayloadBuffer, beamOffset, payload.data(), bytes);
            beamOffset += bytes;
        }
    }

    void WebGPUFrameBufferSet::setImagePayloads(
      wgpu::Device const & device,
      wgpu::Queue const & queue,
            std::vector<std::vector<std::uint32_t>> const & imagePayloadTable)
    {
        std::vector<std::uint32_t> offsetTable(imagePayloadTable.size() * 2u, 0u);
                std::size_t totalWords = 0u;
        for (std::size_t slot = 0u; slot < imagePayloadTable.size(); ++slot)
        {
            auto const & payload = imagePayloadTable[slot];
            if (payload.empty())
            {
                continue;
            }
            if (totalWords > std::numeric_limits<std::uint32_t>::max() ||
                payload.size() > std::numeric_limits<std::uint32_t>::max() - totalWords)
            {
                throw std::length_error("WebGPU image payload table exceeds 32-bit addressing");
            }
            offsetTable[slot * 2u] = static_cast<std::uint32_t>(totalWords);
            offsetTable[slot * 2u + 1u] = static_cast<std::uint32_t>(payload.size());
            totalWords += payload.size();
        }

        if (totalWords == 0u)
        {
            m_imagePayloadBuffer = nullptr;
            m_imageOffsetTableBuffer = nullptr;
            return;
        }

        auto const totalBytes = totalWords * sizeof(std::uint32_t);
        if (!m_imagePayloadBuffer || m_imagePayloadBufferSize < totalBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius image payloads";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = totalBytes;
            m_imagePayloadBuffer = device.CreateBuffer(&descriptor);
            m_imagePayloadBufferSize = totalBytes;
        }

        auto const offsetTableBytes = offsetTable.size() * sizeof(std::uint32_t);
        if (!m_imageOffsetTableBuffer || m_imageOffsetTableBuffer.GetSize() < offsetTableBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius image offset table";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = offsetTableBytes;
            m_imageOffsetTableBuffer = device.CreateBuffer(&descriptor);
        }
        queue.WriteBuffer(m_imageOffsetTableBuffer,
                          0u,
                          offsetTable.data(),
                          offsetTableBytes);

        std::size_t imageOffset = 0u;
        for (auto const & payload : imagePayloadTable)
        {
            if (payload.empty())
            {
                continue;
            }
            auto const bytes = payload.size() * sizeof(std::uint32_t);
            queue.WriteBuffer(m_imagePayloadBuffer, imageOffset, payload.data(), bytes);
            imageOffset += bytes;
        }
    }

    void WebGPUFrameBufferSet::ensureMetricsBuffers(wgpu::Device const & device)
    {
        if (m_metricsBuffer && m_metricsStagingBuffer)
        {
            return;
        }

        wgpu::BufferDescriptor metricsDescriptor;
        metricsDescriptor.label = "Gladius frame metrics";
        // CopyDst is required so the per-submission counter zeroing via queue.WriteBuffer
        // passes validation; without it the counters silently accumulate across frames.
        metricsDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc
                                  | wgpu::BufferUsage::CopyDst;
        metricsDescriptor.size = FRAME_METRICS_SIZE_BYTES;
        m_metricsBuffer = device.CreateBuffer(&metricsDescriptor);

        wgpu::BufferDescriptor stagingDescriptor;
        stagingDescriptor.label = "Gladius frame metrics readback";
        stagingDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        stagingDescriptor.size = FRAME_METRICS_SIZE_BYTES;
        m_metricsStagingBuffer = device.CreateBuffer(&stagingDescriptor);

        if (!m_metricsBuffer || !m_metricsStagingBuffer)
        {
            m_metricsBuffer = nullptr;
            m_metricsStagingBuffer = nullptr;
            throw std::runtime_error("Unable to allocate WebGPU frame metrics buffers");
        }
    }

    void WebGPUFrameBufferSet::ensureTimestampBuffers(wgpu::Device const & device)
    {
        if (m_querySet && m_timestampResolveBuffer && m_timestampStagingBuffer)
        {
            return;
        }

        wgpu::QuerySetDescriptor queryDescriptor;
        queryDescriptor.label = "Gladius frame timestamps";
        queryDescriptor.type = wgpu::QueryType::Timestamp;
        queryDescriptor.count = 2u;
        m_querySet = device.CreateQuerySet(&queryDescriptor);

        wgpu::BufferDescriptor resolveDescriptor;
        resolveDescriptor.label = "Gladius frame timestamp resolve";
        resolveDescriptor.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
        resolveDescriptor.size = 2u * sizeof(std::uint64_t);
        m_timestampResolveBuffer = device.CreateBuffer(&resolveDescriptor);

        wgpu::BufferDescriptor stagingDescriptor;
        stagingDescriptor.label = "Gladius frame timestamp readback";
        stagingDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        stagingDescriptor.size = 2u * sizeof(std::uint64_t);
        m_timestampStagingBuffer = device.CreateBuffer(&stagingDescriptor);

        if (!m_querySet || !m_timestampResolveBuffer || !m_timestampStagingBuffer)
        {
            m_querySet = nullptr;
            m_timestampResolveBuffer = nullptr;
            m_timestampStagingBuffer = nullptr;
            throw std::runtime_error("Unable to allocate WebGPU frame timestamp buffers");
        }
    }

    void WebGPUFrameBufferSet::ensurePrecomputedSdfResources(wgpu::Device const & device,
                                                           wgpu::Queue const & queue,
                                                           std::uint32_t const gridSize)
    {
        auto const targetGridSize = std::max(gridSize, 1u);
        if (!m_precomputedSdfUniformBuffer)
        {
            wgpu::BufferDescriptor uniformDescriptor;
            uniformDescriptor.label = "Gladius precomputed SDF uniforms";
            uniformDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            uniformDescriptor.size = sizeof(PrecomputedSdfUniforms);
            m_precomputedSdfUniformBuffer = device.CreateBuffer(&uniformDescriptor);
            if (!m_precomputedSdfUniformBuffer)
            {
                throw std::runtime_error("Unable to allocate WebGPU precomputed SDF uniform buffer");
            }
            PrecomputedSdfUniforms const emptyUniforms{};
            queue.WriteBuffer(
              m_precomputedSdfUniformBuffer, 0u, &emptyUniforms, sizeof(emptyUniforms));
        }

        if (!m_precomputedSdfTexture || m_precomputedSdfGridSize < targetGridSize)
        {
            wgpu::TextureDescriptor textureDescriptor;
            textureDescriptor.label = "Gladius precomputed SDF 3D";
            textureDescriptor.dimension = wgpu::TextureDimension::e3D;
            textureDescriptor.size = {
              .width = targetGridSize, .height = targetGridSize, .depthOrArrayLayers = targetGridSize};
            textureDescriptor.format = wgpu::TextureFormat::R32Float;
            textureDescriptor.mipLevelCount = 1u;
            textureDescriptor.sampleCount = 1u;
            textureDescriptor.usage =
              wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::StorageBinding;
            m_precomputedSdfTexture = device.CreateTexture(&textureDescriptor);
            m_precomputedSdfTextureView =
              m_precomputedSdfTexture ? m_precomputedSdfTexture.CreateView() : wgpu::TextureView{};
            if (!m_precomputedSdfTexture || !m_precomputedSdfTextureView)
            {
                m_precomputedSdfTexture = nullptr;
                m_precomputedSdfTextureView = nullptr;
                m_precomputedSdfGridSize = 0u;
                throw std::runtime_error("Unable to allocate WebGPU precomputed SDF 3D texture");
            }
            m_precomputedSdfGridSize = targetGridSize;
            invalidatePrecomputedSdf(queue);
        }
    }

    void WebGPUFrameBufferSet::writePrecomputedSdfUniforms(
      wgpu::Queue const & queue,
      PrecomputedSdfUniforms const & uniforms) const
    {
        if (!m_precomputedSdfUniformBuffer)
        {
            return;
        }
        queue.WriteBuffer(m_precomputedSdfUniformBuffer, 0u, &uniforms, sizeof(uniforms));
    }

    void WebGPUFrameBufferSet::invalidatePrecomputedSdf(wgpu::Queue const & queue)
    {
        if (!m_precomputedSdfUniformBuffer)
        {
            return;
        }
        PrecomputedSdfUniforms const emptyUniforms{};
        queue.WriteBuffer(m_precomputedSdfUniformBuffer, 0u, &emptyUniforms, sizeof(emptyUniforms));
    }

    void WebGPUFrameBufferSet::reset()
    {
        m_precomputedSdfTextureView = nullptr;
        m_precomputedSdfTexture = nullptr;
        m_precomputedSdfUniformBuffer = nullptr;
        m_precomputedSdfGridSize = 0u;
        m_metricsBuffer = nullptr;
        m_metricsStagingBuffer = nullptr;
        m_querySet = nullptr;
        m_timestampResolveBuffer = nullptr;
        m_timestampStagingBuffer = nullptr;
        m_stagingBuffer = nullptr;
        m_parameterBuffer = nullptr;
        m_outputBuffer = nullptr;
        m_uniformBuffer = nullptr;
        m_meshPayloadBuffer = nullptr;
        m_meshOffsetTableBuffer = nullptr;
        m_meshPayloadBufferSize = 0u;
        m_beamPayloadBuffer = nullptr;
        m_beamOffsetTableBuffer = nullptr;
        m_beamPayloadBufferSize = 0u;
        m_imagePayloadBuffer = nullptr;
        m_imageOffsetTableBuffer = nullptr;
        m_imagePayloadBufferSize = 0u;
        m_width = 0u;
        m_height = 0u;
        m_outputSizeBytes = 0u;
        m_outputCapacitySizeBytes = 0u;
        m_parameterSizeBytes = 0u;
        m_parameterCapacitySizeBytes = 0u;
        m_uploadedParameterSceneId.reset();
        m_uploadedParameterGeneration.reset();
        m_uploadedParameterCount = 0u;
        m_cachedBindGroupKey.reset();
        m_cachedBindGroup = nullptr;
    }

    bool WebGPUFrameBufferSet::isValid() const noexcept
    {
        return m_uniformBuffer && m_outputBuffer && m_stagingBuffer && m_parameterBuffer;
    }

    std::size_t WebGPUFrameBufferSet::getOutputSizeBytes() const noexcept { return m_outputSizeBytes; }
    std::size_t WebGPUFrameBufferSet::getOutputCapacitySizeBytes() const noexcept
    {
        return m_outputCapacitySizeBytes;
    }
    std::size_t WebGPUFrameBufferSet::getParameterSizeBytes() const noexcept { return m_parameterSizeBytes; }
    std::size_t WebGPUFrameBufferSet::getParameterCapacitySizeBytes() const noexcept
    {
        return m_parameterCapacitySizeBytes;
    }
    wgpu::Buffer const & WebGPUFrameBufferSet::getUniformBuffer() const noexcept { return m_uniformBuffer; }
    wgpu::Buffer const & WebGPUFrameBufferSet::getOutputBuffer() const noexcept { return m_outputBuffer; }
    wgpu::Buffer const & WebGPUFrameBufferSet::getStagingBuffer() const noexcept { return m_stagingBuffer; }
    wgpu::Buffer const & WebGPUFrameBufferSet::getParameterBuffer() const noexcept { return m_parameterBuffer; }
}