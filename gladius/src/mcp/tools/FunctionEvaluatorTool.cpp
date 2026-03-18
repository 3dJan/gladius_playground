/**
 * @file FunctionEvaluatorTool.cpp
 * @brief Implementation of FunctionEvaluatorTool — OpenCL-based function evaluation
 */

#include "FunctionEvaluatorTool.h"

#include "../../Application.h"
#include "../../CLProgram.h"
#include "../../Document.h"
#include "../../ResourceContext.h"
#include "../../compute/ComputeCore.h"
#include "../../compute/ProgramManager.h"
#include "../../nodes/Assembly.h"
#include "../../nodes/Model.h"
#include "../../nodes/Parameter.h"
#include "../../nodes/Port.h"
#include "../../nodes/ToOCLVisitor.h"

#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <variant>

namespace gladius::mcp::tools
{
    namespace
    {
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

        /// Return the number of float components for a given type (1 for float, 3 for float3).
        int componentCount(std::type_index typeIndex)
        {
            if (typeIndex == nodes::ParameterTypeIndex::Float3)
            {
                return 3;
            }
            if (typeIndex == nodes::ParameterTypeIndex::Matrix4)
            {
                return 16;
            }
            return 1;
        }

        /// User-facing type name.
        std::string typeIndexToUserString(std::type_index typeIndex)
        {
            if (typeIndex == nodes::ParameterTypeIndex::Float)
            {
                return "float";
            }
            if (typeIndex == nodes::ParameterTypeIndex::Float3)
            {
                return "vec3";
            }
            return "float";
        }

        struct InputInfo
        {
            std::string name;      ///< Unique name in the OpenCL signature.
            std::string oclType;   ///< "float" | "float3"
            int components;        ///< 1 or 3
            std::type_index type;  ///< Original type index.
        };

        struct OutputInfo
        {
            std::string name;
            std::string oclType;
            int components;
            std::type_index type;
        };

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

        // --- Locate the function model ------------------------------------------------
        auto const assemblyModelId = assembly->getAssemblyModelId();
        bool const isAssembly = (functionId == assemblyModelId);
        nodes::SharedModel functionModel;

        if (isAssembly)
        {
            functionModel = assembly->assemblyModel();
        }
        else
        {
            functionModel = assembly->findModel(functionId);
        }

        if (!functionModel)
        {
            return createToolError(
              fmt::format("Function with resource ID {} not found.", functionId));
        }

        // --- Collect input / output metadata ------------------------------------------
        std::vector<InputInfo> inputs;
        int totalInputComponents = 0;

        if (isAssembly)
        {
            // Assembly model always has a single float3 pos input.
            inputs.push_back(
              {"pos", "float3", 3, nodes::ParameterTypeIndex::Float3});
            totalInputComponents = 3;
        }
        else
        {
            for (auto & [name, port] : functionModel->getInputs())
            {
                auto const & typeIdx = port.getTypeIndex();
                int const comps = componentCount(typeIdx);
                inputs.push_back(
                  {std::string(port.getUniqueName()), typeIndexToOclString(typeIdx), comps, typeIdx});
                totalInputComponents += comps;
            }
        }

        if (inputs.empty())
        {
            return createToolError("Function has no inputs — nothing to evaluate.");
        }

        // Determine output type. For assembly model it's always float (the .w component).
        // For sub-functions, take the first output.
        auto const makeOutputMeta = [&]() -> std::variant<OutputInfo, json>
        {
            if (isAssembly)
            {
                return OutputInfo{"result", "float", 1, nodes::ParameterTypeIndex::Float};
            }
            auto & outputs = functionModel->getOutputs();
            if (outputs.empty())
            {
                return createToolError("Function has no outputs.");
            }
            auto const & [oName, oParam] = *outputs.begin();
            return OutputInfo{oName,
                          typeIndexToOclString(oParam.getTypeIndex()),
                          componentCount(oParam.getTypeIndex()),
                          oParam.getTypeIndex()};
        };

        auto outputVariant = makeOutputMeta();
        if (auto * errPtr = std::get_if<json>(&outputVariant))
        {
            return *errPtr;
        }
        auto const & outputMeta = std::get<OutputInfo>(outputVariant);

        // --- Validate and flatten sample data -----------------------------------------
        if (!samples.is_array() || samples.empty())
        {
            return createToolError("'samples' must be a non-empty array.");
        }

        auto const sampleCount = samples.size();
        if (sampleCount > 1000)
        {
            return createToolError("Maximum 1000 sample points allowed.");
        }

        std::vector<float> inputBuffer;
        inputBuffer.reserve(sampleCount * static_cast<size_t>(totalInputComponents));

        for (size_t si = 0; si < sampleCount; ++si)
        {
            auto const & sample = samples[si];
            if (!sample.is_object())
            {
                return createToolError(
                  fmt::format("Sample {} must be a JSON object mapping argument names to values.",
                              si));
            }

            for (auto const & inp : inputs)
            {
                // The JSON key for a given input. Strip any prefix the unique name may carry —
                // clients use the short name (e.g. "pos"), while Model stores the unique name
                // (e.g. "pos" or "input_pos"). Try the unique name first, then fall back to
                // stripping an "input_" prefix.
                std::string key = inp.name;
                if (!sample.contains(key))
                {
                    // Try without "input_" prefix that Model sometimes adds.
                    if (key.substr(0, 6) == "input_")
                    {
                        key = key.substr(6);
                    }
                }
                if (!sample.contains(key))
                {
                    // Also try original if the short name didn't match.
                    key = inp.name;
                }

                if (!sample.contains(key))
                {
                    // Build a helpful error listing expected arguments.
                    std::string expected;
                    for (auto const & i : inputs)
                    {
                        if (!expected.empty())
                        {
                            expected += ", ";
                        }
                        auto shortName = i.name;
                        if (shortName.substr(0, 6) == "input_")
                        {
                            shortName = shortName.substr(6);
                        }
                        expected += fmt::format("\"{}\" ({})", shortName,
                                                typeIndexToUserString(i.type));
                    }
                    return createToolError(
                      fmt::format("Sample {} is missing argument \"{}\". "
                                  "Expected arguments: {}",
                                  si, key, expected));
                }

                auto const & val = sample[key];
                if (inp.components == 1)
                {
                    if (!val.is_number())
                    {
                        return createToolError(
                          fmt::format("Sample {} argument \"{}\" must be a number.", si, key));
                    }
                    inputBuffer.push_back(val.get<float>());
                }
                else if (inp.components == 3)
                {
                    if (!val.is_array() || val.size() != 3)
                    {
                        return createToolError(
                          fmt::format("Sample {} argument \"{}\" must be an array of 3 numbers.",
                                      si, key));
                    }
                    for (int c = 0; c < 3; ++c)
                    {
                        if (!val[c].is_number())
                        {
                            return createToolError(
                              fmt::format("Sample {} argument \"{}\"[{}] must be a number.",
                                          si, key, c));
                        }
                        inputBuffer.push_back(val[c].get<float>());
                    }
                }
            }
        }

        // --- Ensure model is compiled -------------------------------------------------
        document->refreshModelBlocking();

        auto core = document->getCore();
        if (!core)
        {
            return createToolError("Compute core not available.");
        }

        // For assembly (root) evaluation, use the pre-compiled flat assembly source.
        // For sub-function evaluation, generate OCL from the original assembly so
        // individual function definitions (function_N) are available — the flat
        // assembly inlines everything into model() and omits them.
        std::string modelSource;
        if (isAssembly)
        {
            modelSource = core->getProgramManager().getModelSource();
        }
        else
        {
            std::stringstream oclStream;
            nodes::ToOclVisitor visitor;
            visitor.setStandaloneMode(true);
            assembly->visitNodes(visitor);
            visitor.write(oclStream);
            modelSource = oclStream.str();
        }

        if (modelSource.empty())
        {
            return createToolError(
              "Model source is empty — the function graph could not be compiled. "
              "Check that the document contains valid functions.");
        }

        auto computeContext = core->getComputeContext();
        if (!computeContext || !computeContext->isValid())
        {
            return createToolError(
              "OpenCL compute context is not available. "
              "An OpenCL device (GPU or CPU) is required for function evaluation.");
        }

        auto resources = core->getResourceContext();
        if (!resources)
        {
            return createToolError("Resource context not available.");
        }

        // --- Generate eval kernel source ----------------------------------------------
        std::string const oclFuncName =
          isAssembly ? "model" : functionModel->getModelName();

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

        // Unpack inputs from the flat input buffer.
        int bufferOffset = 0;
        for (auto const & inp : inputs)
        {
            if (inp.components == 3)
            {
                kernelSrc << fmt::format(
                  "    {0} const {1} = vload3(0, evalInputBuf + gid * {2} + {3});\n",
                  inp.oclType, inp.name, totalInputComponents, bufferOffset);
            }
            else
            {
                kernelSrc << fmt::format(
                  "    {0} const {1} = evalInputBuf[gid * {2} + {3}];\n",
                  inp.oclType, inp.name, totalInputComponents, bufferOffset);
            }
            bufferOffset += inp.components;
        }

        kernelSrc << "\n";

        // Call the function and write the result.
        if (isAssembly)
        {
            // Assembly: float4 model(float3 pos, PAYLOAD_ARGS) — extract .w for SDF
            kernelSrc << fmt::format(
              "    float4 const evalResult4 = model({}, PASS_PAYLOAD_ARGS);\n",
              inputs.front().name);
            if (outputMeta.components == 1)
            {
                kernelSrc << "    evalOutputBuf[gid] = evalResult4.w;\n";
            }
        }
        else
        {
            // Sub-function: void function_N(inputs..., output*, PAYLOAD_ARGS)
            // Declare output variable.
            kernelSrc << fmt::format("    {0} evalOutVar;\n", outputMeta.oclType);

            // Build call: function_N(inp1, inp2, ..., &evalOutVar, PASS_PAYLOAD_ARGS)
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
            callArgs << ", &evalOutVar, PASS_PAYLOAD_ARGS";
            kernelSrc << fmt::format("    {}({});\n", oclFuncName, callArgs.str());

            // Write output to buffer.
            if (outputMeta.components == 3)
            {
                kernelSrc << "    vstore3(evalOutVar, gid, evalOutputBuf);\n";
            }
            else
            {
                kernelSrc << "    evalOutputBuf[gid] = evalOutVar;\n";
            }
        }

        kernelSrc << "}\n";

        std::string const fullDynamic = modelSource + kernelSrc.str();

        // --- Compile the eval program -------------------------------------------------
        FileNames const sourceFiles = {"arguments.h",
                                       "types.h",
                                       "sdf.h",
                                       "sampler.h",
                                       "rendering.h",
                                       "sdf_generator.h",
                                       "mesh_sdf.cl",
                                       "sdf.cl",
                                       "rendering.cl",
                                       "sdf_generator.cl"};

        CLProgram evalProg(computeContext);
        BuildCallBack noCallback;
        try
        {
            evalProg.buildFromSourceAndLinkWithLib(sourceFiles, fullDynamic, noCallback);
        }
        catch (std::exception const & e)
        {
            return createToolError(
              fmt::format("OpenCL compilation failed: {}", e.what()));
        }

        if (!evalProg.isValid())
        {
            // Dump source to temp file for debugging
            {
                std::ofstream dump("/tmp/gladius_eval_debug.cl");
                dump << fullDynamic;
            }
            return createToolError(
              fmt::format("OpenCL compilation failed. Source dumped to /tmp/gladius_eval_debug.cl. "
                          "Generated eval kernel:\n{}",
                          kernelSrc.str()));
        }

        // --- Create buffers and dispatch ----------------------------------------------
        auto const inputByteSize = inputBuffer.size() * sizeof(float);
        auto const outputFloatCount =
          sampleCount * static_cast<size_t>(outputMeta.components);
        auto const outputByteSize = outputFloatCount * sizeof(float);

        try
        {
            auto clInputBuffer = computeContext->createBufferChecked(
              CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, inputByteSize, inputBuffer.data());

            auto clOutputBuffer =
              computeContext->createBufferChecked(CL_MEM_WRITE_ONLY, outputByteSize);

            auto queue = computeContext->createQueue();

            auto primitives = core->getPrimitives();
            evalProg.run(
              queue,
              "gladius_eval",
              cl::NullRange,
              cl::NDRange(sampleCount),
              *clInputBuffer,
              *clOutputBuffer,
              static_cast<cl_uint>(sampleCount),
              resources->getBuildArea(),
              primitives->primitives.getBuffer(),
              static_cast<cl_int>(primitives->primitives.getSize()),
              primitives->data.getBuffer(),
              static_cast<cl_int>(primitives->data.getSize()),
              resources->getRenderingSettings(),
              resources->getPrecompSdfBuffer().getBuffer(),
              resources->getParameterBuffer().getBuffer(),
              resources->getCommandBuffer().getBuffer(),
              static_cast<cl_int>(resources->getCommandBuffer().getData().size()),
              resources->getPreCompSdfBBox());

            // Read back results.
            std::vector<float> outputData(outputFloatCount);
            queue.enqueueReadBuffer(*clOutputBuffer, CL_TRUE, 0, outputByteSize, outputData.data());

            // Build response.
            json result;
            result["success"] = true;
            result["function_id"] = functionId;

            json warnings = json::array();
            json results = json::array();

            if (outputMeta.components == 1)
            {
                for (size_t i = 0; i < sampleCount; ++i)
                {
                    float const v = outputData[i];
                    if (std::isnan(v) || std::isinf(v))
                    {
                        results.push_back(nullptr);
                        warnings.push_back(
                          {{"sample_index", i},
                           {"message", std::isnan(v) ? "NaN result" : "Infinity result"}});
                    }
                    else
                    {
                        results.push_back(v);
                    }
                }
            }
            else
            {
                // vec3 output: return arrays of 3 floats.
                for (size_t i = 0; i < sampleCount; ++i)
                {
                    auto const base = i * 3;
                    bool hasAnomalies = false;
                    for (int c = 0; c < 3; ++c)
                    {
                        float const v = outputData[base + c];
                        if (std::isnan(v) || std::isinf(v))
                        {
                            hasAnomalies = true;
                        }
                    }
                    if (hasAnomalies)
                    {
                        results.push_back(nullptr);
                        warnings.push_back(
                          {{"sample_index", i}, {"message", "NaN/Infinity in vec3 result"}});
                    }
                    else
                    {
                        results.push_back({outputData[base],
                                           outputData[base + 1],
                                           outputData[base + 2]});
                    }
                }
            }

            result["results"] = std::move(results);
            if (!warnings.empty())
            {
                result["warnings"] = std::move(warnings);
            }

            result["output_type"] = typeIndexToUserString(outputMeta.type);
            return result;
        }
        catch (std::exception const & e)
        {
            return createToolError(
              fmt::format("Evaluation failed: {}", e.what()));
        }
    }

} // namespace gladius::mcp::tools
