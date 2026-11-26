#include "webgpu/WebGPUSdfEvaluator.h"
#include "webgpu/WebGPUBrowserGuard.h"
#include "webgpu/WebGPUSdfDispatch.h"

#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <array>
#include <atomic>
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
        constexpr std::size_t MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE = 128u * 1024u * 1024u;

        struct alignas(16) EvaluationUniforms
        {
            float isoValue{};
            std::uint32_t pointCount{};
            std::uint32_t workgroupsX{};
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
                        std::size_t const outputComponents,
                        std::size_t const argumentComponents = 0u)
            {
                auto const parameterSize = std::max(sizeof(float), parameterCount * sizeof(float));
                auto const functionMode = argumentComponents != 0u;
                auto const positionSize = functionMode
                                            ? sizeof(EvaluationPosition)
                                            : pointCount * sizeof(EvaluationPosition);
                auto const argumentSize = functionMode
                                            ? std::max<std::size_t>(
                                                sizeof(float) * 4u, pointCount * argumentComponents * sizeof(float))
                                            : 0u;
                auto const outputSize = pointCount * outputComponents * sizeof(float);
                if (positionSize > MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE ||
                    outputSize > MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE ||
                    argumentSize > MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE ||
                    parameterSize > MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE)
                {
                    throw std::length_error(
                      "WebGPU SDF evaluation exceeds the minimum storage-buffer binding limit");
                }

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

                if (functionMode)
                {
                    wgpu::BufferDescriptor argumentDescriptor;
                    argumentDescriptor.label = "Gladius function arguments";
                    argumentDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                    argumentDescriptor.size = argumentSize;
                    m_arguments = device.CreateBuffer(&argumentDescriptor);
                    if (!m_arguments)
                    {
                        throw std::runtime_error("Unable to allocate WebGPU function argument buffers");
                    }
                }

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
                m_argumentSize = argumentSize;
                createMeshPayloadBuffers(device);
                createBeamPayloadBuffers(device);
                createImagePayloadBuffers(device);
            }

            /// Prepare payloads without holding the shared device gate.
            void preparePayloads(std::vector<std::vector<float>> const & meshPayloadTable,
                                 std::vector<std::vector<float>> const & beamPayloadTable,
                                 std::vector<std::vector<std::uint32_t>> const & imagePayloadTable)
            {
                prepareMeshPayloads(meshPayloadTable);
                prepareBeamPayloads(beamPayloadTable);
                prepareImagePayloads(imagePayloadTable);
            }

            /// Release Dawn handles inside the owner's device transaction, before CPU storage destruction.
            void release()
            {
                m_uniform = nullptr;
                m_positions = nullptr;
                m_arguments = nullptr;
                m_output = nullptr;
                m_staging = nullptr;
                m_parameters = nullptr;
                m_meshPayload = nullptr;
                m_meshOffsetTable = nullptr;
                m_beamPayload = nullptr;
                m_beamOffsetTable = nullptr;
                m_imagePayload = nullptr;
                m_imageOffsetTable = nullptr;
            }

            /// Concatenate image stack payloads plus a per-resource offset table.
            void prepareImagePayloads(std::vector<std::vector<std::uint32_t>> const & imagePayloadTable)
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
                    return;
                }
                if (totalWords > MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE / sizeof(std::uint32_t) ||
                    offsetTable.size() > MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE / sizeof(std::uint32_t))
                {
                    throw std::length_error(
                      "WebGPU image payload exceeds the minimum storage-buffer binding limit");
                }

                m_imagePayloadData.clear();
                m_imagePayloadData.reserve(totalWords);
                for (auto const & payload : imagePayloadTable)
                {
                    m_imagePayloadData.insert(m_imagePayloadData.end(), payload.begin(), payload.end());
                }
                m_imageOffsetTableData = std::move(offsetTable);
            }

            void createImagePayloadBuffers(wgpu::Device const & device)
            {
                if (m_imagePayloadData.empty())
                {
                    return;
                }
                wgpu::BufferDescriptor payloadDescriptor;
                payloadDescriptor.label = "Gladius SDF evaluation image payloads";
                payloadDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                payloadDescriptor.size = m_imagePayloadData.size() * sizeof(std::uint32_t);
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

            /// Concatenate beam lattice payloads plus a per-resource offset table.
            void prepareBeamPayloads(std::vector<std::vector<float>> const & beamPayloadTable)
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
                if (totalFloats > MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE / sizeof(float) ||
                    offsetTable.size() > MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE / sizeof(std::uint32_t))
                {
                    throw std::length_error(
                      "WebGPU beam payload exceeds the minimum storage-buffer binding limit");
                }

                m_beamPayloadData.clear();
                m_beamPayloadData.reserve(totalFloats);
                for (auto const & payload : beamPayloadTable)
                {
                    m_beamPayloadData.insert(m_beamPayloadData.end(), payload.begin(), payload.end());
                }
                m_beamOffsetTableData = std::move(offsetTable);
            }

            void createBeamPayloadBuffers(wgpu::Device const & device)
            {
                if (m_beamPayloadData.empty())
                {
                    return;
                }
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

            /// Concatenate mesh payloads plus a per-resource offset table.
            void prepareMeshPayloads(std::vector<std::vector<float>> const & meshPayloadTable)
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
                if (totalFloats > MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE / sizeof(float) ||
                    offsetTable.size() > MAX_WEBGPU_STORAGE_BUFFER_BINDING_SIZE /
                                           sizeof(std::uint32_t))
                {
                    throw std::length_error(
                      "WebGPU mesh payload exceeds the minimum storage-buffer binding limit");
                }

                m_meshPayloadData.clear();
                m_meshPayloadData.reserve(totalFloats);
                for (auto const & payload : meshPayloadTable)
                {
                    m_meshPayloadData.insert(m_meshPayloadData.end(), payload.begin(), payload.end());
                }
                m_meshOffsetTableData = std::move(offsetTable);
            }

            void createMeshPayloadBuffers(wgpu::Device const & device)
            {
                if (m_meshPayloadData.empty())
                {
                    return;
                }
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
            [[nodiscard]] std::vector<std::uint32_t> const & imagePayloadData() const noexcept
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

                writeParameters(queue, parameters);
            }

            void writeArguments(wgpu::Queue const & queue,
                                EvaluationUniforms const & uniforms,
                                std::vector<float> const & arguments,
                                std::vector<float> const & parameters) const
            {
                queue.WriteBuffer(m_uniform, 0u, &uniforms, sizeof(uniforms));
                queue.WriteBuffer(m_arguments, 0u, arguments.data(), arguments.size() * sizeof(float));

                writeParameters(queue, parameters);
            }

            [[nodiscard]] std::size_t outputSize() const noexcept { return m_outputSize; }
            [[nodiscard]] std::size_t argumentSize() const noexcept { return m_argumentSize; }
            [[nodiscard]] std::size_t parameterSize() const noexcept { return m_parameterSize; }
            [[nodiscard]] wgpu::Buffer const & uniform() const noexcept { return m_uniform; }
            [[nodiscard]] wgpu::Buffer const & positions() const noexcept { return m_positions; }
            [[nodiscard]] wgpu::Buffer const & arguments() const noexcept { return m_arguments; }
            [[nodiscard]] wgpu::Buffer const & output() const noexcept { return m_output; }
            [[nodiscard]] wgpu::Buffer const & staging() const noexcept { return m_staging; }
            [[nodiscard]] wgpu::Buffer const & parameters() const noexcept { return m_parameters; }

          private:
            void writeParameters(wgpu::Queue const & queue, std::vector<float> const & parameters) const
            {
                float const zero{};
                auto const * data = parameters.empty() ? &zero : parameters.data();
                auto const size = parameters.empty() ? sizeof(zero) : parameters.size() * sizeof(float);
                queue.WriteBuffer(m_parameters, 0u, data, size);
            }
            std::size_t m_outputSize{};
            std::size_t m_argumentSize{};
            std::size_t m_parameterSize{};
            wgpu::Buffer m_uniform;
            wgpu::Buffer m_positions;
            wgpu::Buffer m_arguments;
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
            std::vector<std::uint32_t> m_imagePayloadData;
            std::vector<std::uint32_t> m_imageOffsetTableData;
        };

        struct EvaluationResources
        {
            explicit EvaluationResources(std::shared_ptr<WebGPUComputeContext> computeContext)
                : context(std::move(computeContext))
            {
            }

            ~EvaluationResources()
            {
                WebGPUComputeContext::DeviceLock const lock(*context);
                bindGroup = nullptr;
                pipeline = nullptr;
                buffers.release();
            }

            std::shared_ptr<WebGPUComputeContext> context;
            EvaluationBuffers buffers;
            wgpu::ComputePipeline pipeline;
            wgpu::BindGroup bindGroup;
        };

        struct ReadbackState
        {
            explicit ReadbackState(std::shared_ptr<WebGPUComputeContext> computeContext)
                : context(std::move(computeContext))
            {
            }

            ~ReadbackState()
            {
                WebGPUComputeContext::DeviceLock const lock(*context);
                staging = nullptr;
            }

            std::shared_ptr<WebGPUComputeContext> context;
            wgpu::Buffer staging;
            std::vector<float> values;
            std::string errorMessage;
            std::atomic<bool> complete{false};
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

    std::string WebGPUSdfEvaluator::getErrorMessage() const
    {
        return m_context ? m_context->getErrorMessage() : std::string{};
    }

    compute::SdfEvaluationResult WebGPUSdfEvaluator::evaluate(
      compute::SdfEvaluationRequest request) const
    {
        if (request.positions.empty())
        {
            return {};
        }
        if (request.returnColor)
        {
            throw std::invalid_argument("Color evaluation requires evaluateColorsGenerated");
        }

        auto inputPositions = std::move(request.positions);
        compute::SdfEvaluationResult result;
        result.values.resize(inputPositions.size());
        if (!evaluateGenerated(
              request,
              inputPositions.size(),
              [positions = std::move(inputPositions)](
                std::size_t const offset,
                std::size_t const count,
                std::vector<std::array<float, 3>> & output)
              {
                  output.assign(positions.begin() + static_cast<std::ptrdiff_t>(offset),
                                positions.begin() + static_cast<std::ptrdiff_t>(offset + count));
              },
              [&result](std::size_t const offset, std::vector<float> && values)
              {
                  std::copy(values.begin(),
                            values.end(),
                            result.values.begin() + static_cast<std::ptrdiff_t>(offset));
              }))
        {
            throw std::runtime_error("WebGPU SDF evaluation was cancelled");
        }
        return result;
    }

    bool WebGPUSdfEvaluator::evaluateGenerated(compute::SdfEvaluationRequest const& request,
                                               std::size_t const pointCount,
                                               PositionBatchSource positionSource,
                                               ValueBatchSink valueSink,
                                               CancellationPredicate shouldCancel) const
    {
#ifdef __EMSCRIPTEN__
        (void)request;
        (void)pointCount;
        (void)positionSource;
        (void)valueSink;
        (void)shouldCancel;
        throw makeBrowserBlockingUnavailableError("WebGPU evaluation");
#else
        return compute::runBlocking(evaluateGeneratedAsync(request,
                                                           pointCount,
                                                           std::move(positionSource),
                                                           std::move(valueSink),
                                                           std::move(shouldCancel)),
                                    [this] { m_context->processEvents(); });
#endif
    }

    compute::AsyncTask<bool> WebGPUSdfEvaluator::evaluateGeneratedAsync(
      compute::SdfEvaluationRequest request,
      std::size_t const pointCount,
      PositionBatchSource positionSource,
      ValueBatchSink valueSink,
      CancellationPredicate shouldCancel) const
    {
        if (request.returnColor)
        {
            throw std::invalid_argument("Color evaluation requires evaluateColorsGenerated");
        }
        return evaluateGeneratedImpl(std::move(request),
                                     pointCount,
                                     1u,
                                     std::move(positionSource),
                                     std::move(valueSink),
                                     std::move(shouldCancel));
    }

    bool WebGPUSdfEvaluator::evaluateColorsGenerated(
      compute::SdfEvaluationRequest const & request,
      std::size_t const pointCount,
      PositionBatchSource positionSource,
      ColorBatchSink colorSink,
      CancellationPredicate shouldCancel) const
    {
#ifdef __EMSCRIPTEN__
        (void)request;
        (void)pointCount;
        (void)positionSource;
        (void)colorSink;
        (void)shouldCancel;
        throw makeBrowserBlockingUnavailableError("WebGPU evaluation");
#else
        return compute::runBlocking(evaluateColorsGeneratedAsync(request,
                                                                 pointCount,
                                                                 std::move(positionSource),
                                                                 std::move(colorSink),
                                                                 std::move(shouldCancel)),
                                    [this] { m_context->processEvents(); });
#endif
    }

    compute::AsyncTask<bool> WebGPUSdfEvaluator::evaluateColorsGeneratedAsync(
      compute::SdfEvaluationRequest request,
      std::size_t const pointCount,
      PositionBatchSource positionSource,
      ColorBatchSink colorSink,
      CancellationPredicate shouldCancel) const
    {
        if (!request.returnColor)
        {
            throw std::invalid_argument("Color evaluation request must enable returnColor");
        }
        if (!colorSink)
        {
            throw std::invalid_argument("Generated WebGPU color sink must be provided");
        }

        return evaluateGeneratedImpl(
          std::move(request),
          pointCount,
          4u,
          std::move(positionSource),
          [sink = std::move(colorSink)](std::size_t const offset, std::vector<float> && values)
          {
              if (values.size() % 4u != 0u)
              {
                  throw std::runtime_error("WebGPU color evaluation returned an invalid batch size");
              }
              std::vector<std::array<float, 3>> colors(values.size() / 4u);
              for (std::size_t index = 0u; index < colors.size(); ++index)
              {
                  colors[index] = {values[index * 4u], values[index * 4u + 1u], values[index * 4u + 2u]};
              }
              sink(offset, std::move(colors));
          },
          std::move(shouldCancel));
    }

    compute::AsyncTask<bool> WebGPUSdfEvaluator::evaluateGeneratedImpl(
      compute::SdfEvaluationRequest request,
      std::size_t const pointCount,
      std::size_t const outputComponents,
      PositionBatchSource positionSource,
      RawValueBatchSink valueSink,
      CancellationPredicate shouldCancel) const
    {
        return evaluateGeneratedImpl(std::move(request),
                                     pointCount,
                                     outputComponents,
                                     0u,
                                     std::move(positionSource),
                                     {},
                                     std::move(valueSink),
                                     std::move(shouldCancel));
    }

    compute::FunctionEvaluationResult WebGPUSdfEvaluator::evaluateFunction(
      compute::FunctionEvaluationRequest request) const
    {
        auto const argumentComponents = request.argumentComponents;
        auto const outputComponents = request.outputComponents;
        auto const sampleCount =
          argumentComponents != 0u ? request.arguments.size() / argumentComponents : 0u;
        compute::FunctionEvaluationResult result;
        if (sampleCount == 0u)
        {
            return result;
        }
        result.values.resize(sampleCount * outputComponents);
        auto task = evaluateRecordImpl(
          std::move(request),
          sampleCount,
          {},
          [&result, outputComponents](std::size_t const sampleOffset, std::vector<float> && values)
          {
              auto const first = static_cast<std::ptrdiff_t>(sampleOffset * outputComponents);
              std::copy(values.begin(),
                        values.end(),
                        result.values.begin() + first);
          },
          {});
#ifdef __EMSCRIPTEN__
        (void)task;
        throw makeBrowserBlockingUnavailableError("WebGPU evaluation");
#else
        if (!compute::runBlocking(std::move(task), [this] { m_context->processEvents(); }))
        {
            throw std::runtime_error("WebGPU function evaluation was cancelled");
        }
#endif
        return result;
    }

    compute::AsyncTask<bool> WebGPUSdfEvaluator::evaluateRecordImpl(
      compute::FunctionEvaluationRequest request,
      std::size_t const sampleCount,
      ArgumentBatchSource argumentSource,
      RawValueBatchSink valueSink,
      CancellationPredicate shouldCancel) const
    {
        if (request.argumentComponents == 0u || request.outputComponents == 0u)
        {
            throw std::invalid_argument("Function evaluation requires a non-empty argument and output layout");
        }

        compute::SdfEvaluationRequest converted;
        converted.shaderSource = std::move(request.shaderSource);
        converted.parameterValues = std::move(request.parameterValues);
        converted.meshPayloadTable = std::move(request.meshPayloadTable);
        converted.beamPayloadTable = std::move(request.beamPayloadTable);
        converted.imagePayloadTable = std::move(request.imagePayloadTable);

        std::vector<float> const arguments = std::move(request.arguments);
        auto const argumentComponents = request.argumentComponents;
        auto const outputComponents = request.outputComponents;

        ArgumentBatchSource batchSource = std::move(argumentSource);
        if (!batchSource)
        {
            batchSource = [arguments, argumentComponents](std::size_t const offset,
                                                          std::size_t const count,
                                                          std::vector<float> & output)
            {
                auto const first = static_cast<std::ptrdiff_t>(offset * argumentComponents);
                output.assign(arguments.begin() + first,
                              arguments.begin() + first +
                                static_cast<std::ptrdiff_t>(count * argumentComponents));
            };
        }

        co_return co_await evaluateGeneratedImpl(
          std::move(converted),
          sampleCount,
          outputComponents,
          argumentComponents,
          {},
          std::move(batchSource),
          std::move(valueSink),
          std::move(shouldCancel));
    }

    compute::AsyncTask<bool> WebGPUSdfEvaluator::evaluateGeneratedImpl(
      compute::SdfEvaluationRequest request,
      std::size_t const pointCount,
      std::size_t const outputComponents,
      std::size_t const argumentComponents,
      PositionBatchSource positionSource,
      ArgumentBatchSource argumentSource,
      RawValueBatchSink valueSink,
      CancellationPredicate shouldCancel) const
    {
        if (pointCount == 0u)
        {
            co_return true;
        }
        if (shouldCancel && shouldCancel())
        {
            co_return false;
        }
        if (!isAvailable())
        {
            throw std::runtime_error("WebGPU compute context is unavailable");
        }
        if (request.shaderSource.empty())
        {
            throw std::invalid_argument("WebGPU SDF evaluator shader source must not be empty");
        }
        if (!request.positions.empty())
        {
            throw std::invalid_argument(
              "Generated WebGPU SDF evaluation request must not contain materialized positions");
        }
        bool const recordMode = argumentComponents != 0u;
        if (!valueSink || (recordMode ? !argumentSource : !positionSource))
        {
            throw std::invalid_argument("Generated WebGPU SDF evaluation callbacks must be provided");
        }
        if (pointCount > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            throw std::invalid_argument("WebGPU SDF evaluation point count is too large");
        }
        auto const batchCapacity = std::min(pointCount, MAX_BATCH_POINTS);
        std::vector<std::array<float, 3>> generatedPositions;
        generatedPositions.reserve(batchCapacity);
        std::vector<EvaluationPosition> positions;
        positions.reserve(batchCapacity);
        std::vector<float> arguments;
        if (recordMode)
        {
            arguments.reserve(batchCapacity * argumentComponents);
        }

        EvaluationResources resources(m_context);
        auto & buffers = resources.buffers;
        buffers.preparePayloads(request.meshPayloadTable,
                                request.beamPayloadTable,
                                request.imagePayloadTable);
        if (shouldCancel && shouldCancel())
        {
            co_return false;
        }
        struct ValidationScopeState
        {
            std::atomic<bool> done{false};
            wgpu::ErrorType type{wgpu::ErrorType::NoError};
            std::string message;
        };
        auto validationState = std::make_shared<ValidationScopeState>();
        wgpu::ShaderModule shader{};
        // Setup is one synchronous device transaction; no lock survives into batching.
        auto const prepareDevice = [&]
        {
            WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
            buffers.create(m_context->getDevice(),
                           batchCapacity,
                           request.parameterValues.size(),
                           outputComponents,
                           argumentComponents);

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
                                                  buffers.imagePayloadData().size() * sizeof(std::uint32_t));
                m_context->getQueue().WriteBuffer(buffers.imageOffsetTable(),
                                                  0u,
                                                  buffers.imageOffsetTableData().data(),
                                                  buffers.imageOffsetTableData().size() * sizeof(std::uint32_t));
            }

            auto const device = m_context->getDevice();
            device.PushErrorScope(wgpu::ErrorFilter::Validation);

            wgpu::ShaderSourceWGSL wgsl;
            wgsl.code = {request.shaderSource.data(), request.shaderSource.size()};
            wgpu::ShaderModuleDescriptor shaderDescriptor;
            shaderDescriptor.nextInChain = &wgsl;
            shader = device.CreateShaderModule(&shaderDescriptor);

            wgpu::BindGroupLayoutEntry bindings[4]{};
            bindings[0].binding = 0u;
            bindings[0].visibility = wgpu::ShaderStage::Compute;
            bindings[0].buffer.type = wgpu::BufferBindingType::Uniform;
            bindings[0].buffer.minBindingSize = sizeof(EvaluationUniforms);
            bindings[1].binding = 1u;
            bindings[1].visibility = wgpu::ShaderStage::Compute;
            bindings[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
            bindings[1].buffer.minBindingSize =
              recordMode ? buffers.argumentSize() : batchCapacity * sizeof(EvaluationPosition);
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
            resources.pipeline = device.CreateComputePipeline(&pipelineDescriptor);

            // Bind group creation completes the submission that closes the error scope.
            wgpu::BindGroupEntry bindGroupEntries[4]{};
            bindGroupEntries[0].binding = 0u;
            bindGroupEntries[0].buffer = buffers.uniform();
            bindGroupEntries[0].size = sizeof(EvaluationUniforms);
            bindGroupEntries[1].binding = 1u;
            bindGroupEntries[1].buffer = recordMode ? buffers.arguments() : buffers.positions();
            bindGroupEntries[1].size =
              recordMode ? buffers.argumentSize() : batchCapacity * sizeof(EvaluationPosition);
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
                payloadEntry.size = buffers.imagePayloadData().size() * sizeof(std::uint32_t);
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
            resources.bindGroup = m_context->getDevice().CreateBindGroup(&bindGroupDescriptor);

            device.PopErrorScope(
#ifdef __EMSCRIPTEN__
              wgpu::CallbackMode::AllowSpontaneous,
#else
              wgpu::CallbackMode::AllowProcessEvents,
#endif
              [validationState](wgpu::PopErrorScopeStatus const status,
                                wgpu::ErrorType const type,
                                wgpu::StringView const message)
              {
                  if (status != wgpu::PopErrorScopeStatus::Success)
                  {
                      validationState->type = wgpu::ErrorType::Unknown;
                      validationState->message = "Failed to pop WebGPU validation error scope";
                  }
                  else
                  {
                      validationState->type = type;
                      if (message.data != nullptr)
                      {
                          validationState->message =
                            message.length == wgpu::kStrlen
                              ? std::string(message.data)
                              : std::string(message.data, message.length);
                      }
                  }
                  validationState->done.store(true, std::memory_order_release);
              });
        };
        prepareDevice();

        // The pop callback only runs from the host event loop; never spin waiting for it.
        co_await compute::WaitUntil([validationState, context = m_context]
        {
            return validationState->done.load(std::memory_order_acquire) || !context->isValid();
        });

        if (validationState->type != wgpu::ErrorType::NoError || !shader || !resources.pipeline)
        {
            throw std::runtime_error(
              validationState->message.empty()
                ? "WebGPU shader or compute pipeline creation failed"
                : validationState->message);
        }

        for (std::size_t valueOffset = 0u; valueOffset < pointCount;)
        {
            if (shouldCancel && shouldCancel())
            {
                co_return false;
            }
            auto const batchCount = std::min(batchCapacity, pointCount - valueOffset);
            if (recordMode)
            {
                arguments.clear();
                argumentSource(valueOffset, batchCount, arguments);
                if (arguments.size() != batchCount * argumentComponents)
                {
                    throw std::runtime_error(
                      "WebGPU function argument source returned an unexpected batch size");
                }
            }
            else
            {
                generatedPositions.clear();
                positionSource(valueOffset, batchCount, generatedPositions);
                if (generatedPositions.size() != batchCount)
                {
                    throw std::runtime_error(
                      "WebGPU SDF position source returned an unexpected batch size");
                }

                positions.clear();
                for (auto const & position : generatedPositions)
                {
                    if (!std::isfinite(position[0]) || !std::isfinite(position[1]) ||
                        !std::isfinite(position[2]))
                    {
                        throw std::invalid_argument(
                          "WebGPU SDF position source returned a non-finite coordinate");
                    }
                    positions.push_back(
                      EvaluationPosition{{position[0], position[1], position[2], 0.0f}});
                }
            }

            auto const batchPointCount = static_cast<std::uint32_t>(batchCount);
            auto const dispatchPlan = makeSdfEvaluationDispatchPlan(batchPointCount);
            auto const batchOutputSize = batchCount * outputComponents * sizeof(float);
            auto readback = std::make_shared<ReadbackState>(m_context);
            readback->values.resize(batchCount * outputComponents);
            if (shouldCancel && shouldCancel())
            {
                co_return false;
            }
            {
                WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
                if (recordMode)
                {
                    buffers.writeArguments(m_context->getQueue(),
                                          EvaluationUniforms{0.0f,
                                                             batchPointCount,
                                                             dispatchPlan.workgroupsX,
                                                             0u},
                                          arguments,
                                          request.parameterValues);
                }
                else
                {
                    buffers.write(m_context->getQueue(),
                                  EvaluationUniforms{request.isoValue,
                                                     batchPointCount,
                                                     dispatchPlan.workgroupsX,
                                                     0u},
                                  positions,
                                  request.parameterValues);
                }

                auto const encoder = m_context->getDevice().CreateCommandEncoder();
                auto const computePass = encoder.BeginComputePass();
                computePass.SetPipeline(resources.pipeline);
                computePass.SetBindGroup(0u, resources.bindGroup);
                computePass.DispatchWorkgroups(
                  dispatchPlan.workgroupsX, dispatchPlan.workgroupsY, 1u);
                computePass.End();
                encoder.CopyBufferToBuffer(buffers.output(),
                                           0u,
                                           buffers.staging(),
                                           0u,
                                           batchOutputSize);
                auto const commandBuffer = encoder.Finish();
                m_context->getQueue().Submit(1u, &commandBuffer);

                // Keep the context and staging handle alive even if the suspended task is abandoned.
                readback->staging = buffers.staging();
                // Browser callbacks arrive from the JS event loop; native drivers pump them.
                readback->staging.MapAsync(
                  wgpu::MapMode::Read,
                  0u,
                  batchOutputSize,
#ifdef __EMSCRIPTEN__
                  wgpu::CallbackMode::AllowSpontaneous,
#else
                  wgpu::CallbackMode::AllowProcessEvents,
#endif
                  [readback, batchOutputSize](wgpu::MapAsyncStatus const status,
                                             wgpu::StringView const message)
                  {
                      WebGPUComputeContext::DeviceLock const callbackLock(*readback->context);
                      (void)message;
                      if (status != wgpu::MapAsyncStatus::Success)
                      {
                          readback->errorMessage = "WebGPU SDF evaluation readback failed";
                          readback->complete.store(true, std::memory_order_release);
                          return;
                      }

                      auto const * mappedValues = static_cast<float const *>(
                        readback->staging.GetConstMappedRange(0u, batchOutputSize));
                      if (mappedValues == nullptr)
                      {
                          readback->errorMessage = "WebGPU SDF evaluation staging buffer returned no mapped data";
                          readback->staging.Unmap();
                          readback->complete.store(true, std::memory_order_release);
                          return;
                      }

                      std::memcpy(readback->values.data(), mappedValues, batchOutputSize);
                      readback->staging.Unmap();
                      readback->complete.store(true, std::memory_order_release);
                  });
            }

            co_await compute::WaitUntil([readback]
            {
                return readback->complete.load(std::memory_order_acquire) || !readback->context->isValid();
            });
            if (!m_context->isValid())
            {
                WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
                throw std::runtime_error(m_context->getErrorMessage());
            }
            if (!readback->errorMessage.empty())
            {
                throw std::runtime_error(readback->errorMessage);
            }
            if (shouldCancel && shouldCancel())
            {
                co_return false;
            }

            valueSink(valueOffset, std::move(readback->values));
            valueOffset += batchCount;
            if (valueOffset < pointCount)
            {
                co_await compute::AsyncYield{};
            }
        }
        co_return true;
    }
}
