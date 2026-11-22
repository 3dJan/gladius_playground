/**
 * @file FunctionEvaluationSupport.h
 * @brief Backend-neutral helpers for the evaluate_function MCP tool
 */

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <typeindex>
#include <vector>

namespace gladius::nodes
{
    class Model;
}

namespace gladius::mcp::tools
{
    /// @brief One argument of the evaluated function, in signature order.
    struct FunctionInputInfo
    {
        std::string name;      ///< Unique name in the generated signature.
        std::string shortName; ///< User-facing short name (e.g. "pos").
        int components{};      ///< 1, 3 or 16 floats.
        std::type_index type;  ///< Original graph type.
    };

    /// @brief One output of the evaluated function, in signature order.
    struct FunctionOutputInfo
    {
        std::string name;
        int components{};
        std::type_index type;
    };

    /// @brief Layout (float count) of a graph value in a flat evaluation record.
    [[nodiscard]] int functionValueComponentCount(std::type_index typeIndex) noexcept;

    /// @brief User-facing type name ("float" or "vec3").
    [[nodiscard]] std::string functionValueUserTypeName(std::type_index typeIndex);

    /// @brief Collect the function arguments in the order the generated signature binds them.
    [[nodiscard]] std::vector<FunctionInputInfo> collectFunctionInputs(nodes::Model & model);

    /// @brief Collect every function output in the order the generated signature returns them.
    [[nodiscard]] std::vector<FunctionOutputInfo> collectFunctionOutputs(nodes::Model & model);

    /**
     * @brief Validate and flatten sample data into a contiguous float buffer.
     * @return Error JSON on failure, empty JSON on success (populates inputBuffer).
     */
    [[nodiscard]] nlohmann::json flattenFunctionSamples(nlohmann::json const & samples,
                                                        std::vector<FunctionInputInfo> const & inputs,
                                                        int totalInputComponents,
                                                        std::vector<float> & inputBuffer);

    /// @brief Format flat evaluation results into the tool response JSON.
    [[nodiscard]] nlohmann::json formatFunctionResults(std::vector<float> const & outputData,
                                                        std::vector<FunctionOutputInfo> const & outputInfos,
                                                        int totalOutputComponents,
                                                        std::size_t sampleCount,
                                                        std::uint32_t functionId);
}
