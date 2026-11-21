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
#include "../../ResourceContext.h"
#include "../../compute/ComputeCore.h"
#include "../../compute/GpuKernelAccessGuard.h"
#include "../../compute/ProgramManager.h"
#include "../../compute/RenderPayloadSnapshot.h"
#include "../../nodes/ToOCLVisitor.h"
#endif

#include <fmt/format.h>

#ifdef GLADIUS_ENABLE_OPENCL
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
            return evaluateFunctionOpenCL(*document, *core, *assembly, functionId, isAssembly,
                                          *functionModel, inputs, outputInfos, inputBuffer,
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
        nlohmann::json evaluateFunctionOpenCL(
        Document & document,
        ComputeCore & core,
        nodes::Assembly & assembly,
        uint32_t functionId,
        bool isAssembly,
        nodes::Model & functionModel,
        std::vector<FunctionInputInfo> const & inputs,
        std::vector<FunctionOutputInfo> const & outputInfos,
        std::vector<float> const & inputBuffer,
        int totalInputComponents,
        int totalOutputComponents)
    {
        using json = nlohmann::json;

        auto computeContext = core.getComputeContext();
        if (!computeContext)
        {
            return MCPToolBase::createToolError("OpenCL compute context is not available. "
                                   "An OpenCL device (GPU or CPU) is required for function evaluation.");
        }

        // --- Build the model source --------------------------------------------------
        std::string modelSource;
        std::string oclFuncName;

        if (isAssembly)
        {
            document.refreshModelBlocking();
            modelSource = core.getProgramManager().getModelSource();
            oclFuncName = "model";
        }
        else
        {
            // Visitor walks the entire assembly to pull in all function definitions,
            // but we invoke the specific function directly.
            nodes::ToOCLVisitor oclVisitor(assembly);
            oclVisitor.setStandaloneMode(true);
            assembly.visitNodes(oclVisitor);
            oclVisitor.write(modelSource);

            if (modelSource.empty())
            {
                return MCPToolBase::createToolError("Model source is empty - the function graph could not be compiled. "
                                       "Check that the document contains valid functions.");
            }

            oclFuncName = fmt::format("graphFunction_{}", functionId);
        }

        // --- Generate eval kernel ----------------------------------------------------
        std::string const kernelSource = generateEvalKernel(inputs, outputInfos,
                                                            totalInputComponents, totalOutputComponents,
                                                            oclFuncName, isAssembly);

        // --- Compile ------------------------------------------------------------------
        // The kernel is appended directly to modelSource - no separate include needed.
        std::string fullSource = modelSource + "\n" + kernelSource;

        std::vector<std::string> const fileNames{
            "arguments.h", "types.h", "sdf.h", "sampler.h", "rendering.h", "sdf_generator.h",
            "mesh_sdf.cl", "sdf.cl", "rendering.cl", "sdf_generator.cl"};

        try
        {
            core.getProgramManager().buildFromSourceAndLinkWithLib(fileNames, fullSource, nullptr);
        }
        catch (std::exception const & e)
        {
            // Dump the failing source to a temp file for debugging
            auto dumpPath = std::filesystem::temp_directory_path() /
                            ("gladius_eval_debug_" + std::to_string(static_cast<int>(getpid())) + ".cl");
            std::ofstream(dumpPath) << fullSource;
            return MCPToolBase::createToolError(fmt::format("OpenCL compilation failed: {}\n"
                                               "OpenCL compilation failed. Source dumped to {}. Generated eval kernel:\n{}",
                                               e.what(), dumpPath.string(), kernelSource));
        }

        auto program = core.getProgramManager().getProgram();
        if (!program)
        {
            return MCPToolBase::createToolError("Compiled OpenCL program is not available.");
        }

        // --- Create buffers -----------------------------------------------------------
        auto const sampleCount = static_cast<uint32_t>(inputBuffer.size() / totalInputComponents);

        std::vector<float> outputData(static_cast<size_t>(sampleCount) * totalOutputComponents, 0.0f);

        cl::CommandQueue queue = computeContext->getCommandQueue();

        auto inputBufferPtr = computeContext->createBufferChecked(
          CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
          sizeof(float) * inputBuffer.size(),
          inputBuffer.data());

        auto outputBufferPtr = computeContext->createBufferChecked(
          CL_MEM_WRITE_ONLY,
          sizeof(float) * outputData.size(),
          nullptr);

        // --- Build payload bundle ------------------------------------------------------
        // Resource paths (Mesh, Image) are active if the model was compiled with them.
        // We capture all payloads; unused kernel arguments are harmless in OpenCL.
        auto resources = document.getResourceManager();
        auto const & resourceContext = core.getResourceContext();
        auto const * primitives = core.getPrimitiveManagerPtr();

        if (!primitives)
        {
            return MCPToolBase::createToolError("Primitive buffers are not available.");
        }

        compute::RenderPayloadSnapshot const payloadSnapshot =
            compute::RenderPayloadSnapshot::capture(resources, *primitives);
        if (!payloadSnapshot.isValid())
        {
            return MCPToolBase::createToolError("GPU resources are not available for evaluation.");
        }

        // --- Launch kernel -------------------------------------------------------------
        const uint32_t evalSampleCount = sampleCount;
        auto & meshBuffer = payloadSnapshot.meshBuffer;
        auto & beamBuffer = payloadSnapshot.beamBuffer;
        auto & imageBuffer = payloadSnapshot.imageBuffer;
        uint32_t meshCount = payloadSnapshot.meshCount;
        uint32_t beamCount = payloadSnapshot.beamCount;
        uint32_t imageCount = payloadSnapshot.imageCount;
        uint32_t meshSdfCapacity = primitives->getMeshSdfCellCapacity();
        uint32_t meshBeamLatticeCapacity = primitives->getBeamLatticeCellCapacity();
        uint32_t meshMaxFaces = primitives->getMeshMaxFaceCount();
        uint32_t meshMaxVertexIndex = primitives->getMaxVertexIndex();

        try
        {
            compute::GpuKernelAccessGuard gpuAccess(*computeContext, queue, "gladius_eval",
                                                    payloadSnapshot.readAccesses());
            auto evalEvent = program->runNonBlockingWithWaitList(
              queue,
              "gladius_eval",
              cl::NullRange,
              cl::NDRange(sampleCount),
              gpuAccess.waitEvents(),
              *inputBufferPtr,
              *outputBufferPtr,
              evalSampleCount,
              meshBuffer,
              beamBuffer,
              imageBuffer,
              meshCount,
              beamCount,
              imageCount,
              meshSdfCapacity,
              meshBeamLatticeCapacity,
              meshMaxFaces,
              meshMaxVertexIndex);
            gpuAccess.complete(evalEvent);

            // Wait for all work
            CL_ERROR(queue.finish());

            // Readback
            CL_ERROR(queue.enqueueReadBuffer(*outputBufferPtr, CL_TRUE, 0,
                                             sizeof(float) * outputData.size(),
                                             outputData.data()));
        }
        catch (cl::Error const & e)
        {
            return MCPToolBase::createToolError(fmt::format("OpenCL runtime error: {} ({})",
                                               compute::clErrorMessage(e), e.what()));
        }

        return formatFunctionResults(outputData, outputInfos, totalOutputComponents,
                                     static_cast<std::size_t>(sampleCount), functionId);
    }
    } // anonymous namespace
#endif

} // namespace gladius::mcp::tools
