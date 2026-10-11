#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace gladius::ui::numeric_input_detail
{
    struct NumericExpressionResult
    {
        std::optional<float> value;
        std::string error;
    };

    /// Evaluate a unitless calculator expression, interpreting angles in degrees when requested.
    [[nodiscard]] NumericExpressionResult evaluateNumericExpression(std::string_view expression,
                                                                    bool angleInDegrees);

    /// Format a numeric result using engineering exponents outside the ordinary decimal range.
    [[nodiscard]] std::string formatEngineeringValue(float value);
} // namespace gladius::ui::numeric_input_detail