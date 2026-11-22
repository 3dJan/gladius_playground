/**
 * @file FunctionEvaluatorTool.cpp
 * @brief Implementation of FunctionEvaluatorTool — backend-agnostic function evaluation
 */

#include "FunctionEvaluatorTool.h"
#include "FunctionEvaluatorToolWebGPU.h"

#include "../../Application.h"
#include "../../Document.h"
#include "../../compute/AnalyticRenderSceneSnapshotFactory.h"
#include "../../compute/ApplicationComputeRuntime.h"
#include "../../compute/ComputeBackend.h"
#include "../../compute/FunctionEvaluatorSnapshot.h"
#include "../../compute/SdfEvaluation.h"
#include "../../nodes/Assembly.h"
#include "../../nodes/Model.h"
#include "../../nodes/Parameter.h"
#include "../../nodes/Port.h"
#include "../../types.h"
#include "FunctionEvaluationSupport.h"

#ifdef GLADIUS_ENABLE_OPENCL
#include "../../CLProgram.h"
#include "../../ComputeContext.h"
#include "../../ResourceContext.h"
#include "../../compute/ComputeCore.h"
#include "../../compute/GpuKernelAccessGuard.h"
#include "../../compute/ProgramManager.h"
#include "../../compute/RenderPayloadSnapshot.h"
#include "../../nodes/ToOCLVisitor.h"
#endif

#include <fmt/format.h>

#ifdef GLADIUS_ENABLE_OPENCL
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <variant>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#endif

namespace gladius::mcp::tools
{
    namespace
    {
        /// @brief Describe why no compute backend could serve the evaluation request.
        std::string noBackendMessage(Document const & document)
        {
            auto const backend = document.getBackendKind();
            auto const backendName = backend ? std::string(compute::toString(*backend)) : std::string("unknown");
#ifdef GLADIUS_ENABLE_OPENCL
            (void)backendName;
            return "Function evaluation requires an active OpenCL compute core. "
                   "The selected backend (" + (backend ? std::string(compute::toString(*backend)) : std::string("unknown")) +
                   ") does not support this evaluation tool.";
#else
            return "Function evaluation requires an active compute backend with function support. "
                   "No usable WebGPU compute context is available (active backend: " + backendName + ").";
#endif
        }

        /// @brief Validate the shape of the JSON sample array before flattening.
        /// @return Error JSON on failure, empty JSON on success.
        nlohmann::json validateSamples(nlohmann::json const & samples)
        {
            if (!samples.is_array() || samples.empty())
            {
                return {{"error", "'samples' must be a non-empty array."}};
            }
            if (samples.size() > 1000)
            {
                return {{"error", "Maximum 1000 sample points allowed."}};
            }
            return {};
        }

#ifdef GLADIUS_ENABLE_OPENCL
        /// Map a node type_index to the OpenCL type name.
        std::string typeIndexToOclString(std::type_index typeIndex)
        {
            if (typeIndex == nodes::ParameterTypeIndex::Float)
            {
                return "float";
            }
            if (typeIndex == nodes::ParameterTypeIndex::Float3)
            {
                return "float3";
            }
            if (typeIndex == nodes::ParameterTypeIndex::Matrix4)
            {
                return "float16";
            }
            return "float";
        }

        /// @brief Generate the OpenCL eval kernel source code.
        std::string generateEvalKernel(std::vector<FunctionInputInfo> const & inputs,
                                       std::vector<FunctionOutputInfo> const & outputInfos,
                                       int totalInputComponents,
                                       int totalOutputComponents,
                                       std::string const & oclFuncName,
                                       bool isAssembly)
        {
            std::ostringstream kernelSrc;
            kernelSrc << "\n// --- gladius_eval kernel (auto-generated) ---\n";
            kernelSrc << "__kernel void gladius_eval(\n"
                      << "    __global const float* evalInputBuf,\n"
                      << "    __global float* evalOutputBuf,\n"
                      << "    const uint evalSampleCount,\n"
                      << "    PAYLOAD_ARGS)\n"
                      << "{\n"
                      << "    uint const gid = get_global_id(0);\n"
                      << "    if (gid >= evalSampleCount) return;\n\n";

            int bufferOffset = 0;
            for (auto const & inp : inputs)
            {
                if (inp.components == 3)
                {
                    kernelSrc << fmt::format(
                      "    {0} const {1} = vload3(0, evalInputBuf + gid * {2} + {3});\n",
                      typeIndexToOclString(inp.type), inp.name, totalInputComponents, bufferOffset);
                }
                else
                {
                    kernelSrc << fmt::format(
                      "    {0} const {1} = evalInputBuf[gid * {2} + {3}];\n",
                      typeIndexToOclString(inp.type), inp.name, totalInputComponents, bufferOffset);
                }
                bufferOffset += inp.components;
            }

            kernelSrc << "\n";

            if (isAssembly)
            {
                kernelSrc << fmt::format(
                  "    float4 const evalResult4 = model({}, PASS_PAYLOAD_ARGS);\n",
                  inputs.front().name);
                kernelSrc << "    evalOutputBuf[gid] = evalResult4.w;\n";
            }
            else
            {
                for (auto const & out : outputInfos)
                {
                    kernelSrc << fmt::format("    {0} evalOut_{1};\n", typeIndexToOclString(out.type), out.name);
                }

                std::ostringstream callArgs;
                bool first = true;
                for (auto const & inp : inputs)
                {
                    if (!first)
                    {
                        callArgs << ", ";
                    }
                    callArgs << inp.name;
                    first = false;
                }
                for (auto const & out : outputInfos)
                {
                    callArgs << ", &evalOut_" << out.name;
                }
                callArgs << ", PASS_PAYLOAD_ARGS";
                kernelSrc << fmt::format("    {}({});\n", oclFuncName, callArgs.str());

                int outOffset = 0;
                for (auto const & out : outputInfos)
                {
                    if (out.components == 3)
                    {
                        kernelSrc << fmt::format(
                          "    vstore3(evalOut_{0}, 0, evalOutputBuf + gid * {1} + {2});\n",
                          out.name, totalOutputComponents, outOffset);
                    }
                    else
                    {
                        kernelSrc << fmt::format(
                          "    evalOutputBuf[gid * {0} + {1}] = evalOut_{2};\n",
                          totalOutputComponents, outOffset, out.name);
                    }
                    outOffset += out.components;
                }
            }

            kernelSrc << "}\n";
            return kernelSrc.str();
        }

        /// @brief Compile and evaluate the function with the OpenCL compute backend.
        nlohmann::json evaluateFunctionOpenCL(Document & document,
                                              ComputeCore & core,
                                              nodes::Assembly & assembly,
                                              nodes::Model & functionModel,
                                              uint32_t functionId,
                                              bool isAssembly,
                                              std::vector<FunctionInputInfo> const & inputs,
                                              std::vector<FunctionOutputInfo> const & outputInfos,
                                              std::vector<float> const & inputBuffer,
                                              int totalInputComponents,
                                              int totalOutputComponents);
#endif

    } // anonymous namespace

    FunctionEvaluatorTool::FunctionEvaluatorTool(Application * app)
        : MCPToolBase(app)
    {
    }

    nlohmann::json FunctionEvaluatorTool::evaluateFunction(uint32_t functionId,
                                                           nlohmann::json const & samples)
    {
        using json = nlohmann::json;

        if (!validateApplication())
        {
            return createToolError("Application not available");
        }

        auto document = m_application->getCurrentDocument();
        if (!document)
        {
            return createToolError("No active document. Open or create a document first.");
        }

        auto assembly = document->getAssembly();
        if (!assembly)
        {
            return createToolError("Document has no assembly.");
        }

        if (auto const sampleError = validateSamples(samples); !sampleError.empty())
        {
            return createToolError(sampleError["error"].get<std::string>());
        }

        // --- Locate the function model ------------------------------------------------
        auto const assemblyModelId = assembly->getAssemblyModelId();
        bool const isAssembly = (functionId == assemblyModelId);
        nodes::SharedModel functionModel = isAssembly
                                             ? assembly->assemblyModel()
                                             : assembly->findModel(functionId);

        if (!functionModel)
        {
            return createToolError(fmt::format("Function with resource ID {} not found.", functionId));
        }

        // --- Collect input / output metadata ------------------------------------------
        std::vector<FunctionInputInfo> inputs;
        std::vector<FunctionOutputInfo> outputInfos;
        int totalInputComponents = 0;
        int totalOutputComponents = 0;

        if (isAssembly)
        {
            // The assembly model always has a single float3 pos input and a single scalar result.
            inputs.push_back({"pos", "pos", 3, nodes::ParameterTypeIndex::Float3});
            totalInputComponents = 3;
            outputInfos.push_back({"result", 1, nodes::ParameterTypeIndex::Float});
            totalOutputComponents = 1;
        }
        else
        {
            inputs = collectFunctionInputs(*functionModel);
            for (auto const & input : inputs)
            {
                totalInputComponents += input.components;
            }

            outputInfos = collectFunctionOutputs(*functionModel);
            for (auto const & output : outputInfos)
            {
                totalOutputComponents += output.components;
            }
        }

        if (inputs.empty())
        {
            return createToolError("Function has no inputs - nothing to evaluate.");
        }

        if (outputInfos.empty())
        {
            return createToolError("Function has no outputs.");
        }

        // --- Flatten samples into a contiguous float buffer ---------------------------
        std::vector<float> inputBuffer;
        if (auto const flattenError = flattenFunctionSamples(samples, inputs, totalInputComponents, inputBuffer);
            !flattenError.empty())
        {
            return createToolError(flattenError["error"].get<std::string>());
        }

#ifdef GLADIUS_ENABLE_OPENCL
        if (auto const core = document->getCore(); core != nullptr)
        {
            return evaluateFunctionOpenCL(*document, *core, *assembly, *functionModel, functionId,
                                          isAssembly, inputs, outputInfos, inputBuffer,
                                          totalInputComponents, totalOutputComponents);
        }
#endif

#if defined(GLADIUS_ENABLE_WEBGPU)
        if (auto const webgpuResult = evaluateFunctionWebGPU(
              *document, *assembly, functionId, isAssembly, samples.size(),
              outputInfos, inputBuffer, totalInputComponents, totalOutputComponents);
            !webgpuResult.is_null())
        {
            return webgpuResult;
        }
#endif

        return createToolError(noBackendMessage(*document));
    }

#ifdef GLADIUS_ENABLE_OPENCL
    namespace
    {
        /// @brief Compile and evaluate the function with the OpenCL compute backend.
        nlohmann::json evaluateFunctionOpenCL(Document & document,
                                              ComputeCore & core,
                                              nodes::Assembly & assembly,
                                              nodes::Model & functionModel,
                                              uint32_t functionId,
                                              bool isAssembly,
                                              std::vector<FunctionInputInfo> const & inputs,
                                              std::vector<FunctionOutputInfo> const & outputInfos,
                                              std::vector<float> const & inputBuffer,
                                              int totalInputComponents,
                                              int totalOutputComponents)
        {
            auto computeContext = core.getComputeContext();
            if (!computeContext || !computeContext->isValid())
            {
                return MCPToolBase::createToolError(
                  "OpenCL compute context is not available. "
                  "An OpenCL device (GPU or CPU) is required for function evaluation.");
            }

            auto resources = core.getResourceContext();
            if (!resources)
            {
                return MCPToolBase::createToolError("Resource context not available.");
            }

            auto primitives = core.getPrimitives();
            if (!primitives)
            {
                return MCPToolBase::createToolError("Primitive buffers are not available.");
            }

            // --- Build the model source ----------------------------------------------
            // For the assembly we reuse the already compiled flat source. Sub-functions need
            // the full assembly written in standalone mode so that every function definition
            // is callable from the generated eval kernel.
            document.refreshModelBlocking();
            std::string modelSource;
            if (isAssembly)
            {
                modelSource = core.getProgramManager().getModelSource();
            }
            else
            {
                std::stringstream oclStream;
                nodes::ToOclVisitor visitor;
                visitor.setStandaloneMode(true);
                assembly.visitNodes(visitor);
                visitor.write(oclStream);
                modelSource = oclStream.str();
            }

            if (modelSource.empty())
            {
                return MCPToolBase::createToolError(
                  "Model source is empty - the function graph could not be compiled. "
                  "Check that the document contains valid functions.");
            }

            std::string const oclFuncName =
              isAssembly ? std::string("model") : functionModel.getModelName();

            std::string const kernelSource =
              generateEvalKernel(inputs, outputInfos, totalInputComponents, totalOutputComponents,
                                 oclFuncName, isAssembly);
            std::string const fullSource = modelSource + "\n" + kernelSource;

            // --- Compile the eval program --------------------------------------------
            // Kernel source files that make up the static OpenCL library.
            FileNames const evalSourceFiles{
              "arguments.h", "types.h",   "sdf.h",           "sampler.h",     "rendering.h",
              "sdf_generator.h", "mesh_sdf.cl", "sdf.cl",    "rendering.cl",  "sdf_generator.cl"};

            CLProgram evalProgram(computeContext);
            BuildCallBack noCallback;
            try
            {
                evalProgram.buildFromSourceAndLinkWithLib(evalSourceFiles, fullSource, noCallback);
            }
            catch (std::exception const & e)
            {
                return MCPToolBase::createToolError(
                  fmt::format("OpenCL compilation failed: {}\nGenerated eval kernel:\n{}",
                              e.what(), kernelSource));
            }

            if (!evalProgram.isValid())
            {
                auto const dumpPath = std::filesystem::temp_directory_path() /
                                      ("gladius_eval_debug_" + std::to_string(::getpid()) + ".cl");
                std::ofstream(dumpPath) << fullSource;
                return MCPToolBase::createToolError(
                  fmt::format("OpenCL compilation failed. Source dumped to {}. Generated eval kernel:\n{}",
                              dumpPath.string(), kernelSource));
            }

            // --- Allocate buffers ----------------------------------------------------
            auto const sampleCount = static_cast<uint32_t>(inputBuffer.size() / totalInputComponents);
            auto const outputFloatCount =
              static_cast<size_t>(sampleCount) * static_cast<size_t>(totalOutputComponents);

            auto inputBufferPtr = computeContext->createBufferChecked(
              CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
              inputBuffer.size() * sizeof(float),
              const_cast<void *>(static_cast<void const *>(inputBuffer.data())),
              "evalInput");
            auto outputBufferPtr = computeContext->createBufferChecked(
              CL_MEM_WRITE_ONLY, outputFloatCount * sizeof(float), nullptr, "evalOutput");

            auto queue = computeContext->createQueue();

            RenderPayloadSnapshot const payloadSnapshot =
              RenderPayloadSnapshot::capture(resources, *primitives);

            // --- Dispatch ------------------------------------------------------------
            std::vector<float> outputData(outputFloatCount, 0.0f);
            try
            {
                GpuKernelAccessGuard gpuAccess(*computeContext, queue, "gladius_eval",
                                               payloadSnapshot.readAccesses());
                if (!gpuAccess.granted())
                {
                    return MCPToolBase::createToolError(
                      "GPU resources are not available for evaluation.");
                }

                auto evalEvent = evalProgram.runNonBlockingWithWaitList(
                  queue, "gladius_eval", cl::NullRange, cl::NDRange(sampleCount),
                  gpuAccess.waitEvents(),
                  *inputBufferPtr,
                  *outputBufferPtr,
                  cl_uint(sampleCount),
                  resources->getBuildArea(),
                  primitives->primitives.getBuffer(),
                  cl_int(primitives->primitives.getSize()),
                  primitives->data.getBuffer(),
                  cl_int(primitives->data.getSize()),
                  resources->getRenderingSettings(),
                  resources->getPrecompSdfBuffer().getBuffer(),
                  resources->getParameterBuffer().getBuffer(),
                  resources->getCommandBuffer().getBuffer(),
                  cl_int(resources->getCommandBuffer().getData().size()),
                  resources->getPreCompSdfBBox());
                gpuAccess.complete(evalEvent);

                CL_ERROR(queue.finish());
                CL_ERROR(queue.enqueueReadBuffer(*outputBufferPtr, CL_TRUE, 0,
                                                 outputFloatCount * sizeof(float),
                                                 outputData.data()));
            }
            catch (std::exception const & e)
            {
                return MCPToolBase::createToolError(
                  fmt::format("Evaluation failed: {}", e.what()));
            }

            return formatFunctionResults(outputData, outputInfos, totalOutputComponents,
                                         static_cast<std::size_t>(sampleCount), functionId);
        }
    } // anonymous namespace
#endif

} // namespace gladius::mcp::tools
