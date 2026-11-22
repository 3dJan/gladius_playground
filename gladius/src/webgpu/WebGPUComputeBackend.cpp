#include "webgpu/WebGPUComputeBackend.h"

#include "webgpu/WebGPUBufferSet.h"
#include "webgpu/WebGPUFrameBufferSet.h"
#include "webgpu/WebGPUFrameShaderComposer.h"
#include "webgpu/WebGPUSliceShaderComposer.h"

#include <cmrc/cmrc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif
#include <utility>
#include <vector>

CMRC_DECLARE(gladius_resources);

namespace gladius::webgpu
{
    namespace
    {
        std::string toString(wgpu::StringView value);

        [[nodiscard]] std::uint64_t hashWgslSource(std::string_view const source) noexcept
        {
            std::uint64_t hash = 14695981039346656037ull;
            for (unsigned char const byte : source)
            {
                hash ^= static_cast<std::uint64_t>(byte);
                hash *= 1099511628211ull;
            }
            return hash == 0u ? 1u : hash;
        }

        [[nodiscard]] std::string applyHardwareFilteredPrecomputedSdf(std::string const & source)
        {
            constexpr std::string_view beginMarker = "// GLADIUS_PRECOMPUTED_SDF_SAMPLE_BEGIN";
            constexpr std::string_view endMarker = "// GLADIUS_PRECOMPUTED_SDF_SAMPLE_END";
            auto const beginPos = source.find(beginMarker);
            if (beginPos == std::string::npos)
            {
                return source;
            }
            auto const endPos = source.find(endMarker, beginPos + beginMarker.size());
            if (endPos == std::string::npos)
            {
                return source;
            }

            constexpr std::string_view hardwareSamplerImpl = R"wgsl(
@group(0) @binding(13)
var precomputed_sdf_sampler: sampler;

fn sample_precomputed_sdf(position: vec3<f32>) -> f32 {
    let bbox_min = precomputed_sdf_info.bbox_min_and_tolerance.xyz;
    let bbox_max = precomputed_sdf_info.bbox_max_and_valid.xyz;
    let dims = vec3<f32>(precomputed_sdf_info.grid_dims.xyz);
    let span = max(bbox_max - bbox_min, vec3<f32>(1.0e-5f));
    let uvw_norm = clamp((position - bbox_min) / span, vec3<f32>(0.0f), vec3<f32>(1.0f));
    let uvw = (uvw_norm * (dims - vec3<f32>(1.0f)) + vec3<f32>(0.5f)) / dims;
    return textureSampleLevel(precomputed_sdf, precomputed_sdf_sampler, uvw, 0.0f).r;
}
)wgsl";
            std::string rewritten;
            rewritten.reserve(source.size());
            rewritten.append(source, 0u, beginPos);
            rewritten.append(hardwareSamplerImpl);
            rewritten.append(source, endPos + endMarker.size(), std::string::npos);
            return rewritten;
        }
    }

    class WebGPUFramePipelineCache
    {
      public:
        struct Pipeline
        {
            enum class Status
            {
                Pending,
                Ready,
                Failed,
            };

            struct Handles
            {
                wgpu::BindGroupLayout bindGroupLayout;
                wgpu::ComputePipeline computePipeline;
            };

            void setBindGroupLayout(wgpu::BindGroupLayout layout)
            {
                std::scoped_lock lock{m_mutex};
                m_bindGroupLayout = std::move(layout);
            }

            void complete(wgpu::CreatePipelineAsyncStatus status,
                          wgpu::ComputePipeline pipeline,
                          wgpu::StringView message)
            {
                std::scoped_lock lock{m_mutex};
                if (status == wgpu::CreatePipelineAsyncStatus::Success && pipeline)
                {
                    m_computePipeline = std::move(pipeline);
                    m_status = Status::Ready;
                    return;
                }

                m_errorMessage = "WebGPU frame pipeline creation failed";
                auto const details = toString(message);
                if (!details.empty())
                {
                    m_errorMessage += ": ";
                    m_errorMessage += details;
                }
                m_status = Status::Failed;
            }

            [[nodiscard]] Status getStatus() const
            {
                std::scoped_lock lock{m_mutex};
                return m_status;
            }

            [[nodiscard]] std::optional<Handles> getReadyHandles() const
            {
                std::scoped_lock lock{m_mutex};
                if (m_status != Status::Ready)
                {
                    return std::nullopt;
                }
                return Handles{.bindGroupLayout = m_bindGroupLayout,
                               .computePipeline = m_computePipeline};
            }

            [[nodiscard]] wgpu::BindGroupLayout getBindGroupLayout() const
            {
                std::scoped_lock lock{m_mutex};
                return m_bindGroupLayout;
            }

            [[nodiscard]] std::string getErrorMessage() const
            {
                std::scoped_lock lock{m_mutex};
                return m_errorMessage;
            }

          private:
            mutable std::mutex m_mutex;
            wgpu::BindGroupLayout m_bindGroupLayout;
            wgpu::ComputePipeline m_computePipeline;
            Status m_status{Status::Pending};
            std::string m_errorMessage;
        };

        struct Result
        {
            std::shared_ptr<Pipeline> pipeline;
        };

        [[nodiscard]] Result getOrCreate(wgpu::Device const & device,
                                         std::string const & shaderSource,
                                         std::uint64_t shaderHash,
                                         bool const hasMeshPayloads,
                                         bool const hasBeamPayloads,
                                         bool const hasImagePayloads,
                                         bool const useHardwareFilteredSdf)
        {
            if (shaderHash == 0u)
            {
                shaderHash = hashWgslSource(shaderSource);
            }
            std::scoped_lock lock{m_mutex};
            auto const cached = std::find_if(
              m_entries.begin(),
              m_entries.end(),
              [&](Entry const & entry)
              {
                  return entry.shaderHash == shaderHash &&
                         entry.hasMeshPayloads == hasMeshPayloads &&
                         entry.hasBeamPayloads == hasBeamPayloads &&
                         entry.hasImagePayloads == hasImagePayloads &&
                         entry.useHardwareFilteredSdf == useHardwareFilteredSdf &&
                         entry.shaderSource.size() == shaderSource.size() &&
                         entry.shaderSource == shaderSource;
              });
            if (cached != m_entries.end())
            {
                if (cached->pipeline->getStatus() != Pipeline::Status::Failed)
                {
                    return Result{.pipeline = cached->pipeline};
                }
                m_entries.erase(cached);
            }

            std::string const effectiveShader =
              useHardwareFilteredSdf ? applyHardwareFilteredPrecomputedSdf(shaderSource) : shaderSource;

            wgpu::ShaderSourceWGSL wgsl;
            wgsl.code = {effectiveShader.data(), effectiveShader.size()};
            wgpu::ShaderModuleDescriptor shaderDescriptor;
            shaderDescriptor.nextInChain = &wgsl;
            auto const shader = device.CreateShaderModule(&shaderDescriptor);

            wgpu::BindGroupLayoutEntry bindings[13]{};
            bindings[0].binding = 0u;
            bindings[0].visibility = wgpu::ShaderStage::Compute;
            bindings[0].buffer.type = wgpu::BufferBindingType::Uniform;
            bindings[0].buffer.minBindingSize = sizeof(FrameUniforms);
            bindings[1].binding = 1u;
            bindings[1].visibility = wgpu::ShaderStage::Compute;
            bindings[1].buffer.type = wgpu::BufferBindingType::Storage;
            bindings[1].buffer.minBindingSize = sizeof(std::uint32_t);
            bindings[2].binding = 2u;
            bindings[2].visibility = wgpu::ShaderStage::Compute;
            bindings[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
            bindings[2].buffer.minBindingSize = sizeof(float);
            std::size_t bindingCount = 3u;
            if (hasMeshPayloads)
            {
                bindings[bindingCount].binding = 4u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = sizeof(float);
                ++bindingCount;
                bindings[bindingCount].binding = 5u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
                ++bindingCount;
            }
            if (hasBeamPayloads)
            {
                bindings[bindingCount].binding = 6u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = sizeof(float);
                ++bindingCount;
                bindings[bindingCount].binding = 7u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
                ++bindingCount;
            }
            if (hasImagePayloads)
            {
                bindings[bindingCount].binding = 8u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = sizeof(float);
                ++bindingCount;
                bindings[bindingCount].binding = 9u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
                ++bindingCount;
            }
            // The metric counter buffer is always bound (binding 10); the shader gates the
            // atomic writes behind the RF_DEBUG_METRICS rendering flag.
            bindings[bindingCount].binding = 10u;
            bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
            bindings[bindingCount].buffer.type = wgpu::BufferBindingType::Storage;
            bindings[bindingCount].buffer.minBindingSize = FRAME_METRICS_SIZE_BYTES;
            ++bindingCount;

            bindings[bindingCount].binding = 11u;
            bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
            bindings[bindingCount].buffer.type = wgpu::BufferBindingType::Uniform;
            bindings[bindingCount].buffer.minBindingSize = sizeof(PrecomputedSdfUniforms);
            ++bindingCount;

            bindings[bindingCount].binding = 12u;
            bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
            bindings[bindingCount].texture.sampleType =
              useHardwareFilteredSdf ? wgpu::TextureSampleType::Float
                                     : wgpu::TextureSampleType::UnfilterableFloat;
            bindings[bindingCount].texture.viewDimension = wgpu::TextureViewDimension::e3D;
            ++bindingCount;

            if (useHardwareFilteredSdf)
            {
                bindings[bindingCount].binding = 13u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].sampler.type = wgpu::SamplerBindingType::Filtering;
                ++bindingCount;
            }

            wgpu::BindGroupLayoutDescriptor layoutDescriptor;
            layoutDescriptor.entryCount = bindingCount;
            layoutDescriptor.entries = bindings;
            auto const bindGroupLayout = device.CreateBindGroupLayout(&layoutDescriptor);

            wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor;
            pipelineLayoutDescriptor.bindGroupLayoutCount = 1u;
            pipelineLayoutDescriptor.bindGroupLayouts = &bindGroupLayout;
            auto const pipelineLayout = device.CreatePipelineLayout(&pipelineLayoutDescriptor);

            wgpu::ComputePipelineDescriptor pipelineDescriptor;
            pipelineDescriptor.layout = pipelineLayout;
            pipelineDescriptor.compute.module = shader;
            pipelineDescriptor.compute.entryPoint = "main";
            auto pipeline = std::make_shared<Pipeline>();
                        pipeline->setBindGroupLayout(bindGroupLayout);
#ifdef __EMSCRIPTEN__
                        auto const callbackMode = wgpu::CallbackMode::AllowSpontaneous;
#else
                        auto const callbackMode = wgpu::CallbackMode::AllowProcessEvents;
#endif
                        auto const pipelineFuture = device.CreateComputePipelineAsync(
                            &pipelineDescriptor,
                            callbackMode,
                            [pipeline](wgpu::CreatePipelineAsyncStatus const status,
                                                 wgpu::ComputePipeline computePipeline,
                                                 wgpu::StringView const message)
                            { pipeline->complete(status, std::move(computePipeline), message); });
                        (void) pipelineFuture;

            constexpr std::size_t maxCachedPipelineEntries = 8u;
            if (m_entries.size() == maxCachedPipelineEntries)
            {
                m_entries.erase(m_entries.begin());
            }
            m_entries.push_back(Entry{.shaderHash = shaderHash,
                                      .shaderSource = shaderSource,
                                      .hasMeshPayloads = hasMeshPayloads,
                                      .hasBeamPayloads = hasBeamPayloads,
                                      .hasImagePayloads = hasImagePayloads,
                                      .useHardwareFilteredSdf = useHardwareFilteredSdf,
                                      .pipeline = pipeline});
            return Result{.pipeline = m_entries.back().pipeline};
        }

      private:
        struct Entry
        {
            std::uint64_t shaderHash{0u};
            std::string shaderSource;
            bool hasMeshPayloads{false};
            bool hasBeamPayloads{false};
            bool hasImagePayloads{false};
            bool useHardwareFilteredSdf{false};
            std::shared_ptr<Pipeline> pipeline;
        };

        std::mutex m_mutex;
        std::vector<Entry> m_entries;
    };

    class WebGPUPrebakePipelineCache
    {
      public:
        [[nodiscard]] std::shared_ptr<WebGPUFramePipelineCache::Pipeline>
        getOrCreate(wgpu::Device const & device,
                    std::string const & shaderSource,
                    std::uint64_t shaderHash,
                    bool const hasMeshPayloads,
                    bool const hasBeamPayloads,
                    bool const hasImagePayloads)
        {
            if (shaderHash == 0u)
            {
                shaderHash = hashWgslSource(shaderSource);
            }
            std::scoped_lock lock{m_mutex};
            auto const cached = std::find_if(
              m_entries.begin(),
              m_entries.end(),
              [&](Entry const & entry)
              {
                  return entry.shaderHash == shaderHash &&
                         entry.hasMeshPayloads == hasMeshPayloads &&
                         entry.hasBeamPayloads == hasBeamPayloads &&
                         entry.hasImagePayloads == hasImagePayloads &&
                         entry.shaderSource.size() == shaderSource.size() &&
                         entry.shaderSource == shaderSource;
              });
            if (cached != m_entries.end())
            {
                if (cached->pipeline->getStatus() !=
                    WebGPUFramePipelineCache::Pipeline::Status::Failed)
                {
                    return cached->pipeline;
                }
                m_entries.erase(cached);
            }

            wgpu::ShaderSourceWGSL wgsl;
            wgsl.code = {shaderSource.data(), shaderSource.size()};
            wgpu::ShaderModuleDescriptor shaderDescriptor;
            shaderDescriptor.nextInChain = &wgsl;
            auto const shader = device.CreateShaderModule(&shaderDescriptor);

            wgpu::BindGroupLayoutEntry bindings[9]{};
            bindings[0].binding = 0u;
            bindings[0].visibility = wgpu::ShaderStage::Compute;
            bindings[0].buffer.type = wgpu::BufferBindingType::Uniform;
            bindings[0].buffer.minBindingSize = sizeof(PrecomputedSdfUniforms);
            bindings[1].binding = 1u;
            bindings[1].visibility = wgpu::ShaderStage::Compute;
            bindings[1].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
            bindings[1].storageTexture.format = wgpu::TextureFormat::R32Float;
            bindings[1].storageTexture.viewDimension = wgpu::TextureViewDimension::e3D;
            bindings[2].binding = 2u;
            bindings[2].visibility = wgpu::ShaderStage::Compute;
            bindings[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
            bindings[2].buffer.minBindingSize = sizeof(float);
            std::size_t bindingCount = 3u;
            if (hasMeshPayloads)
            {
                bindings[bindingCount].binding = 4u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = sizeof(float);
                ++bindingCount;
                bindings[bindingCount].binding = 5u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
                ++bindingCount;
            }
            if (hasBeamPayloads)
            {
                bindings[bindingCount].binding = 6u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = sizeof(float);
                ++bindingCount;
                bindings[bindingCount].binding = 7u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
                ++bindingCount;
            }
            if (hasImagePayloads)
            {
                bindings[bindingCount].binding = 8u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = sizeof(float);
                ++bindingCount;
                bindings[bindingCount].binding = 9u;
                bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[bindingCount].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
                ++bindingCount;
            }

            wgpu::BindGroupLayoutDescriptor layoutDescriptor;
            layoutDescriptor.entryCount = bindingCount;
            layoutDescriptor.entries = bindings;
            auto const bindGroupLayout = device.CreateBindGroupLayout(&layoutDescriptor);

            wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor;
            pipelineLayoutDescriptor.bindGroupLayoutCount = 1u;
            pipelineLayoutDescriptor.bindGroupLayouts = &bindGroupLayout;
            auto const pipelineLayout = device.CreatePipelineLayout(&pipelineLayoutDescriptor);

            wgpu::ComputePipelineDescriptor pipelineDescriptor;
            pipelineDescriptor.layout = pipelineLayout;
            pipelineDescriptor.compute.module = shader;
            pipelineDescriptor.compute.entryPoint = "main";

            auto pipeline = std::make_shared<WebGPUFramePipelineCache::Pipeline>();
            pipeline->setBindGroupLayout(bindGroupLayout);
#ifdef __EMSCRIPTEN__
            auto const callbackMode = wgpu::CallbackMode::AllowSpontaneous;
#else
            auto const callbackMode = wgpu::CallbackMode::AllowProcessEvents;
#endif
            (void) device.CreateComputePipelineAsync(
              &pipelineDescriptor,
              callbackMode,
              [pipeline](wgpu::CreatePipelineAsyncStatus const status,
                         wgpu::ComputePipeline computePipeline,
                         wgpu::StringView const message)
              { pipeline->complete(status, std::move(computePipeline), message); });

            constexpr std::size_t maxCachedEntries = 4u;
            if (m_entries.size() == maxCachedEntries)
            {
                m_entries.erase(m_entries.begin());
            }
            m_entries.push_back(Entry{.shaderHash = shaderHash,
                                      .shaderSource = shaderSource,
                                      .hasMeshPayloads = hasMeshPayloads,
                                      .hasBeamPayloads = hasBeamPayloads,
                                      .hasImagePayloads = hasImagePayloads,
                                      .pipeline = pipeline});
            return pipeline;
        }

      private:
        struct Entry
        {
            std::uint64_t shaderHash{0u};
            std::string shaderSource;
            bool hasMeshPayloads{false};
            bool hasBeamPayloads{false};
            bool hasImagePayloads{false};
            std::shared_ptr<WebGPUFramePipelineCache::Pipeline> pipeline;
        };

        std::mutex m_mutex;
        std::vector<Entry> m_entries;
    };

    class WebGPUComputeBackend::FrameBufferPool
        : public std::enable_shared_from_this<WebGPUComputeBackend::FrameBufferPool>
    {
      public:
        [[nodiscard]] std::shared_ptr<WebGPUFrameBufferSet> acquire()
        {
            std::shared_ptr<Slot> slot;
            {
                std::scoped_lock lock{m_mutex};
                auto const available = std::find_if(
                  m_slots.begin(), m_slots.end(), [](auto const & candidate)
                  { return !candidate->isLeased; });
                if (available != m_slots.end())
                {
                    slot = *available;
                }
                else
                {
                    slot = std::make_shared<Slot>();
                    slot->isLeased = true;
                    constexpr std::size_t maximumRetainedSlots = 2u;
                    if (m_slots.size() < maximumRetainedSlots)
                    {
                        m_slots.push_back(slot);
                    }
                }
                slot->isLeased = true;
            }

            auto const owner = shared_from_this();
            return std::shared_ptr<WebGPUFrameBufferSet>(
              &slot->buffers,
              [owner, slot = std::move(slot)](WebGPUFrameBufferSet *)
              { owner->release(slot); });
        }

      private:
        struct Slot
        {
            WebGPUFrameBufferSet buffers;
            bool isLeased{false};
        };

        void release(std::shared_ptr<Slot> const & slot) noexcept
        {
            std::scoped_lock lock{m_mutex};
            slot->isLeased = false;
        }

        std::mutex m_mutex;
        std::vector<std::shared_ptr<Slot>> m_slots;
    };

    struct PrecomputedSdfState
    {
        std::mutex mutex;
        wgpu::Buffer uniformBuffer;
        wgpu::Texture texture;
        wgpu::TextureView textureView;
        wgpu::Sampler linearSampler;
        std::uint32_t gridSize{0u};
        std::uint64_t generationEpoch{0u};
        bool valid{false};

        void ensureInitialized(wgpu::Device const & device,
                               wgpu::Queue const & queue,
                               std::uint32_t const targetGridSize,
                               bool const createLinearSampler = false)
        {
            std::scoped_lock lock{mutex};
            auto const clampedSize = std::max(targetGridSize, 1u);
            if (createLinearSampler && !linearSampler)
            {
                wgpu::SamplerDescriptor samplerDesc;
                samplerDesc.label = "Gladius precomputed SDF linear sampler";
                samplerDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
                samplerDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
                samplerDesc.addressModeW = wgpu::AddressMode::ClampToEdge;
                samplerDesc.magFilter = wgpu::FilterMode::Linear;
                samplerDesc.minFilter = wgpu::FilterMode::Linear;
                samplerDesc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
                linearSampler = device.CreateSampler(&samplerDesc);
            }
            if (!uniformBuffer)
            {
                wgpu::BufferDescriptor uniformDescriptor;
                uniformDescriptor.label = "Gladius precomputed SDF uniforms";
                uniformDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
                uniformDescriptor.size = sizeof(PrecomputedSdfUniforms);
                uniformBuffer = device.CreateBuffer(&uniformDescriptor);
                if (!uniformBuffer)
                {
                    throw std::runtime_error("Unable to allocate precomputed SDF uniform buffer");
                }
                PrecomputedSdfUniforms const emptyUniforms{};
                queue.WriteBuffer(uniformBuffer, 0u, &emptyUniforms, sizeof(emptyUniforms));
            }

            if (!texture || gridSize < clampedSize)
            {
                wgpu::TextureDescriptor textureDescriptor;
                textureDescriptor.label = "Gladius precomputed SDF 3D";
                textureDescriptor.dimension = wgpu::TextureDimension::e3D;
                textureDescriptor.size = {.width = clampedSize,
                                          .height = clampedSize,
                                          .depthOrArrayLayers = clampedSize};
                textureDescriptor.format = wgpu::TextureFormat::R32Float;
                textureDescriptor.mipLevelCount = 1u;
                textureDescriptor.sampleCount = 1u;
                textureDescriptor.usage =
                  wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::StorageBinding;
                texture = device.CreateTexture(&textureDescriptor);
                textureView = texture ? texture.CreateView() : wgpu::TextureView{};
                if (!texture || !textureView)
                {
                    texture = nullptr;
                    textureView = nullptr;
                    gridSize = 0u;
                    throw std::runtime_error("Unable to allocate precomputed SDF 3D texture");
                }
                gridSize = clampedSize;
                valid = false;
                PrecomputedSdfUniforms const emptyUniforms{};
                queue.WriteBuffer(uniformBuffer, 0u, &emptyUniforms, sizeof(emptyUniforms));
            }
        }

        void invalidate(wgpu::Queue const & queue)
        {
            std::scoped_lock lock{mutex};
            ++generationEpoch;
            valid = false;
            if (uniformBuffer && queue)
            {
                PrecomputedSdfUniforms const emptyUniforms{};
                queue.WriteBuffer(uniformBuffer, 0u, &emptyUniforms, sizeof(emptyUniforms));
            }
        }
    };

    namespace
    {
        constexpr std::uint32_t MAX_FRAME_DIMENSION = 4096u;
        // Keep the explicit allocation guard, but allow common desktop viewport sizes such as
        // 3685x1855 (approximately 6.8 million pixels).
        constexpr std::size_t MAX_FRAME_PIXELS = 10u * 1024u * 1024u;
        constexpr std::uint32_t MAX_FRAME_RAY_STEPS = 2048u;

        std::string toString(wgpu::StringView const value)
        {
            if (value.data == nullptr)
            {
                return {};
            }

            if (value.length == wgpu::kStrlen)
            {
                return std::string(value.data);
            }

            return std::string(value.data, value.length);
        }

        struct WebGPUSliceSubmissionState
        {
            mutable std::mutex mutex;
            WebGPUBufferSet buffers;
            compute::ComputeCompletionStatus status{compute::ComputeCompletionStatus::Pending};
            std::optional<compute::SliceResult> result;
            std::string errorMessage;
        };

        struct WebGPUFrameSubmissionState
        {
            explicit WebGPUFrameSubmissionState(std::shared_ptr<WebGPUFrameBufferSet> lease)
                : bufferLease(std::move(lease))
                , buffers(*bufferLease)
            {
            }

            mutable std::mutex mutex;
            std::shared_ptr<WebGPUFrameBufferSet> bufferLease;
            WebGPUFrameBufferSet & buffers;
            compute::ComputeCompletionStatus status{compute::ComputeCompletionStatus::Pending};
            bool subbandInFlight{false};
            std::optional<compute::FrameResult> result;
            std::optional<compute::FrameMetrics> metrics;
            /// GPU compute-pass duration in milliseconds from timestamp queries;
            /// gpuTimestampsAvailable is set only when valid timestamps were resolved.
            std::optional<double> gpuPassMs;
            bool gpuTimestampsAvailable{false};
            /// Raw timepoints; FrameTiming is assembled lazily in getFrameTiming().
            std::optional<std::chrono::steady_clock::time_point> submittedAt;
            std::optional<std::chrono::steady_clock::time_point> workDoneAt;
            std::optional<std::chrono::steady_clock::time_point> readbackAt;
            std::string errorMessage;
        };

        class WebGPUSliceSubmission final : public compute::ISliceSubmission
        {
          public:
            WebGPUSliceSubmission(std::shared_ptr<WebGPUComputeContext> context,
                                  compute::SliceRequest request)
                : m_context(std::move(context))
                , m_width(request.width)
                , m_height(request.height)
                , m_state(std::make_shared<WebGPUSliceSubmissionState>())
            {
                try
                {
                    submit(std::move(request));
                }
                catch (std::exception const & exception)
                {
                    std::scoped_lock lock{m_state->mutex};
                    m_state->status = compute::ComputeCompletionStatus::Failed;
                    m_state->errorMessage = exception.what();
                }
            }

            ~WebGPUSliceSubmission() override
            {
#ifndef __EMSCRIPTEN__
                if (getStatus() == compute::ComputeCompletionStatus::Pending)
                {
                    try
                    {
                        wait();
                    }
                    catch (...)
                    {
                    }
                }
#endif
            }

            [[nodiscard]] compute::ComputeCompletionStatus getStatus() const noexcept override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->status;
            }

            void wait() override
            {
#ifdef __EMSCRIPTEN__
                // Browser callbacks are delivered by the event loop. Waiting here would block
                // that loop and prevent the readback callback from ever running.
                return;
#else
                while (getStatus() == compute::ComputeCompletionStatus::Pending)
                {
                    m_context->processEvents();
                    if (!m_context->isValid())
                    {
                        std::scoped_lock lock{m_state->mutex};
                        if (m_state->status == compute::ComputeCompletionStatus::Pending)
                        {
                            m_state->status = compute::ComputeCompletionStatus::Failed;
                            m_state->errorMessage = m_context->getErrorMessage();
                        }
                    }
                }
#endif
            }

            [[nodiscard]] std::optional<compute::SliceResult> takeResult() override
            {
                std::scoped_lock lock{m_state->mutex};
                if (m_state->status != compute::ComputeCompletionStatus::Succeeded)
                {
                    return std::nullopt;
                }

                m_state->status = compute::ComputeCompletionStatus::Failed;
                return std::exchange(m_state->result, std::nullopt);
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->errorMessage;
            }

          private:
            static std::string loadDefaultShader()
            {
                constexpr std::string_view DEFAULT_EVALUATOR = R"(
fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
    let distance = length(position) - 0.5;
    return vec4<f32>(vec3<f32>(1.0), distance);
}
)";
                return WebGPUSliceShaderComposer::compose(DEFAULT_EVALUATOR);
            }

            void submit(compute::SliceRequest request)
            {
                if (!m_context || !m_context->isValid())
                {
                    throw std::runtime_error("WebGPU compute context is unavailable");
                }
                // Serialize all device/queue access: Dawn instance processing and queue
                // submission must not run concurrently from multiple threads.
                WebGPUComputeContext::DeviceLock const deviceLock(*m_context);

                auto const dispatchSize = calculateSliceDispatchSize(request.width, request.height);
                if (!dispatchSize.has_value())
                {
                    throw std::invalid_argument("Invalid WebGPU slice dimensions");
                }

                m_state->buffers.resize(m_context->getDevice(),
                                        request.width,
                                        request.height,
                                        request.parameterValues.size());
                m_state->buffers.writeUniforms(m_context->getQueue(),
                                               SliceUniforms{.sliceZ = request.sliceZ,
                                                             .width = request.width,
                                                             .height = request.height,
                                                             .scale = request.scale});
                m_state->buffers.writeParameters(m_context->getQueue(), request.parameterValues);

                if (request.shaderSource.empty())
                {
                    request.shaderSource = loadDefaultShader();
                }

                wgpu::ShaderSourceWGSL wgsl;
                wgsl.code = {request.shaderSource.data(), request.shaderSource.size()};
                wgpu::ShaderModuleDescriptor shaderDescriptor;
                shaderDescriptor.nextInChain = &wgsl;
                auto const shader = m_context->getDevice().CreateShaderModule(&shaderDescriptor);

                wgpu::BindGroupLayoutEntry bindings[3]{};
                bindings[0].binding = 0u;
                bindings[0].visibility = wgpu::ShaderStage::Compute;
                bindings[0].buffer.type = wgpu::BufferBindingType::Uniform;
                bindings[0].buffer.minBindingSize = sizeof(SliceUniforms);
                bindings[1].binding = 1u;
                bindings[1].visibility = wgpu::ShaderStage::Compute;
                bindings[1].buffer.type = wgpu::BufferBindingType::Storage;
                bindings[1].buffer.minBindingSize = m_state->buffers.getOutputSizeBytes();
                bindings[2].binding = 2u;
                bindings[2].visibility = wgpu::ShaderStage::Compute;
                bindings[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[2].buffer.minBindingSize = sizeof(float);

                wgpu::BindGroupLayoutDescriptor layoutDescriptor;
                layoutDescriptor.entryCount = std::size(bindings);
                layoutDescriptor.entries = bindings;
                auto const bindGroupLayout =
                  m_context->getDevice().CreateBindGroupLayout(&layoutDescriptor);

                wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor;
                pipelineLayoutDescriptor.bindGroupLayoutCount = 1u;
                pipelineLayoutDescriptor.bindGroupLayouts = &bindGroupLayout;
                auto const pipelineLayout =
                  m_context->getDevice().CreatePipelineLayout(&pipelineLayoutDescriptor);

                wgpu::ComputePipelineDescriptor pipelineDescriptor;
                pipelineDescriptor.layout = pipelineLayout;
                pipelineDescriptor.compute.module = shader;
                pipelineDescriptor.compute.entryPoint = "main";
                auto const pipeline =
                  m_context->getDevice().CreateComputePipeline(&pipelineDescriptor);

                wgpu::BindGroupEntry bindGroupEntries[3]{};
                bindGroupEntries[0].binding = 0u;
                bindGroupEntries[0].buffer = m_state->buffers.getUniformBuffer();
                bindGroupEntries[0].size = sizeof(SliceUniforms);
                bindGroupEntries[1].binding = 1u;
                bindGroupEntries[1].buffer = m_state->buffers.getOutputBuffer();
                bindGroupEntries[1].size = m_state->buffers.getOutputSizeBytes();
                bindGroupEntries[2].binding = 2u;
                bindGroupEntries[2].buffer = m_state->buffers.getParameterBuffer();
                bindGroupEntries[2].size = m_state->buffers.getParameterSizeBytes();

                wgpu::BindGroupDescriptor bindGroupDescriptor;
                bindGroupDescriptor.layout = bindGroupLayout;
                bindGroupDescriptor.entryCount = std::size(bindGroupEntries);
                bindGroupDescriptor.entries = bindGroupEntries;
                auto const bindGroup = m_context->getDevice().CreateBindGroup(&bindGroupDescriptor);

                auto const encoder = m_context->getDevice().CreateCommandEncoder();
                auto const computePass = encoder.BeginComputePass();
                computePass.SetPipeline(pipeline);
                computePass.SetBindGroup(0u, bindGroup);
                computePass.DispatchWorkgroups(dispatchSize->workgroupsX,
                                               dispatchSize->workgroupsY);
                computePass.End();
                encoder.CopyBufferToBuffer(m_state->buffers.getOutputBuffer(),
                                           0u,
                                           m_state->buffers.getStagingBuffer(),
                                           0u,
                                           m_state->buffers.getOutputSizeBytes());
                auto const commandBuffer = encoder.Finish();
                m_context->getQueue().Submit(1u, &commandBuffer);

                                auto const state = m_state;
                                state->buffers.getStagingBuffer().MapAsync(
                  wgpu::MapMode::Read,
                  0u,
                                    state->buffers.getOutputSizeBytes(),
#ifdef __EMSCRIPTEN__
                                    wgpu::CallbackMode::AllowSpontaneous,
#else
                                    wgpu::CallbackMode::AllowProcessEvents,
#endif
                                    [state, width = m_width, height = m_height](wgpu::MapAsyncStatus const status,
                                                                                                                             wgpu::StringView const message)
                  {
                                            std::scoped_lock lock{state->mutex};
                      if (status != wgpu::MapAsyncStatus::Success)
                      {
                                                    state->status = compute::ComputeCompletionStatus::Failed;
                                                    state->errorMessage = "WebGPU slice readback failed: " + toString(message);
                          return;
                      }

                      auto const * mappedPixels = static_cast<std::uint32_t const *>(
                                                state->buffers.getStagingBuffer().GetConstMappedRange(
                                                    0u, state->buffers.getOutputSizeBytes()));
                      if (mappedPixels == nullptr)
                      {
                                                    state->status = compute::ComputeCompletionStatus::Failed;
                                                    state->errorMessage = "WebGPU slice staging buffer returned no mapped data";
                          return;
                      }

                      auto const pixelCount =
                                                state->buffers.getOutputSizeBytes() / sizeof(std::uint32_t);
                                            state->result = compute::SliceResult{.width = width,
                                                                                                                     .height = height,
                                                                                                                     .pixels = {mappedPixels,
                                                                                                                                            mappedPixels + pixelCount}};
                                            state->buffers.getStagingBuffer().Unmap();
                                            state->status = compute::ComputeCompletionStatus::Succeeded;
                  });
            }

            std::shared_ptr<WebGPUComputeContext> m_context;
            std::uint32_t m_width{};
            std::uint32_t m_height{};
            std::shared_ptr<WebGPUSliceSubmissionState> m_state;
        };

        class WebGPUFrameSubmission final : public compute::IFrameSubmission
        {
          public:
            WebGPUFrameSubmission(std::shared_ptr<WebGPUComputeContext> context,
                                  compute::FrameRequest request,
                                  WebGPUFrameSubmissionOptions options,
                                  std::shared_ptr<WebGPUFrameBufferSet> bufferLease,
                                  std::shared_ptr<WebGPUFramePipelineCache> pipelineCache,
                                  std::shared_ptr<PrecomputedSdfState> precomputedSdf)
                : m_context(std::move(context))
                , m_width(request.width)
                , m_height(request.height)
                , m_pipelineCache(std::move(pipelineCache))
                , m_precomputedSdf(std::move(precomputedSdf))
                , m_scenePayloads(std::move(options.scenePayloads))
                , m_state(std::make_shared<WebGPUFrameSubmissionState>(std::move(bufferLease)))
            {
                try
                {
                    submit(std::move(request), std::move(options));
                }
                catch (std::exception const & exception)
                {
                    std::scoped_lock lock{m_state->mutex};
                    m_state->status = compute::ComputeCompletionStatus::Failed;
                    m_state->errorMessage = exception.what();
                }
            }

            ~WebGPUFrameSubmission() override
            {
#ifndef __EMSCRIPTEN__
                if (getStatus() == compute::ComputeCompletionStatus::Pending)
                {
                    try
                    {
                        wait();
                    }
                    catch (...)
                    {
                    }
                }
#endif
            }

            [[nodiscard]] compute::ComputeCompletionStatus getStatus() const noexcept override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->status;
            }

            void progress() noexcept override
            {
                if (getStatus() != compute::ComputeCompletionStatus::Pending || !m_context)
                {
                    return;
                }

                try
                {
#ifndef __EMSCRIPTEN__
                    m_context->processEvents();
#endif
                    if (!m_context->isValid())
                    {
                        std::scoped_lock lock{m_state->mutex};
                        if (m_state->status == compute::ComputeCompletionStatus::Pending)
                        {
                            m_state->status = compute::ComputeCompletionStatus::Failed;
                            m_state->errorMessage = m_context->getErrorMessage();
                        }
                        return;
                    }
                    if (!m_frameSubmitted || (m_skipCpuReadback && m_nextSubbandRow < m_rowCount))
                    {
                        WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
                        submitFrameWithPipeline();
                    }
                }
                catch (std::exception const & error)
                {
                    std::scoped_lock lock{m_state->mutex};
                    m_state->status = compute::ComputeCompletionStatus::Failed;
                    m_state->errorMessage = error.what();
                }
                catch (...)
                {
                    std::scoped_lock lock{m_state->mutex};
                    m_state->status = compute::ComputeCompletionStatus::Failed;
                    m_state->errorMessage = "WebGPU event processing failed";
                }
            }

            void wait() override
            {
#ifdef __EMSCRIPTEN__
                // Browser callbacks are delivered by the event loop. Waiting here would block
                // that loop and prevent the readback callback from ever running.
                return;
#else
                while (getStatus() == compute::ComputeCompletionStatus::Pending)
                {
                    progress();
                }
#endif
            }

            [[nodiscard]] std::optional<compute::FrameResult> takeResult() override
            {
                std::scoped_lock lock{m_state->mutex};
                if (m_state->status != compute::ComputeCompletionStatus::Succeeded)
                {
                    return std::nullopt;
                }

                m_state->status = compute::ComputeCompletionStatus::Failed;
                return std::exchange(m_state->result, std::nullopt);
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->errorMessage;
            }

            [[nodiscard]] std::optional<compute::FrameTiming> getFrameTiming() const override
            {
                std::scoped_lock lock{m_state->mutex};
                if (!m_state->submittedAt.has_value() || !m_state->readbackAt.has_value())
                {
                    return std::nullopt;
                }
                compute::FrameTiming timing;
                if (m_state->gpuTimestampsAvailable && m_state->gpuPassMs.has_value())
                {
                    timing.gpuPassMs = *m_state->gpuPassMs;
                    timing.gpuTimestampsAvailable = true;
                }
                auto const readback = *m_state->readbackAt;
                if (m_state->workDoneAt.has_value())
                {
                    timing.submitToWorkDoneMs =
                      std::chrono::duration<double, std::milli>(*m_state->workDoneAt - *m_state->submittedAt).count();
                    timing.workDoneToReadbackMs =
                      std::chrono::duration<double, std::milli>(readback - *m_state->workDoneAt).count();
                }
                else
                {
                    timing.submitToWorkDoneMs =
                      std::chrono::duration<double, std::milli>(readback - *m_state->submittedAt).count();
                }
                return timing;
            }

            [[nodiscard]] std::optional<compute::FrameMetrics> getFrameMetrics() const override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->metrics;
            }

          private:
            static std::string loadDefaultShader()
            {
                constexpr std::string_view DEFAULT_EVALUATOR = R"(
fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
    let distance = length(position) - 0.5;
    return vec4<f32>(vec3<f32>(1.0), distance);
}
)";
                return WebGPUFrameShaderComposer::compose(DEFAULT_EVALUATOR);
            }

            void submitFrameWithPipeline()
            {
                if (!m_skipCpuReadback && m_frameSubmitted)
                {
                    return;
                }
                if (m_skipCpuReadback)
                {
                    std::scoped_lock lock{m_state->mutex};
                    if (m_state->status != compute::ComputeCompletionStatus::Pending ||
                        m_state->subbandInFlight || m_nextSubbandRow >= m_rowCount)
                    {
                        return;
                    }
                }

                auto const pipelineHandles = m_pipeline->getReadyHandles();
                if (!pipelineHandles.has_value())
                {
                    if (m_pipeline->getStatus() == WebGPUFramePipelineCache::Pipeline::Status::Failed)
                    {
                        auto const errorMessage = m_pipeline->getErrorMessage();
                        throw std::runtime_error(errorMessage.empty()
                                                   ? "WebGPU frame pipeline creation failed"
                                                   : errorMessage);
                    }
                    return;
                }

                auto const state = m_state;
                auto const & queue = m_context->getQueue();
                if (m_skipCpuReadback)
                {
                    bool const hasHeavyPrimitives =
                      m_scenePayloads &&
                      (m_scenePayloads->hasMeshPayloads() || m_scenePayloads->hasBeamPayloads());
                    bool precomputedSdfValid = false;
                    {
                        std::scoped_lock sdfLock{m_precomputedSdf->mutex};
                        precomputedSdfValid = m_precomputedSdf->valid;
                    }
#ifdef __EMSCRIPTEN__
                    // In the browser, each subband waits for an async MapAsync callback on a
                    // subsequent animation frame (~16ms each). Dispatch normal/prebaked frames
                    // in a single pass so interactive camera movement is not throttled to <1 FPS.
                    std::uint32_t const maxPixelsPerDispatch =
                      (hasHeavyPrimitives && !precomputedSdfValid)
                        ? 65536u
                        : std::max(1u, m_width * m_rowCount);
#else
                    // Keep individual GPU dispatches well below the OS/driver TDR timeout
                    // (~2s on Linux amdgpu), especially for unbaked mesh/beam-lattice scenes.
                    std::uint32_t const maxPixelsPerDispatch =
                      (hasHeavyPrimitives && !precomputedSdfValid) ? 2048u : 32768u;
#endif
                    std::uint32_t const rowsPerSubband =
                      std::max(1u, maxPixelsPerDispatch / std::max(m_width, 1u));
                    std::uint32_t const subbandOffset = m_nextSubbandRow;
                    std::uint32_t const subbandRows =
                      std::min(rowsPerSubband, m_rowCount - subbandOffset);
                    bool const isFinalSubband = (subbandOffset + subbandRows) >= m_rowCount;

                    auto const dispatchSize =
                      calculateSliceDispatchSize(m_width, subbandRows, m_workgroupShape);
                    if (!dispatchSize.has_value())
                    {
                        throw std::logic_error("WebGPU frame sub-band dispatch dimensions became invalid");
                    }

                    FrameUniforms subbandUniforms = m_baseUniforms;
                    subbandUniforms.firstRowAndCount[0] =
                      static_cast<float>(m_firstRow + subbandOffset);
                    subbandUniforms.firstRowAndCount[1] = static_cast<float>(subbandRows);
                    subbandUniforms.firstRowAndCount[2] = static_cast<float>(m_rowStridePixels);
                    subbandUniforms.firstRowAndCount[3] = static_cast<float>(subbandOffset);
                    m_state->buffers.writeUniforms(queue, subbandUniforms);

                    auto const encoder = m_context->getDevice().CreateCommandEncoder();
                    auto const computePass = encoder.BeginComputePass();
                    computePass.SetPipeline(pipelineHandles->computePipeline);
                    computePass.SetBindGroup(0u, m_bindGroup);
                    computePass.DispatchWorkgroups(
                      dispatchSize->workgroupsX, dispatchSize->workgroupsY);
                    computePass.End();
                    encoder.CopyBufferToBuffer(m_state->buffers.getOutputBuffer(),
                                               0u,
                                               m_state->buffers.getGpuFenceBuffer(),
                                               0u,
                                               sizeof(std::uint32_t));
                    auto const commandBuffer = encoder.Finish();
                    {
                        std::scoped_lock lock{state->mutex};
                        if (!state->submittedAt.has_value())
                        {
                            state->submittedAt = std::chrono::steady_clock::now();
                        }
                        state->subbandInFlight = true;
                    }
                    m_nextSubbandRow = subbandOffset + subbandRows;
                    m_frameSubmitted = true;
                    queue.Submit(1u, &commandBuffer);

                    auto const bytesPerRow =
                      m_rowStridePixels * static_cast<std::uint32_t>(sizeof(std::uint32_t));
                    auto const rowCount = m_rowCount;
                    auto const width = m_width;
                    auto const height = m_height;
                    state->buffers.getGpuFenceBuffer().MapAsync(
                      wgpu::MapMode::Read,
                      0u,
                      sizeof(std::uint32_t),
#ifdef __EMSCRIPTEN__
                      wgpu::CallbackMode::AllowSpontaneous,
#else
                      wgpu::CallbackMode::AllowProcessEvents,
#endif
                      [state, width, height, bytesPerRow, rowCount, isFinalSubband](
                        wgpu::MapAsyncStatus const fenceStatus,
                        wgpu::StringView const message)
                      {
                          std::scoped_lock lock{state->mutex};
                          state->subbandInFlight = false;
                          if (fenceStatus != wgpu::MapAsyncStatus::Success)
                          {
                              state->status = compute::ComputeCompletionStatus::Failed;
                              state->errorMessage =
                                "WebGPU zero-copy frame fence failed: " + toString(message);
                              return;
                          }
                          state->buffers.getGpuFenceBuffer().Unmap();
                          if (!isFinalSubband)
                          {
                              return;
                          }
                          auto gpuHandle = std::make_shared<WebGPUGpuOutputFrame>(
                            WebGPUGpuOutputFrame{.outputBuffer = state->buffers.getOutputBuffer(),
                                                 .bytesPerRow = bytesPerRow,
                                                 .rowCount = rowCount,
                                                 .bufferLease = state->bufferLease});
                          state->result = compute::FrameResult{.width = width,
                                                               .height = height,
                                                               .gpuOutputHandle = std::move(gpuHandle)};
                          auto const now = std::chrono::steady_clock::now();
                          state->workDoneAt = now;
                          state->readbackAt = now;
                          state->status = compute::ComputeCompletionStatus::Succeeded;
                      });
                    return;
                }

                auto const dispatchSize =
                  calculateSliceDispatchSize(m_width, m_rowCount, m_workgroupShape);
                if (!dispatchSize.has_value())
                {
                    throw std::logic_error("WebGPU frame dispatch dimensions became invalid");
                }

                auto const encoder = m_context->getDevice().CreateCommandEncoder();
                if (m_collectMetrics)
                {
                    // The bundled WebGPU header predates the BufferUsage::Clear flag, so
                    // ClearBuffer would fail validation; zero the counters via WriteBuffer.
                    std::array<std::uint32_t, FRAME_METRICS_COUNTER_COUNT> zeroCounters{};
                    m_context->getQueue().WriteBuffer(m_state->buffers.getMetricsBuffer(),
                                                      0u,
                                                      zeroCounters.data(),
                                                      zeroCounters.size() * sizeof(std::uint32_t));
                }
                auto const useTimestamps = m_collectGpuTiming && m_state->buffers.hasTimestampBuffers();
                wgpu::ComputePassDescriptor passDescriptor;
                wgpu::PassTimestampWrites timestampWrites;
                if (useTimestamps)
                {
                    timestampWrites.querySet = m_state->buffers.getQuerySet();
                    timestampWrites.beginningOfPassWriteIndex = 0u;
                    timestampWrites.endOfPassWriteIndex = 1u;
                    passDescriptor.timestampWrites = &timestampWrites;
                }
                auto const computePass = encoder.BeginComputePass(useTimestamps ? &passDescriptor : nullptr);
                computePass.SetPipeline(pipelineHandles->computePipeline);
                computePass.SetBindGroup(0u, m_bindGroup);
                computePass.DispatchWorkgroups(dispatchSize->workgroupsX, dispatchSize->workgroupsY);
                computePass.End();
                if (useTimestamps)
                {
                    encoder.ResolveQuerySet(m_state->buffers.getQuerySet(),
                                            0u,
                                            2u,
                                            m_state->buffers.getTimestampResolveBuffer(),
                                            0u);
                    encoder.CopyBufferToBuffer(m_state->buffers.getTimestampResolveBuffer(),
                                               0u,
                                               m_state->buffers.getTimestampStagingBuffer(),
                                               0u,
                                               2u * sizeof(std::uint64_t));
                }
                if (m_collectMetrics)
                {
                    encoder.CopyBufferToBuffer(m_state->buffers.getMetricsBuffer(),
                                               0u,
                                               m_state->buffers.getMetricsStagingBuffer(),
                                               0u,
                                               FRAME_METRICS_SIZE_BYTES);
                }
                encoder.CopyBufferToBuffer(m_state->buffers.getOutputBuffer(),
                                           0u,
                                           m_state->buffers.getStagingBuffer(),
                                           0u,
                                           state->buffers.getOutputSizeBytes());
                auto const commandBuffer = encoder.Finish();
                {
                    std::scoped_lock lock{state->mutex};
                    state->submittedAt = std::chrono::steady_clock::now();
                }
                queue.Submit(1u, &commandBuffer);
                m_frameSubmitted = true;
                if (m_collectGpuTiming)
                {
                    queue.OnSubmittedWorkDone(
#ifdef __EMSCRIPTEN__
                      wgpu::CallbackMode::AllowSpontaneous,
#else
                      wgpu::CallbackMode::AllowProcessEvents,
#endif
                      [state](wgpu::QueueWorkDoneStatus const workDoneStatus,
                              wgpu::StringView const message)
                      {
                          (void)message;
                          if (workDoneStatus == wgpu::QueueWorkDoneStatus::Success)
                          {
                              std::scoped_lock lock{state->mutex};
                              state->workDoneAt = std::chrono::steady_clock::now();
                          }
                      });
                }

                auto const collectTimestamps = m_collectGpuTiming && m_state->buffers.hasTimestampBuffers();
                auto const collectMetrics = m_collectMetrics;
                auto readMetrics = [state](wgpu::MapAsyncStatus const metricsStatus,
                                           wgpu::StringView const metricsMessage)
                {
                    std::scoped_lock metricsLock{state->mutex};
                    if (metricsStatus != wgpu::MapAsyncStatus::Success)
                    {
                        state->status = compute::ComputeCompletionStatus::Failed;
                        state->errorMessage =
                          "WebGPU frame metrics readback failed: " + toString(metricsMessage);
                        return;
                    }
                    auto const * counters = static_cast<std::uint32_t const *>(
                      state->buffers.getMetricsStagingBuffer().GetConstMappedRange(
                        0u, FRAME_METRICS_SIZE_BYTES));
                    if (counters == nullptr)
                    {
                        state->buffers.getMetricsStagingBuffer().Unmap();
                        state->status = compute::ComputeCompletionStatus::Failed;
                        state->errorMessage = "WebGPU metrics staging buffer returned no mapped data";
                        return;
                    }
                    state->metrics = compute::FrameMetrics{
                      .totalRays = counters[0],
                      .totalSteps = counters[1],
                      .totalHits = counters[2],
                      .nonConverged = counters[3],
                      .refinementTaps = counters[4],
                      .shadowTaps = counters[5],
                      .aoTaps = counters[6],
                      .backtracks = counters[7],
                      .interiorSteps = counters[8]};
                    state->buffers.getMetricsStagingBuffer().Unmap();
                    state->status = compute::ComputeCompletionStatus::Succeeded;
                };
                auto readTimestamps = [state, collectMetrics, readMetrics](wgpu::MapAsyncStatus const timestampStatus,
                                                              wgpu::StringView const timestampMessage)
                {
                    (void)timestampMessage;
                    {
                        std::scoped_lock lock{state->mutex};
                        if (timestampStatus == wgpu::MapAsyncStatus::Success)
                        {
                            auto const * timestamps = static_cast<std::uint64_t const *>(
                              state->buffers.getTimestampStagingBuffer().GetConstMappedRange(
                                0u, 2u * sizeof(std::uint64_t)));
                            if (timestamps != nullptr)
                            {
                                auto const deltaNs = timestamps[1] - timestamps[0];
                                if (deltaNs > 0u)
                                {
                                    state->gpuPassMs = static_cast<double>(deltaNs) / 1'000'000.0;
                                    state->gpuTimestampsAvailable = true;
                                }
                            }
                            state->buffers.getTimestampStagingBuffer().Unmap();
                        }
                    }
                    if (collectMetrics)
                    {
                        state->buffers.getMetricsStagingBuffer().MapAsync(
                          wgpu::MapMode::Read,
                          0u,
                          FRAME_METRICS_SIZE_BYTES,
#ifdef __EMSCRIPTEN__
                          wgpu::CallbackMode::AllowSpontaneous,
#else
                          wgpu::CallbackMode::AllowProcessEvents,
#endif
                          readMetrics);
                        return;
                    }
                    std::scoped_lock lock{state->mutex};
                    state->status = compute::ComputeCompletionStatus::Succeeded;
                };

                state->buffers.getStagingBuffer().MapAsync(
                  wgpu::MapMode::Read,
                  0u,
                  state->buffers.getOutputSizeBytes(),
#ifdef __EMSCRIPTEN__
                  wgpu::CallbackMode::AllowSpontaneous,
#else
                  wgpu::CallbackMode::AllowProcessEvents,
#endif
                  [state, width = m_width, height = m_height, collectTimestamps, collectMetrics, readMetrics, readTimestamps](
                      wgpu::MapAsyncStatus const status,
                      wgpu::StringView const message)
                  {
                      std::unique_lock lock{state->mutex};
                      if (status != wgpu::MapAsyncStatus::Success)
                      {
                          state->status = compute::ComputeCompletionStatus::Failed;
                          state->errorMessage = "WebGPU frame readback failed: " + toString(message);
                          return;
                      }

                      auto const * mappedPixels = static_cast<std::uint32_t const *>(
                        state->buffers.getStagingBuffer().GetConstMappedRange(
                          0u, state->buffers.getOutputSizeBytes()));
                      if (mappedPixels == nullptr)
                      {
                          state->buffers.getStagingBuffer().Unmap();
                          state->status = compute::ComputeCompletionStatus::Failed;
                          state->errorMessage = "WebGPU frame staging buffer returned no mapped data";
                          return;
                      }

                      auto const pixelCount =
                        state->buffers.getOutputSizeBytes() / sizeof(std::uint32_t);
                      state->result = compute::FrameResult{.width = width,
                                                           .height = height,
                                                           .pixels = {mappedPixels, mappedPixels + pixelCount}};
                      state->buffers.getStagingBuffer().Unmap();
                      state->readbackAt = std::chrono::steady_clock::now();
                      lock.unlock();

                      if (collectTimestamps)
                      {
                          state->buffers.getTimestampStagingBuffer().MapAsync(
                            wgpu::MapMode::Read,
                            0u,
                            2u * sizeof(std::uint64_t),
#ifdef __EMSCRIPTEN__
                            wgpu::CallbackMode::AllowSpontaneous,
#else
                            wgpu::CallbackMode::AllowProcessEvents,
#endif
                            readTimestamps);
                          return;
                      }

                      if (!collectMetrics)
                      {
                          std::scoped_lock finalizeLock{state->mutex};
                          state->status = compute::ComputeCompletionStatus::Succeeded;
                          return;
                      }

                      // Metrics copy is part of the same command buffer, so the GPU work
                      // finished already; this MapAsync completes on the next event processing.
                      state->buffers.getMetricsStagingBuffer().MapAsync(
                        wgpu::MapMode::Read,
                        0u,
                        FRAME_METRICS_SIZE_BYTES,
#ifdef __EMSCRIPTEN__
                        wgpu::CallbackMode::AllowSpontaneous,
#else
                        wgpu::CallbackMode::AllowProcessEvents,
#endif
                        readMetrics);
                  });
            }

            void submit(compute::FrameRequest request, WebGPUFrameSubmissionOptions options)
            {
                if (!m_context || !m_context->isValid())
                {
                    throw std::runtime_error("WebGPU compute context is unavailable");
                }
                // Serialize all device/queue access: Dawn instance processing and queue
                // submission must not run concurrently from multiple threads.
                WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
                if (request.endRow == 0u)
                {
                    request.endRow = request.height;
                }
                if (!request.workgroupShape.isValid())
                {
                    throw std::invalid_argument("Invalid WebGPU workgroup shape " +
                                                request.workgroupShape.toString());
                }
                if (request.firstRow >= request.endRow || request.endRow > request.height ||
                    !calculateSliceDispatchSize(request.width,
                                                request.endRow - request.firstRow,
                                                request.workgroupShape)
                       .has_value())
                {
                    throw std::invalid_argument("Invalid WebGPU frame row range");
                }
                if (request.horizontalScale <= 0.0f || request.verticalScale <= 0.0f ||
                    request.maxRaySteps == 0u || request.maxTravelDistance <= 0.0f)
                {
                    throw std::invalid_argument("WebGPU frame camera values must be positive");
                }
                if (request.maxRaySteps > MAX_FRAME_RAY_STEPS)
                {
                    throw std::invalid_argument(
                      "WebGPU frame maxRaySteps exceeds the supported limit of 2048");
                }
                if (request.width > MAX_FRAME_DIMENSION || request.height > MAX_FRAME_DIMENSION)
                {
                    throw std::invalid_argument("WebGPU frame dimensions exceed the safe limit");
                }
                if (request.modelBounds.has_value() && !request.modelBounds->isValid())
                {
                    throw std::invalid_argument("WebGPU frame model bounds are invalid");
                }
                if (request.boundsErrorBound.has_value() &&
                    !(std::isfinite(*request.boundsErrorBound) && *request.boundsErrorBound >= 0.0f))
                {
                    throw std::invalid_argument("WebGPU frame bounds error bound must be finite and non-negative");
                }

                auto const framePixelCount = static_cast<std::size_t>(request.width) *
                                              static_cast<std::size_t>(request.height);
                if (framePixelCount > MAX_FRAME_PIXELS ||
                    framePixelCount / request.width != request.height)
                {
                    throw std::invalid_argument("WebGPU frame pixel count exceeds the safe limit");
                }

                m_collectGpuTiming = request.collectGpuTiming;
                m_collectMetrics = request.collectMetrics;
                m_skipCpuReadback =
                  request.skipCpuReadback && !m_collectGpuTiming && !m_collectMetrics;
                m_rowStridePixels =
                  m_skipCpuReadback ? ((request.width + 63u) & ~63u) : request.width;
                auto const effectiveRenderingFlags =
                  m_collectMetrics ? request.renderingFlags | RF_DEBUG_METRICS
                                   : request.renderingFlags;

                auto const rowCount = request.endRow - request.firstRow;
                FrameUniforms uniforms{
                  .eyeAndMaxDistance = {request.eyePosition[0],
                                        request.eyePosition[1],
                                        request.eyePosition[2],
                                        request.maxTravelDistance},
                  .forwardAndHorizontalScale = {request.forwardDirection[0],
                                                request.forwardDirection[1],
                                                request.forwardDirection[2],
                                                request.horizontalScale},
                  .rightAndWidth = {request.rightDirection[0],
                                    request.rightDirection[1],
                                    request.rightDirection[2],
                                    static_cast<float>(request.width)},
                  .upAndHeight = {request.upDirection[0],
                                  request.upDirection[1],
                                  request.upDirection[2],
                                  static_cast<float>(request.height)},
                  .verticalScaleAndMaxSteps = {request.verticalScale,
                                               static_cast<float>(request.maxRaySteps),
                                               request.boundsErrorBound.value_or(0.0f),
                                               0.0f},
                  .firstRowAndCount = {static_cast<float>(request.firstRow),
                                       static_cast<float>(rowCount),
                                       m_skipCpuReadback ? static_cast<float>(m_rowStridePixels)
                                                         : 0.0f,
                                       0.0f},
                  .timeSliceQualityNormal = {request.timeSeconds,
                                             request.sliceHeight,
                                             request.quality,
                                             request.normalOffset},
                  .flagsModeReserved = {effectiveRenderingFlags,
                                        request.renderingMode,
                                        request.modelBounds.has_value() ? 1u : 0u,
                                        request.translucencyMaxSurfaces},
                  .clippingBoxMin = {},
                  .clippingBoxMax = {},
                  .translucencyParams = request.translucencyParams};
                if (request.modelBounds.has_value())
                {
                    uniforms.clippingBoxMin = {request.modelBounds->min[0],
                                               request.modelBounds->min[1],
                                               request.modelBounds->min[2],
                                               0.0f};
                    uniforms.clippingBoxMax = {request.modelBounds->max[0],
                                               request.modelBounds->max[1],
                                               request.modelBounds->max[2],
                                               0.0f};
                }

                m_firstRow = request.firstRow;
                m_baseUniforms = uniforms;
                m_nextSubbandRow = 0u;
                m_state->buffers.resize(m_context->getDevice(),
                                        request.width,
                                        rowCount,
                                        request.parameterValues.size(),
                                        m_rowStridePixels);
                if (!m_skipCpuReadback)
                {
                    m_state->buffers.writeUniforms(m_context->getQueue(), uniforms);
                }
                m_state->buffers.writeParametersIfChanged(m_context->getQueue(),
                                                          request.parameterValues,
                                                          options.sceneResourceId,
                                                          options.parameterGeneration);
                if (!m_scenePayloads)
                {
                    m_state->buffers.setMeshPayloads(m_context->getDevice(),
                                                     m_context->getQueue(),
                                                     request.meshPayloadTable);
                    m_state->buffers.setBeamPayloads(m_context->getDevice(),
                                                     m_context->getQueue(),
                                                     request.beamPayloadTable);
                    m_state->buffers.setImagePayloads(m_context->getDevice(),
                                                      m_context->getQueue(),
                                                      request.imagePayloadTable);
                }
                m_state->buffers.ensureMetricsBuffers(m_context->getDevice());
                if (m_skipCpuReadback)
                {
                    m_state->buffers.ensureGpuFenceBuffer(m_context->getDevice());
                }
                if (m_collectGpuTiming && m_context->supportsTimestampQueries())
                {
                    m_state->buffers.ensureTimestampBuffers(m_context->getDevice());
                }

                std::uint64_t shaderHash = options.shaderHash;
                if (request.shaderSource.empty())
                {
                    request.shaderSource = loadDefaultShader();
                    shaderHash = 0u;
                }
                if (request.workgroupShape.width != SLICE_WORKGROUP_WIDTH ||
                    request.workgroupShape.height != SLICE_WORKGROUP_HEIGHT)
                {
                    request.shaderSource =
                      WebGPUFrameShaderComposer::applyWorkgroupShape(request.shaderSource,
                                                                     request.workgroupShape);
                    shaderHash = 0u;
                }

                bool const hasMeshPayloads = m_scenePayloads ? m_scenePayloads->hasMeshPayloads()
                                                             : m_state->buffers.hasMeshPayloads();
                bool const hasBeamPayloads = m_scenePayloads ? m_scenePayloads->hasBeamPayloads()
                                                             : m_state->buffers.hasBeamPayloads();
                bool const hasImagePayloads = m_scenePayloads ? m_scenePayloads->hasImagePayloads()
                                                              : m_state->buffers.hasImagePayloads();
                bool const useHardwareFilteredSdf = m_context->supportsFloat32Filterable();
                auto const pipelineResult = m_pipelineCache->getOrCreate(m_context->getDevice(),
                                                                         request.shaderSource,
                                                                         shaderHash,
                                                                         hasMeshPayloads,
                                                                         hasBeamPayloads,
                                                                         hasImagePayloads,
                                                                         useHardwareFilteredSdf);
                m_pipeline = pipelineResult.pipeline;
                m_rowCount = rowCount;
                m_workgroupShape = request.workgroupShape;

                m_precomputedSdf->ensureInitialized(m_context->getDevice(),
                                                    m_context->getQueue(),
                                                    1u,
                                                    useHardwareFilteredSdf);
                wgpu::Buffer precomputedUniformBuffer;
                wgpu::TextureView precomputedTextureView;
                wgpu::Sampler precomputedSampler;
                {
                    std::scoped_lock lock{m_precomputedSdf->mutex};
                    precomputedUniformBuffer = m_precomputedSdf->uniformBuffer;
                    precomputedTextureView = m_precomputedSdf->textureView;
                    if (useHardwareFilteredSdf)
                    {
                        precomputedSampler = m_precomputedSdf->linearSampler;
                    }
                }

                m_bindGroup = m_state->buffers.getOrCreateFrameBindGroup(
                  m_context->getDevice(),
                  m_pipeline->getBindGroupLayout(),
                  m_scenePayloads.get(),
                  precomputedUniformBuffer,
                  precomputedTextureView,
                  precomputedSampler);
                submitFrameWithPipeline();
            }

            std::shared_ptr<WebGPUComputeContext> m_context;
            std::uint32_t m_width{};
            std::uint32_t m_height{};
            std::uint32_t m_firstRow{};
            std::uint32_t m_rowCount{};
            std::uint32_t m_rowStridePixels{};
            std::uint32_t m_nextSubbandRow{};
            FrameUniforms m_baseUniforms{};
            compute::WorkgroupShape m_workgroupShape{};
            std::shared_ptr<WebGPUFramePipelineCache> m_pipelineCache;
            std::shared_ptr<WebGPUFramePipelineCache::Pipeline> m_pipeline;
            std::shared_ptr<PrecomputedSdfState> m_precomputedSdf;
            std::shared_ptr<WebGPUScenePayloadBuffers const> m_scenePayloads;
            wgpu::BindGroup m_bindGroup;
            bool m_frameSubmitted{false};
            bool m_collectGpuTiming{false};
            bool m_collectMetrics{false};
            bool m_skipCpuReadback{false};
            std::shared_ptr<WebGPUFrameSubmissionState> m_state;
        };

        class WebGPUSdfPrebakeSubmission final : public compute::IRenderSubmission
        {
          public:
            WebGPUSdfPrebakeSubmission(
              std::shared_ptr<WebGPUComputeContext> context,
              SdfPrebakeRequest request,
              std::shared_ptr<WebGPUFrameBufferSet> bufferLease,
              std::shared_ptr<WebGPUPrebakePipelineCache> prebakePipelineCache,
              std::shared_ptr<PrecomputedSdfState> precomputedSdf)
                : m_context(std::move(context))
                , m_precomputedSdf(std::move(precomputedSdf))
                , m_bufferLease(std::move(bufferLease))
                , m_prebakePipelineCache(std::move(prebakePipelineCache))
                , m_scenePayloads(std::move(request.scenePayloads))
                , m_gridSize(std::clamp(request.gridSize, 8u, 256u))
                , m_state(std::make_shared<PrebakeStatusState>())
            {
                try
                {
                    prepare(std::move(request));
                }
                catch (std::exception const & exception)
                {
                    std::scoped_lock lock{m_state->mutex};
                    m_state->status = compute::RenderSubmissionStatus::Failed;
                    m_state->errorMessage = exception.what();
                }
            }

            ~WebGPUSdfPrebakeSubmission() override
            {
#ifndef __EMSCRIPTEN__
                if (getStatus() == compute::RenderSubmissionStatus::Pending)
                {
                    try
                    {
                        wait();
                    }
                    catch (...)
                    {
                    }
                }
#endif
            }

            [[nodiscard]] compute::RenderSubmissionStatus getStatus() const noexcept override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->status;
            }

            void progress() noexcept override
            {
                if (getStatus() != compute::RenderSubmissionStatus::Pending || !m_context)
                {
                    return;
                }
                try
                {
#ifndef __EMSCRIPTEN__
                    m_context->processEvents();
#endif
                    if (!m_context->isValid())
                    {
                        std::scoped_lock lock{m_state->mutex};
                        if (m_state->status == compute::RenderSubmissionStatus::Pending)
                        {
                            m_state->status = compute::RenderSubmissionStatus::Failed;
                            m_state->errorMessage = m_context->getErrorMessage();
                        }
                        return;
                    }
                    submitIfPipelineReady();
                }
                catch (std::exception const & exception)
                {
                    std::scoped_lock lock{m_state->mutex};
                    if (m_state->status == compute::RenderSubmissionStatus::Pending)
                    {
                        m_state->status = compute::RenderSubmissionStatus::Failed;
                        m_state->errorMessage = exception.what();
                    }
                }
            }

            void requestCancellation() noexcept override
            {
                {
                    std::scoped_lock lock{m_state->mutex};
                    if (m_state->status == compute::RenderSubmissionStatus::Pending)
                    {
                        m_state->status = compute::RenderSubmissionStatus::Cancelled;
                    }
                }
                if (m_precomputedSdf && m_context && m_context->isValid())
                {
                    m_precomputedSdf->invalidate(m_context->getQueue());
                }
            }

            void wait() override
            {
#ifdef __EMSCRIPTEN__
                // Browser callbacks are delivered by the event loop. Waiting here would block
                // that loop and prevent the prebake completion callback from ever running.
                return;
#else
                while (getStatus() == compute::RenderSubmissionStatus::Pending)
                {
                    progress();
                    if (getStatus() == compute::RenderSubmissionStatus::Pending)
                    {
                        std::this_thread::yield();
                    }
                }
#endif
            }

            [[nodiscard]] std::optional<compute::RenderFrame> takeFrame() override
            {
                return std::nullopt;
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->errorMessage;
            }

          private:
            struct PrebakeStatusState
            {
                std::mutex mutex;
                compute::RenderSubmissionStatus status{compute::RenderSubmissionStatus::Pending};
                bool slabInFlight{false};
                std::string errorMessage;
            };
            void prepare(SdfPrebakeRequest request)
            {
                if (!m_context || !m_context->isValid() || !m_precomputedSdf || !m_bufferLease)
                {
                    throw std::runtime_error("WebGPU context is unavailable for SDF prebaking");
                }
                if (!request.bounds.isValid() || request.shaderSource.empty())
                {
                    throw std::invalid_argument("Invalid WebGPU SDF prebake request");
                }

                WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
                auto const & device = m_context->getDevice();
                auto const & queue = m_context->getQueue();

                m_precomputedSdf->invalidate(queue);
                m_precomputedSdf->ensureInitialized(device, queue, m_gridSize);
                {
                    std::scoped_lock lock{m_precomputedSdf->mutex};
                    m_epoch = m_precomputedSdf->generationEpoch;
                }

                float const spanX = std::max(request.bounds.max[0] - request.bounds.min[0], 1.0f);
                float const spanY = std::max(request.bounds.max[1] - request.bounds.min[1], 1.0f);
                float const spanZ = std::max(request.bounds.max[2] - request.bounds.min[2], 1.0f);
                float const margin = std::max({spanX, spanY, spanZ}) * 0.05f + 2.0f;
                std::array<float, 3> const paddedMin{request.bounds.min[0] - margin,
                                                     request.bounds.min[1] - margin,
                                                     request.bounds.min[2] - margin};
                std::array<float, 3> const paddedMax{request.bounds.max[0] + margin,
                                                     request.bounds.max[1] + margin,
                                                     request.bounds.max[2] + margin};
                float const voxelX = (paddedMax[0] - paddedMin[0]) / static_cast<float>(m_gridSize - 1u);
                float const voxelY = (paddedMax[1] - paddedMin[1]) / static_cast<float>(m_gridSize - 1u);
                float const voxelZ = (paddedMax[2] - paddedMin[2]) / static_cast<float>(m_gridSize - 1u);
                float const tolerance = std::max({voxelX, voxelY, voxelZ}) * 2.5f;

                m_uniforms.bboxMinAndTolerance = {paddedMin[0], paddedMin[1], paddedMin[2], tolerance};
                m_uniforms.bboxMaxAndValid = {paddedMax[0], paddedMax[1], paddedMax[2], 1.0f};
                m_uniforms.gridDims = {m_gridSize, m_gridSize, m_gridSize, 0u};

                PrecomputedSdfUniforms prebakeDispatchUniforms = m_uniforms;
                prebakeDispatchUniforms.bboxMaxAndValid[3] = 0.0f;
                queue.WriteBuffer(m_precomputedSdf->uniformBuffer,
                                  0u,
                                  &prebakeDispatchUniforms,
                                  sizeof(prebakeDispatchUniforms));

                m_bufferLease->resize(device, 1u, 1u, request.parameters.size());
                m_bufferLease->ensureGpuFenceBuffer(device);
                m_bufferLease->writeParameters(queue, request.parameters);
                if (!m_scenePayloads)
                {
                    m_bufferLease->setMeshPayloads(device, queue, request.meshPayloads);
                    m_bufferLease->setBeamPayloads(device, queue, request.beamPayloads);
                    m_bufferLease->setImagePayloads(device, queue, request.imagePayloads);
                }

                bool const hasMeshes =
                  m_scenePayloads ? m_scenePayloads->hasMeshPayloads() : m_bufferLease->hasMeshPayloads();
                bool const hasBeams =
                  m_scenePayloads ? m_scenePayloads->hasBeamPayloads() : m_bufferLease->hasBeamPayloads();
                bool const hasImages =
                  m_scenePayloads ? m_scenePayloads->hasImagePayloads() : m_bufferLease->hasImagePayloads();

                m_pipeline = m_prebakePipelineCache->getOrCreate(
                  device, request.shaderSource, request.shaderHash, hasMeshes, hasBeams, hasImages);

                wgpu::BindGroupEntry entries[9]{};
                entries[0].binding = 0u;
                entries[0].buffer = m_precomputedSdf->uniformBuffer;
                entries[0].size = sizeof(PrecomputedSdfUniforms);
                entries[1].binding = 1u;
                entries[1].textureView = m_precomputedSdf->textureView;
                entries[2].binding = 2u;
                entries[2].buffer = m_bufferLease->getParameterBuffer();
                entries[2].size = m_bufferLease->getParameterSizeBytes();
                std::size_t entryCount = 3u;
                if (hasMeshes)
                {
                    auto const & meshBuf = m_scenePayloads ? m_scenePayloads->meshPayloadBuffer
                                                           : m_bufferLease->getMeshPayloadBuffer();
                    auto const & meshOff = m_scenePayloads ? m_scenePayloads->meshOffsetTableBuffer
                                                           : m_bufferLease->getMeshOffsetTableBuffer();
                    entries[entryCount].binding = 4u;
                    entries[entryCount].buffer = meshBuf;
                    entries[entryCount].size = meshBuf.GetSize();
                    ++entryCount;
                    entries[entryCount].binding = 5u;
                    entries[entryCount].buffer = meshOff;
                    entries[entryCount].size = meshOff.GetSize();
                    ++entryCount;
                }
                if (hasBeams)
                {
                    auto const & beamBuf = m_scenePayloads ? m_scenePayloads->beamPayloadBuffer
                                                           : m_bufferLease->getBeamPayloadBuffer();
                    auto const & beamOff = m_scenePayloads ? m_scenePayloads->beamOffsetTableBuffer
                                                           : m_bufferLease->getBeamOffsetTableBuffer();
                    entries[entryCount].binding = 6u;
                    entries[entryCount].buffer = beamBuf;
                    entries[entryCount].size = beamBuf.GetSize();
                    ++entryCount;
                    entries[entryCount].binding = 7u;
                    entries[entryCount].buffer = beamOff;
                    entries[entryCount].size = beamOff.GetSize();
                    ++entryCount;
                }
                if (hasImages)
                {
                    auto const & imgBuf = m_scenePayloads ? m_scenePayloads->imagePayloadBuffer
                                                          : m_bufferLease->getImagePayloadBuffer();
                    auto const & imgOff = m_scenePayloads ? m_scenePayloads->imageOffsetTableBuffer
                                                          : m_bufferLease->getImageOffsetTableBuffer();
                    entries[entryCount].binding = 8u;
                    entries[entryCount].buffer = imgBuf;
                    entries[entryCount].size = imgBuf.GetSize();
                    ++entryCount;
                    entries[entryCount].binding = 9u;
                    entries[entryCount].buffer = imgOff;
                    entries[entryCount].size = imgOff.GetSize();
                    ++entryCount;
                }

                wgpu::BindGroupDescriptor bindGroupDescriptor;
                bindGroupDescriptor.layout = m_pipeline->getBindGroupLayout();
                bindGroupDescriptor.entryCount = entryCount;
                bindGroupDescriptor.entries = entries;
                m_bindGroup = device.CreateBindGroup(&bindGroupDescriptor);

                submitIfPipelineReady();
            }

            void submitIfPipelineReady()
            {
                if (!m_pipeline)
                {
                    return;
                }
                {
                    std::scoped_lock lock{m_state->mutex};
                    if (m_state->status != compute::RenderSubmissionStatus::Pending ||
                        m_state->slabInFlight)
                    {
                        return;
                    }
                }
                auto const handles = m_pipeline->getReadyHandles();
                if (!handles.has_value())
                {
                    if (m_pipeline->getStatus() == WebGPUFramePipelineCache::Pipeline::Status::Failed)
                    {
                        throw std::runtime_error(m_pipeline->getErrorMessage());
                    }
                    return;
                }

                {
                    std::scoped_lock lock{m_precomputedSdf->mutex};
                    if (m_precomputedSdf->generationEpoch != m_epoch)
                    {
                        std::scoped_lock stateLock{m_state->mutex};
                        m_state->status = compute::RenderSubmissionStatus::Cancelled;
                        return;
                    }
                }

                bool const hasHeavyPrimitives =
                  m_scenePayloads &&
                  (m_scenePayloads->hasMeshPayloads() || m_scenePayloads->hasBeamPayloads());
                std::uint32_t const kSlicesPerSlab = hasHeavyPrimitives ? 2u : 8u;
                std::uint32_t const slabStartZ = m_nextSliceZ;
                if (slabStartZ >= m_gridSize)
                {
                    return;
                }
                std::uint32_t const slabSlices = std::min(kSlicesPerSlab, m_gridSize - slabStartZ);
                bool const isFinalSlab = (slabStartZ + slabSlices) >= m_gridSize;

                WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
                auto const & queue = m_context->getQueue();
                PrecomputedSdfUniforms slabUniforms = m_uniforms;
                slabUniforms.bboxMaxAndValid[3] = 0.0f;
                slabUniforms.gridDims[3] = slabStartZ;
                queue.WriteBuffer(
                  m_precomputedSdf->uniformBuffer, 0u, &slabUniforms, sizeof(slabUniforms));

                auto const encoder = m_context->getDevice().CreateCommandEncoder();
                auto const pass = encoder.BeginComputePass();
                pass.SetPipeline(handles->computePipeline);
                pass.SetBindGroup(0u, m_bindGroup);
                auto const groupsXY = (m_gridSize + 3u) / 4u;
                auto const groupsZ = (slabSlices + 3u) / 4u;
                pass.DispatchWorkgroups(groupsXY, groupsXY, groupsZ);
                pass.End();
                encoder.CopyBufferToBuffer(m_bufferLease->getOutputBuffer(),
                                           0u,
                                           m_bufferLease->getGpuFenceBuffer(),
                                           0u,
                                           sizeof(std::uint32_t));
                auto const commands = encoder.Finish();
                {
                    std::scoped_lock lock{m_state->mutex};
                    m_state->slabInFlight = true;
                }
                m_nextSliceZ = slabStartZ + slabSlices;
                queue.Submit(1u, &commands);

                auto const state = m_state;
                auto const bufferLease = m_bufferLease;
                auto const precomputedSdf = m_precomputedSdf;
                auto const context = m_context;
                auto const epoch = m_epoch;
                auto const finalUniforms = m_uniforms;
#ifdef __EMSCRIPTEN__
                auto const callbackMode = wgpu::CallbackMode::AllowSpontaneous;
#else
                auto const callbackMode = wgpu::CallbackMode::AllowProcessEvents;
#endif
                bufferLease->getGpuFenceBuffer().MapAsync(
                  wgpu::MapMode::Read,
                  0u,
                  sizeof(std::uint32_t),
                  callbackMode,
                  [state, bufferLease, precomputedSdf, context, epoch, finalUniforms, isFinalSlab](
                    wgpu::MapAsyncStatus const status,
                    wgpu::StringView const message)
                  {
                      if (status == wgpu::MapAsyncStatus::Success)
                      {
                          bufferLease->getGpuFenceBuffer().Unmap();
                          if (isFinalSlab)
                          {
                              {
                                  std::scoped_lock sdfLock{precomputedSdf->mutex};
                                  if (precomputedSdf->generationEpoch == epoch && context &&
                                      context->isValid())
                                  {
                                      precomputedSdf->valid = true;
                                      context->getQueue().WriteBuffer(
                                        precomputedSdf->uniformBuffer,
                                        0u,
                                        &finalUniforms,
                                        sizeof(finalUniforms));
                                  }
                              }
                              std::scoped_lock lock{state->mutex};
                              state->slabInFlight = false;
                              if (state->status == compute::RenderSubmissionStatus::Pending)
                              {
                                  state->status = compute::RenderSubmissionStatus::Succeeded;
                              }
                          }
                          else
                          {
                              std::scoped_lock lock{state->mutex};
                              state->slabInFlight = false;
                          }
                      }
                      else
                      {
                          std::scoped_lock lock{state->mutex};
                          state->slabInFlight = false;
                          if (state->status == compute::RenderSubmissionStatus::Pending)
                          {
                              state->status = compute::RenderSubmissionStatus::Failed;
                              state->errorMessage =
                                "WebGPU SDF prebake failed: " + toString(message);
                          }
                      }
                  });
            }

            std::shared_ptr<WebGPUComputeContext> m_context;
            std::shared_ptr<PrecomputedSdfState> m_precomputedSdf;
            std::shared_ptr<WebGPUFrameBufferSet> m_bufferLease;
            std::shared_ptr<WebGPUPrebakePipelineCache> m_prebakePipelineCache;
            std::shared_ptr<WebGPUScenePayloadBuffers const> m_scenePayloads;
            std::shared_ptr<WebGPUFramePipelineCache::Pipeline> m_pipeline;
            wgpu::BindGroup m_bindGroup;
            PrecomputedSdfUniforms m_uniforms{};
            std::uint32_t m_gridSize{128u};
            std::uint32_t m_nextSliceZ{0u};
            std::uint64_t m_epoch{0u};
            std::shared_ptr<PrebakeStatusState> m_state;
        };
    }

    WebGPUComputeBackend::WebGPUComputeBackend(std::shared_ptr<WebGPUComputeContext> context)
        : m_context(context ? std::move(context) : std::make_shared<WebGPUComputeContext>())
        , m_frameBufferPool(std::make_shared<FrameBufferPool>())
        , m_framePipelineCache(std::make_shared<WebGPUFramePipelineCache>())
        , m_prebakePipelineCache(std::make_shared<WebGPUPrebakePipelineCache>())
        , m_precomputedSdf(std::make_shared<PrecomputedSdfState>())
    {
    }

    compute::ComputeBackendKind WebGPUComputeBackend::getKind() const noexcept
    { return compute::ComputeBackendKind::WebGPU; }

    bool WebGPUComputeBackend::isAvailable() const noexcept
    { return m_context && m_context->isValid(); }

    std::shared_ptr<WebGPUScenePayloadBuffers> WebGPUComputeBackend::createScenePayloadBuffers(
      std::vector<std::vector<float>> const & meshPayloadTable,
      std::vector<std::vector<float>> const & beamPayloadTable,
      std::vector<std::vector<std::uint32_t>> const & imagePayloadTable)
    {
        if (!isAvailable())
        {
            return nullptr;
        }
        WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
        auto const resourceId = m_nextScenePayloadId.fetch_add(1u, std::memory_order_relaxed);
        return uploadScenePayloadBuffers(m_context->getDevice(),
                                         m_context->getQueue(),
                                         resourceId,
                                         meshPayloadTable,
                                         beamPayloadTable,
                                         imagePayloadTable);
    }

    std::unique_ptr<compute::ISliceSubmission>
    WebGPUComputeBackend::submitSlice(compute::SliceRequest request)
    { return std::make_unique<WebGPUSliceSubmission>(m_context, std::move(request)); }

    std::unique_ptr<compute::IFrameSubmission>
    WebGPUComputeBackend::submitFrame(compute::FrameRequest request)
    {
        return submitFrame(std::move(request), WebGPUFrameSubmissionOptions{});
    }

    std::unique_ptr<compute::IFrameSubmission>
    WebGPUComputeBackend::submitFrame(compute::FrameRequest request,
                                      WebGPUFrameSubmissionOptions options)
    {
        return std::make_unique<WebGPUFrameSubmission>(
                    m_context,
                    std::move(request),
                    std::move(options),
                    m_frameBufferPool->acquire(),
                    m_framePipelineCache,
                    m_precomputedSdf);
    }

    std::unique_ptr<compute::IRenderSubmission>
    WebGPUComputeBackend::submitSdfPrecomputation(SdfPrebakeRequest request)
    {
        return std::make_unique<WebGPUSdfPrebakeSubmission>(
          m_context,
          std::move(request),
          m_frameBufferPool->acquire(),
          m_prebakePipelineCache,
          m_precomputedSdf);
    }

    void WebGPUComputeBackend::invalidatePrecomputedSdf() noexcept
    {
        if (m_precomputedSdf && m_context && m_context->isValid())
        {
            m_precomputedSdf->invalidate(m_context->getQueue());
        }
    }

    bool WebGPUComputeBackend::hasValidPrecomputedSdf() const noexcept
    {
        if (!m_precomputedSdf)
        {
            return false;
        }
        std::scoped_lock lock{m_precomputedSdf->mutex};
        return m_precomputedSdf->valid;
    }
}
