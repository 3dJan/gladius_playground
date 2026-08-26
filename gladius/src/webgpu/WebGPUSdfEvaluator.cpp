#include "webgpu/WebGPUSdfEvaluator.h"

#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gladius::webgpu
{
    namespace
    {
        constexpr std::uint32_t WORKGROUP_SIZE = 64u;

        struct alignas(16) EvaluationUniforms
        {
            float isoValue{};
            std::uint32_t pointCount{};
            std::uint32_t reserved0{};
            std::uint32_t reserved1{};
        };

        static_assert(sizeof(EvaluationUniforms) == 16u);

        struct alignas(16) EvaluationPosition
        {
            std::array<float, 4> value{};
        };

        class EvaluationBuffers final
        {
          public:
            void create(wgpu::Device const & device,
                        std::size_t const pointCount,
                        std::size_t const parameterCount,
                        std::vector<std::vector<float>> const & meshPayloadTable,
                        std::vector<std::vector<float>> const & beamPayloadTable,
                        std::vector<std::vector<float>> const & imagePayloadTable)
            {
                auto const parameterSize = std::max(sizeof(float), parameterCount * sizeof(float));
                auto const positionSize = pointCount * sizeof(EvaluationPosition);
                auto const outputSize = pointCount * sizeof(float);

                wgpu::BufferDescriptor uniformDescriptor;
                uniformDescriptor.label = "Gladius SDF evaluation uniforms";
                uniformDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
                uniformDescriptor.size = sizeof(EvaluationUniforms);
                m_uniform = device.CreateBuffer(&uniformDescriptor);

                wgpu::BufferDescriptor positionDescriptor;
                positionDescriptor.label = "Gladius SDF evaluation positions";
                positionDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                positionDescriptor.size = positionSize;
                m_positions = device.CreateBuffer(&positionDescriptor);

                wgpu::BufferDescriptor outputDescriptor;
                outputDescriptor.label = "Gladius SDF evaluation output";
                outputDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
                outputDescriptor.size = outputSize;
                m_output = device.CreateBuffer(&outputDescriptor);

                wgpu::BufferDescriptor stagingDescriptor;
                stagingDescriptor.label = "Gladius SDF evaluation readback";
                stagingDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
                stagingDescriptor.size = outputSize;
                m_staging = device.CreateBuffer(&stagingDescriptor);

                wgpu::BufferDescriptor parameterDescriptor;
                parameterDescriptor.label = "Gladius SDF evaluation parameters";
                parameterDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                parameterDescriptor.size = parameterSize;
                m_parameters = device.CreateBuffer(&parameterDescriptor);

                if (!m_uniform || !m_positions || !m_output || !m_staging || !m_parameters)
                {
                    throw std::runtime_error("Unable to allocate WebGPU SDF evaluation buffers");
                }

                m_outputSize = outputSize;
                m_parameterSize = parameterSize;

                createMeshPayloads(device, meshPayloadTable);
                createBeamPayloads(device, beamPayloadTable);
                createImagePayloads(device, imagePayloadTable);
            }

            /// Upload image stack payloads (concatenated) plus a per-resource offset table.
            void createImagePayloads(wgpu::Device const & device,
                                     std::vector<std::vector<float>> const & imagePayloadTable)
            {
                std::vector<std::uint32_t> offsetTable(imagePayloadTable.size() * 2u, 0u);
                std::size_t totalFloats = 0u;
                for (std::size_t slot = 0u; slot < imagePayloadTable.size(); ++slot)
                {
                    auto const & payload = imagePayloadTable[slot];
                    if (payload.empty())
                    {
                        continue;
                    }
                    if (totalFloats > std::numeric_limits<std::uint32_t>::max() ||
                        payload.size() > std::numeric_limits<std::uint32_t>::max() - totalFloats)
                    {
                        throw std::length_error("WebGPU image payload table exceeds 32-bit addressing");
                    }
                    offsetTable[slot * 2u] = static_cast<std::uint32_t>(totalFloats);
                    offsetTable[slot * 2u + 1u] = static_cast<std::uint32_t>(payload.size());
                    totalFloats += payload.size();
                }

                if (totalFloats == 0u)
                {
                    return;
                }

                m_imagePayloadData.clear();
                m_imagePayloadData.reserve(totalFloats);
                for (auto const & payload : imagePayloadTable)
                {
                    m_imagePayloadData.insert(m_imagePayloadData.end(), payload.begin(), payload.end());
                }
                m_imageOffsetTableData = std::move(offsetTable);

                wgpu::BufferDescriptor payloadDescriptor;
                payloadDescriptor.label = "Gladius SDF evaluation image payloads";
                payloadDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                payloadDescriptor.size = m_imagePayloadData.size() * sizeof(float);
                m_imagePayload = device.CreateBuffer(&payloadDescriptor);

                wgpu::BufferDescriptor offsetDescriptor;
                offsetDescriptor.label = "Gladius SDF evaluation image offset table";
                offsetDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                offsetDescriptor.size = m_imageOffsetTableData.size() * sizeof(std::uint32_t);
                m_imageOffsetTable = device.CreateBuffer(&offsetDescriptor);

                if (!m_imagePayload || !m_imageOffsetTable)
                {
                    throw std::runtime_error("Unable to allocate WebGPU image payload buffers");
                }
            }

            /// Upload beam lattice payloads (concatenated) plus a per-resource offset table.
            void createBeamPayloads(wgpu::Device const & device,
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
                    return;
                }

                m_beamPayloadData.clear();
                m_beamPayloadData.reserve(totalFloats);
                for (auto const & payload : beamPayloadTable)
                {
                    m_beamPayloadData.insert(m_beamPayloadData.end(), payload.begin(), payload.end());
                }
                m_beamOffsetTableData = std::move(offsetTable);

                wgpu::BufferDescriptor payloadDescriptor;
                payloadDescriptor.label = "Gladius SDF evaluation beam payloads";
                payloadDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                payloadDescriptor.size = m_beamPayloadData.size() * sizeof(float);
                m_beamPayload = device.CreateBuffer(&payloadDescriptor);

                wgpu::BufferDescriptor offsetDescriptor;
                offsetDescriptor.label = "Gladius SDF evaluation beam offset table";
                offsetDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                offsetDescriptor.size = m_beamOffsetTableData.size() * sizeof(std::uint32_t);
                m_beamOffsetTable = device.CreateBuffer(&offsetDescriptor);

                if (!m_beamPayload || !m_beamOffsetTable)
                {
                    throw std::runtime_error("Unable to allocate WebGPU beam payload buffers");
                }
            }

            /// Upload mesh payloads (concatenated) plus a per-resource offset table.
            void createMeshPayloads(wgpu::Device const & device,
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
                    return;
                }

                m_meshPayloadData.clear();
                m_meshPayloadData.reserve(totalFloats);
                for (auto const & payload : meshPayloadTable)
                {
                    m_meshPayloadData.insert(m_meshPayloadData.end(), payload.begin(), payload.end());
                }
                m_meshOffsetTableData = std::move(offsetTable);

                wgpu::BufferDescriptor payloadDescriptor;
                payloadDescriptor.label = "Gladius SDF evaluation mesh payloads";
                payloadDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                payloadDescriptor.size = m_meshPayloadData.size() * sizeof(float);
                m_meshPayload = device.CreateBuffer(&payloadDescriptor);

                wgpu::BufferDescriptor offsetDescriptor;
                offsetDescriptor.label = "Gladius SDF evaluation mesh offset table";
                offsetDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                offsetDescriptor.size = m_meshOffsetTableData.size() * sizeof(std::uint32_t);
                m_meshOffsetTable = device.CreateBuffer(&offsetDescriptor);

                if (!m_meshPayload || !m_meshOffsetTable)
                {
                    throw std::runtime_error("Unable to allocate WebGPU mesh payload buffers");
                }
            }

            [[nodiscard]] bool hasMeshPayloads() const noexcept { return m_meshPayload && m_meshOffsetTable; }
            [[nodiscard]] wgpu::Buffer const & meshPayload() const noexcept { return m_meshPayload; }
            [[nodiscard]] wgpu::Buffer const & meshOffsetTable() const noexcept { return m_meshOffsetTable; }
            [[nodiscard]] std::vector<float> const & meshPayloadData() const noexcept { return m_meshPayloadData; }
            [[nodiscard]] std::vector<std::uint32_t> const & meshOffsetTableData() const noexcept
            {
                return m_meshOffsetTableData;
            }

            [[nodiscard]] bool hasBeamPayloads() const noexcept { return m_beamPayload && m_beamOffsetTable; }
            [[nodiscard]] wgpu::Buffer const & beamPayload() const noexcept { return m_beamPayload; }
            [[nodiscard]] wgpu::Buffer const & beamOffsetTable() const noexcept { return m_beamOffsetTable; }
            [[nodiscard]] std::vector<float> const & beamPayloadData() const noexcept { return m_beamPayloadData; }
            [[nodiscard]] std::vector<std::uint32_t> const & beamOffsetTableData() const noexcept
            {
                return m_beamOffsetTableData;
            }
            [[nodiscard]] bool hasImagePayloads() const noexcept
            {
                return m_imagePayload && m_imageOffsetTable;
            }
            [[nodiscard]] wgpu::Buffer const & imagePayload() const noexcept { return m_imagePayload; }
            [[nodiscard]] wgpu::Buffer const & imageOffsetTable() const noexcept
            {
                return m_imageOffsetTable;
            }
            [[nodiscard]] std::vector<float> const & imagePayloadData() const noexcept
            {
                return m_imagePayloadData;
            }
            [[nodiscard]] std::vector<std::uint32_t> const & imageOffsetTableData() const noexcept
            {
                return m_imageOffsetTableData;
            }

            void write(wgpu::Queue const & queue,
                       EvaluationUniforms const & uniforms,
                       std::vector<EvaluationPosition> const & positions,
                       std::vector<float> const & parameters) const
            {
                queue.WriteBuffer(m_uniform, 0u, &uniforms, sizeof(uniforms));
                queue.WriteBuffer(m_positions,
                                  0u,
                                  positions.data(),
                                  positions.size() * sizeof(EvaluationPosition));

                float const zero{};
                auto const * data = parameters.empty() ? &zero : parameters.data();
                auto const size = parameters.empty() ? sizeof(zero) : parameters.size() * sizeof(float);
                queue.WriteBuffer(m_parameters, 0u, data, size);
            }

            [[nodiscard]] std::size_t outputSize() const noexcept { return m_outputSize; }
            [[nodiscard]] std::size_t parameterSize() const noexcept { return m_parameterSize; }
            [[nodiscard]] wgpu::Buffer const & uniform() const noexcept { return m_uniform; }
            [[nodiscard]] wgpu::Buffer const & positions() const noexcept { return m_positions; }
            [[nodiscard]] wgpu::Buffer const & output() const noexcept { return m_output; }
            [[nodiscard]] wgpu::Buffer const & staging() const noexcept { return m_staging; }
            [[nodiscard]] wgpu::Buffer const & parameters() const noexcept { return m_parameters; }

          private:
            std::size_t m_outputSize{};
            std::size_t m_parameterSize{};
            wgpu::Buffer m_uniform;
            wgpu::Buffer m_positions;
            wgpu::Buffer m_output;
            wgpu::Buffer m_staging;
            wgpu::Buffer m_parameters;
            wgpu::Buffer m_meshPayload;
            wgpu::Buffer m_meshOffsetTable;
            std::vector<float> m_meshPayloadData;
            std::vector<std::uint32_t> m_meshOffsetTableData;
            wgpu::Buffer m_beamPayload;
            wgpu::Buffer m_beamOffsetTable;
            std::vector<float> m_beamPayloadData;
            std::vector<std::uint32_t> m_beamOffsetTableData;
            wgpu::Buffer m_imagePayload;
            wgpu::Buffer m_imageOffsetTable;
            std::vector<float> m_imagePayloadData;
            std::vector<std::uint32_t> m_imageOffsetTableData;
        };
    }

    WebGPUSdfEvaluator::WebGPUSdfEvaluator()
        : m_context(std::make_shared<WebGPUComputeContext>())
    {
    }

    WebGPUSdfEvaluator::WebGPUSdfEvaluator(std::shared_ptr<WebGPUComputeContext> context)
        : m_context(std::move(context))
    {
    }

    bool WebGPUSdfEvaluator::isAvailable() const noexcept
    {
        return m_context && m_context->isValid();
    }

    std::string const & WebGPUSdfEvaluator::getErrorMessage() const noexcept
    {
        static std::string const empty;
        return m_context ? m_context->getErrorMessage() : empty;
    }

    compute::SdfEvaluationResult WebGPUSdfEvaluator::evaluate(
      compute::SdfEvaluationRequest request) const
    {
        if (request.positions.empty())
        {
            return {};
        }
        if (!isAvailable())
        {
            throw std::runtime_error("WebGPU compute context is unavailable");
        }
        // Serialize all device/queue access: this may run on a worker thread while the
        // UI thread submits frames through the same Dawn device.
        WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
        if (request.shaderSource.empty())
        {
            throw std::invalid_argument("WebGPU SDF evaluator shader source must not be empty");
        }
        if (request.positions.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            throw std::invalid_argument("WebGPU SDF evaluation point count is too large");
        }

        std::vector<EvaluationPosition> positions;
        positions.reserve(request.positions.size());
        for (auto const & position : request.positions)
        {
            positions.push_back(EvaluationPosition{{position[0], position[1], position[2], 0.0f}});
        }

        EvaluationBuffers buffers;
        buffers.create(m_context->getDevice(),
                       positions.size(),
                       request.parameterValues.size(),
                       request.meshPayloadTable,
                       request.beamPayloadTable,
                       request.imagePayloadTable);
        buffers.write(m_context->getQueue(),
                      EvaluationUniforms{request.isoValue,
                                         static_cast<std::uint32_t>(positions.size()),
                                         0u,
                                         0u},
                      positions,
                      request.parameterValues);

        if (buffers.hasMeshPayloads())
        {
            m_context->getQueue().WriteBuffer(buffers.meshPayload(),
                                              0u,
                                              buffers.meshPayloadData().data(),
                                              buffers.meshPayloadData().size() * sizeof(float));
            m_context->getQueue().WriteBuffer(buffers.meshOffsetTable(),
                                              0u,
                                              buffers.meshOffsetTableData().data(),
                                              buffers.meshOffsetTableData().size() * sizeof(std::uint32_t));
        }

        if (buffers.hasBeamPayloads())
        {
            m_context->getQueue().WriteBuffer(buffers.beamPayload(),
                                              0u,
                                              buffers.beamPayloadData().data(),
                                              buffers.beamPayloadData().size() * sizeof(float));
            m_context->getQueue().WriteBuffer(buffers.beamOffsetTable(),
                                              0u,
                                              buffers.beamOffsetTableData().data(),
                                              buffers.beamOffsetTableData().size() * sizeof(std::uint32_t));
        }

        if (buffers.hasImagePayloads())
        {
            m_context->getQueue().WriteBuffer(buffers.imagePayload(),
                                              0u,
                                              buffers.imagePayloadData().data(),
                                              buffers.imagePayloadData().size() * sizeof(float));
            m_context->getQueue().WriteBuffer(buffers.imageOffsetTable(),
                                              0u,
                                              buffers.imageOffsetTableData().data(),
                                              buffers.imageOffsetTableData().size() * sizeof(std::uint32_t));
        }

        wgpu::ShaderSourceWGSL wgsl;
        wgsl.code = {request.shaderSource.data(), request.shaderSource.size()};
        wgpu::ShaderModuleDescriptor shaderDescriptor;
        shaderDescriptor.nextInChain = &wgsl;
        auto const shader = m_context->getDevice().CreateShaderModule(&shaderDescriptor);

        wgpu::BindGroupLayoutEntry bindings[4]{};
        bindings[0].binding = 0u;
        bindings[0].visibility = wgpu::ShaderStage::Compute;
        bindings[0].buffer.type = wgpu::BufferBindingType::Uniform;
        bindings[0].buffer.minBindingSize = sizeof(EvaluationUniforms);
        bindings[1].binding = 1u;
        bindings[1].visibility = wgpu::ShaderStage::Compute;
        bindings[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
        bindings[1].buffer.minBindingSize = positions.size() * sizeof(EvaluationPosition);
        bindings[2].binding = 2u;
        bindings[2].visibility = wgpu::ShaderStage::Compute;
        bindings[2].buffer.type = wgpu::BufferBindingType::Storage;
        bindings[2].buffer.minBindingSize = buffers.outputSize();
        bindings[3].binding = 3u;
        bindings[3].visibility = wgpu::ShaderStage::Compute;
        bindings[3].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
        bindings[3].buffer.minBindingSize = sizeof(float);

        bool const hasMeshPayloads = buffers.hasMeshPayloads();
        bool const hasBeamPayloads = buffers.hasBeamPayloads();
        bool const hasImagePayloads = buffers.hasImagePayloads();
        wgpu::BindGroupLayoutEntry meshBindings[2]{};
        if (hasMeshPayloads)
        {
            meshBindings[0].binding = 4u;
            meshBindings[0].visibility = wgpu::ShaderStage::Compute;
            meshBindings[0].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
            meshBindings[0].buffer.minBindingSize = sizeof(float);
            meshBindings[1].binding = 5u;
            meshBindings[1].visibility = wgpu::ShaderStage::Compute;
            meshBindings[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
            meshBindings[1].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
        }

        wgpu::BindGroupLayoutEntry beamBindings[2]{};
        if (hasBeamPayloads)
        {
            beamBindings[0].binding = 6u;
            beamBindings[0].visibility = wgpu::ShaderStage::Compute;
            beamBindings[0].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
            beamBindings[0].buffer.minBindingSize = sizeof(float);
            beamBindings[1].binding = 7u;
            beamBindings[1].visibility = wgpu::ShaderStage::Compute;
            beamBindings[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
            beamBindings[1].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
        }

        wgpu::BindGroupLayoutEntry imageBindings[2]{};
        if (hasImagePayloads)
        {
            imageBindings[0].binding = 8u;
            imageBindings[0].visibility = wgpu::ShaderStage::Compute;
            imageBindings[0].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
            imageBindings[0].buffer.minBindingSize = sizeof(float);
            imageBindings[1].binding = 9u;
            imageBindings[1].visibility = wgpu::ShaderStage::Compute;
            imageBindings[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
            imageBindings[1].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
        }

        std::vector<wgpu::BindGroupLayoutEntry> allBindings(bindings, bindings + std::size(bindings));
        if (hasMeshPayloads)
        {
            allBindings.push_back(meshBindings[0]);
            allBindings.push_back(meshBindings[1]);
        }
        if (hasBeamPayloads)
        {
            allBindings.push_back(beamBindings[0]);
            allBindings.push_back(beamBindings[1]);
        }
        if (hasImagePayloads)
        {
            allBindings.push_back(imageBindings[0]);
            allBindings.push_back(imageBindings[1]);
        }

        wgpu::BindGroupLayoutDescriptor layoutDescriptor;
        layoutDescriptor.entryCount = static_cast<std::uint32_t>(allBindings.size());
        layoutDescriptor.entries = allBindings.data();
        auto const bindGroupLayout = m_context->getDevice().CreateBindGroupLayout(&layoutDescriptor);

        wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor;
        pipelineLayoutDescriptor.bindGroupLayoutCount = 1u;
        pipelineLayoutDescriptor.bindGroupLayouts = &bindGroupLayout;
        auto const pipelineLayout = m_context->getDevice().CreatePipelineLayout(&pipelineLayoutDescriptor);

        wgpu::ComputePipelineDescriptor pipelineDescriptor;
        pipelineDescriptor.layout = pipelineLayout;
        pipelineDescriptor.compute.module = shader;
        pipelineDescriptor.compute.entryPoint = "main";
        auto const pipeline = m_context->getDevice().CreateComputePipeline(&pipelineDescriptor);

        wgpu::BindGroupEntry bindGroupEntries[4]{};
        bindGroupEntries[0].binding = 0u;
        bindGroupEntries[0].buffer = buffers.uniform();
        bindGroupEntries[0].size = sizeof(EvaluationUniforms);
        bindGroupEntries[1].binding = 1u;
        bindGroupEntries[1].buffer = buffers.positions();
        bindGroupEntries[1].size = positions.size() * sizeof(EvaluationPosition);
        bindGroupEntries[2].binding = 2u;
        bindGroupEntries[2].buffer = buffers.output();
        bindGroupEntries[2].size = buffers.outputSize();
        bindGroupEntries[3].binding = 3u;
        bindGroupEntries[3].buffer = buffers.parameters();
        bindGroupEntries[3].size = buffers.parameterSize();

        std::vector<wgpu::BindGroupEntry> allEntries(bindGroupEntries, bindGroupEntries + std::size(bindGroupEntries));
        if (hasMeshPayloads)
        {
            wgpu::BindGroupEntry payloadEntry{};
            payloadEntry.binding = 4u;
            payloadEntry.buffer = buffers.meshPayload();
            payloadEntry.size = buffers.meshPayloadData().size() * sizeof(float);
            allEntries.push_back(payloadEntry);

            wgpu::BindGroupEntry offsetEntry{};
            offsetEntry.binding = 5u;
            offsetEntry.buffer = buffers.meshOffsetTable();
            offsetEntry.size = buffers.meshOffsetTableData().size() * sizeof(std::uint32_t);
            allEntries.push_back(offsetEntry);
        }

        if (hasBeamPayloads)
        {
            wgpu::BindGroupEntry payloadEntry{};
            payloadEntry.binding = 6u;
            payloadEntry.buffer = buffers.beamPayload();
            payloadEntry.size = buffers.beamPayloadData().size() * sizeof(float);
            allEntries.push_back(payloadEntry);

            wgpu::BindGroupEntry offsetEntry{};
            offsetEntry.binding = 7u;
            offsetEntry.buffer = buffers.beamOffsetTable();
            offsetEntry.size = buffers.beamOffsetTableData().size() * sizeof(std::uint32_t);
            allEntries.push_back(offsetEntry);
        }

        if (hasImagePayloads)
        {
            wgpu::BindGroupEntry payloadEntry{};
            payloadEntry.binding = 8u;
            payloadEntry.buffer = buffers.imagePayload();
            payloadEntry.size = buffers.imagePayloadData().size() * sizeof(float);
            allEntries.push_back(payloadEntry);

            wgpu::BindGroupEntry offsetEntry{};
            offsetEntry.binding = 9u;
            offsetEntry.buffer = buffers.imageOffsetTable();
            offsetEntry.size = buffers.imageOffsetTableData().size() * sizeof(std::uint32_t);
            allEntries.push_back(offsetEntry);
        }

        wgpu::BindGroupDescriptor bindGroupDescriptor;
        bindGroupDescriptor.layout = bindGroupLayout;
        bindGroupDescriptor.entryCount = static_cast<std::uint32_t>(allEntries.size());
        bindGroupDescriptor.entries = allEntries.data();
        auto const bindGroup = m_context->getDevice().CreateBindGroup(&bindGroupDescriptor);

        auto const workgroupCount = (static_cast<std::uint32_t>(positions.size()) + WORKGROUP_SIZE - 1u) /
                                     WORKGROUP_SIZE;
        auto const encoder = m_context->getDevice().CreateCommandEncoder();
        auto const computePass = encoder.BeginComputePass();
        computePass.SetPipeline(pipeline);
        computePass.SetBindGroup(0u, bindGroup);
        computePass.DispatchWorkgroups(workgroupCount);
        computePass.End();
        encoder.CopyBufferToBuffer(buffers.output(),
                                   0u,
                                   buffers.staging(),
                                   0u,
                                   buffers.outputSize());
        auto const commandBuffer = encoder.Finish();
        m_context->getQueue().Submit(1u, &commandBuffer);

        std::vector<float> values(positions.size());
        bool complete = false;
        std::string errorMessage;
        buffers.staging().MapAsync(
          wgpu::MapMode::Read,
          0u,
          buffers.outputSize(),
          wgpu::CallbackMode::AllowProcessEvents,
          [&values, &buffers, &complete, &errorMessage](wgpu::MapAsyncStatus const status,
                                                        wgpu::StringView const message)
          {
              (void)message;
              if (status != wgpu::MapAsyncStatus::Success)
              {
                  errorMessage = "WebGPU SDF evaluation readback failed";
                  complete = true;
                  return;
              }

              auto const * mappedValues = static_cast<float const *>(
                buffers.staging().GetConstMappedRange(0u, buffers.outputSize()));
              if (mappedValues == nullptr)
              {
                  errorMessage = "WebGPU SDF evaluation staging buffer returned no mapped data";
                  complete = true;
                  return;
              }

              std::memcpy(values.data(), mappedValues, buffers.outputSize());
              buffers.staging().Unmap();
              complete = true;
          });

        while (!complete)
        {
            m_context->processEvents();
            if (!m_context->isValid())
            {
                throw std::runtime_error(m_context->getErrorMessage());
            }
        }
        if (!errorMessage.empty())
        {
            throw std::runtime_error(errorMessage);
        }

        return compute::SdfEvaluationResult{.values = std::move(values)};
    }
}
