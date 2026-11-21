/**
 * @file FunctionEvaluationSupport.cpp
 * @brief Implementation of the backend-neutral evaluate_function helpers
 */

#include "FunctionEvaluationSupport.h"

#include "../../nodes/Model.h"
#include "../../nodes/Parameter.h"
#include "../../nodes/Port.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <utility>

namespace gladius::mcp::tools
{
    namespace
    {
        /// @brief Order keys used to match the argument order of the generated signature.
        struct SortableInput
        {
            int sortIndex;
            std::string name;
        };
    }

    int functionValueComponentCount(std::type_index const typeIndex) noexcept
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

    std::string functionValueUserTypeName(std::type_index const typeIndex)
    {
        if (typeIndex == nodes::ParameterTypeIndex::Float3)
        {
            return "vec3";
        }
        if (typeIndex == nodes::ParameterTypeIndex::Matrix4)
        {
            return "mat4";
        }
        return "float";
    }

    std::vector<FunctionInputInfo> collectFunctionInputs(nodes::Model & model)
    {
        auto const & rawInputs = model.getInputs();
        std::vector<SortableInput> sortable;
        sortable.reserve(rawInputs.size());
        for (auto const & [name, port] : rawInputs)
        {
            sortable.push_back({port.getSortIndex(), name});
        }
        std::sort(sortable.begin(),
                  sortable.end(),
                  [](SortableInput const & a, SortableInput const & b)
                  {
                      if (a.sortIndex != b.sortIndex)
                      {
                          return a.sortIndex < b.sortIndex;
                      }
                      return a.name < b.name;
                  });

        std::vector<FunctionInputInfo> inputs;
        inputs.reserve(sortable.size());
        for (auto const & entry : sortable)
        {
            auto const & port = rawInputs.at(entry.name);
            auto const typeIndex = port.getTypeIndex();
            inputs.push_back({std::string(port.getUniqueName()),
                              std::string(port.getShortName().empty() ? entry.name
                                                                      : port.getShortName()),
                              functionValueComponentCount(typeIndex),
                              typeIndex});
        }
        return inputs;
    }

    std::vector<FunctionOutputInfo> collectFunctionOutputs(nodes::Model & model)
    {
        std::vector<FunctionOutputInfo> outputs;
        for (auto const & [name, parameter] : model.getOutputs())
        {
            auto const typeIndex = parameter.getTypeIndex();
            outputs.push_back({name, functionValueComponentCount(typeIndex), typeIndex});
        }
        return outputs;
    }

    nlohmann::json flattenFunctionSamples(nlohmann::json const & samples,
                                          std::vector<FunctionInputInfo> const & inputs,
                                          int const totalInputComponents,
                                          std::vector<float> & inputBuffer)
    {
        using json = nlohmann::json;
        if (!samples.is_array() || samples.empty())
        {
            return {{"error", "'samples' must be a non-empty array."}};
        }

        auto const sampleCount = samples.size();
        if (sampleCount > 1000)
        {
            return {{"error", "Maximum 1000 sample points allowed."}};
        }

        inputBuffer.reserve(sampleCount * static_cast<std::size_t>(totalInputComponents));

        for (std::size_t si = 0u; si < sampleCount; ++si)
        {
            auto const & sample = samples[si];
            if (!sample.is_object())
            {
                return {{"error",
                         fmt::format("Sample {} must be a JSON object mapping argument "
                                     "names to values.",
                                     si)}};
            }

            for (auto const & input : inputs)
            {
                std::string key;
                if (sample.contains(input.shortName))
                {
                    key = input.shortName;
                }
                else if (sample.contains(input.name))
                {
                    key = input.name;
                }

                if (key.empty())
                {
                    std::string expected;
                    for (auto const & other : inputs)
                    {
                        if (!expected.empty())
                        {
                            expected += ", ";
                        }
                        expected += fmt::format("\"{}\" ({})",
                                                other.shortName,
                                                functionValueUserTypeName(other.type));
                    }
                    return {{"error",
                             fmt::format("Sample {} is missing argument \"{}\". "
                                         "Expected arguments: {}",
                                         si,
                                         input.shortName,
                                         expected)}};
                }

                auto const & val = sample[key];
                if (input.components == 1)
                {
                    if (!val.is_number())
                    {
                        return {{"error",
                                 fmt::format("Sample {} argument \"{}\" must be a number.", si, key)}};
                    }
                    inputBuffer.push_back(val.get<float>());
                    continue;
                }

                if (!val.is_array() || val.size() != static_cast<std::size_t>(input.components))
                {
                    return {{"error",
                             fmt::format("Sample {} argument \"{}\" must be an array "
                                         "of {} numbers.",
                                         si,
                                         key,
                                         input.components)}};
                }
                for (std::size_t c = 0; c < static_cast<std::size_t>(input.components); ++c)
                {
                    if (!val[c].is_number())
                    {
                        return {{"error",
                                 fmt::format("Sample {} argument \"{}\"[{}] must be "
                                             "a number.",
                                             si,
                                             key,
                                             c)}};
                    }
                    inputBuffer.push_back(val[c].get<float>());
                }
            }
        }

        return {};
    }

    nlohmann::json formatFunctionResults(std::vector<float> const & outputData,
                                         std::vector<FunctionOutputInfo> const & outputInfos,
                                         int const totalOutputComponents,
                                         std::size_t const sampleCount,
                                         std::uint32_t const functionId)
    {
        using json = nlohmann::json;
        json result;
        result["success"] = true;
        result["function_id"] = functionId;

        json warnings = json::array();
        json results = json::array();

        auto const isAnomalous = [](float const value)
        {
            return std::isnan(value) || std::isinf(value);
        };
        auto const anomalyMessage = [](float const value)
        {
            return std::isnan(value) ? "NaN result" : "Infinity result";
        };

        bool const singleOutput = outputInfos.size() == 1u;

        for (std::size_t i = 0u; i < sampleCount; ++i)
        {
            auto const sampleBase = i * static_cast<std::size_t>(totalOutputComponents);

            if (singleOutput)
            {
                auto const & out = outputInfos.front();
                if (out.components == 1)
                {
                    float const value = outputData[sampleBase];
                    if (isAnomalous(value))
                    {
                        results.push_back(nullptr);
                        warnings.push_back({{"sample_index", i},
                                            {"message", anomalyMessage(value)}});
                    }
                    else
                    {
                        results.push_back(value);
                    }
                }
                else
                {
                    json sampleResult = json::array();
                    bool hasAnomalies = false;
                    for (int c = 0; c < out.components; ++c)
                    {
                        float const value = outputData[sampleBase + static_cast<std::size_t>(c)];
                        if (isAnomalous(value))
                        {
                            hasAnomalies = true;
                        }
                        sampleResult.push_back(value);
                    }
                    if (hasAnomalies)
                    {
                        results.push_back(nullptr);
                        warnings.push_back({{"sample_index", i},
                                            {"output", out.name},
                                            {"message", "NaN/Infinity in vec3 result"}});
                    }
                    else
                    {
                        results.push_back(std::move(sampleResult));
                    }
                }
            }
            else
            {
                json sampleResult;
                int offset = 0;
                for (auto const & out : outputInfos)
                {
                    if (out.components == 1)
                    {
                        float const value = outputData[sampleBase + static_cast<std::size_t>(offset)];
                        if (isAnomalous(value))
                        {
                            sampleResult[out.name] = nullptr;
                            warnings.push_back({{"sample_index", i},
                                                {"output", out.name},
                                                {"message", anomalyMessage(value)}});
                        }
                        else
                        {
                            sampleResult[out.name] = value;
                        }
                    }
                    else
                    {
                        json values = json::array();
                        bool hasAnomalies = false;
                        for (int c = 0; c < out.components; ++c)
                        {
                            float const value =
                              outputData[sampleBase + static_cast<std::size_t>(offset + c)];
                            if (isAnomalous(value))
                            {
                                hasAnomalies = true;
                            }
                            values.push_back(value);
                        }
                        if (hasAnomalies)
                        {
                            sampleResult[out.name] = nullptr;
                            warnings.push_back({{"sample_index", i},
                                                {"output", out.name},
                                                {"message", "NaN/Infinity in vec3 result"}});
                        }
                        else
                        {
                            sampleResult[out.name] = std::move(values);
                        }
                    }
                    offset += out.components;
                }
                results.push_back(std::move(sampleResult));
            }
        }

        result["results"] = std::move(results);
        if (!warnings.empty())
        {
            result["warnings"] = std::move(warnings);
        }

        if (singleOutput)
        {
            result["output_type"] = functionValueUserTypeName(outputInfos.front().type);
        }
        else
        {
            json outputTypes;
            for (auto const & out : outputInfos)
            {
                outputTypes[out.name] = functionValueUserTypeName(out.type);
            }
            result["output_types"] = std::move(outputTypes);
        }
        return result;
    }
}
