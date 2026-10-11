#include "NumericExpressionEvaluator.h"

#include <muParser.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

#include <fmt/format.h>

namespace gladius::ui::numeric_input_detail
{
    namespace
    {
        struct FunctionAlias
        {
            std::string_view inputName;
            std::string_view radiansName;
            std::string_view degreesName;
        };

        constexpr std::array<FunctionAlias, 13> FUNCTION_ALIASES = {{
          {"sqrt", "calcSqrt", "calcSqrt"},
          {"pow", "calcPow", "calcPow"},
          {"sin", "calcSinRadians", "calcSinDegrees"},
          {"cos", "calcCosRadians", "calcCosDegrees"},
          {"tan", "calcTanRadians", "calcTanDegrees"},
          {"asin", "calcAsinRadians", "calcAsinDegrees"},
          {"acos", "calcAcosRadians", "calcAcosDegrees"},
          {"atan", "calcAtanRadians", "calcAtanDegrees"},
          {"atan2", "calcAtan2Radians", "calcAtan2Degrees"},
          {"ln", "calcLn", "calcLn"},
          {"log10", "calcLog10", "calcLog10"},
          {"exp", "calcExp", "calcExp"},
          {"abs", "calcAbs", "calcAbs"},
        }};

        void defineCalculatorFunctions(mu::Parser & parser)
        {
            parser.DefineFun("calcSqrt", [](double value) { return std::sqrt(value); });
            parser.DefineFun("calcPow", [](double base, double exponent)
                             { return std::pow(base, exponent); });
            parser.DefineFun("calcSinRadians", [](double value) { return std::sin(value); });
            parser.DefineFun("calcCosRadians", [](double value) { return std::cos(value); });
            parser.DefineFun("calcTanRadians", [](double value) { return std::tan(value); });
            parser.DefineFun("calcAsinRadians", [](double value) { return std::asin(value); });
            parser.DefineFun("calcAcosRadians", [](double value) { return std::acos(value); });
            parser.DefineFun("calcAtanRadians", [](double value) { return std::atan(value); });
            parser.DefineFun("calcAtan2Radians", [](double y, double x)
                             { return std::atan2(y, x); });
            parser.DefineFun("calcSinDegrees", [](double value)
                             { return std::sin(value * std::numbers::pi_v<double> / 180.0); });
            parser.DefineFun("calcCosDegrees", [](double value)
                             { return std::cos(value * std::numbers::pi_v<double> / 180.0); });
            parser.DefineFun("calcTanDegrees", [](double value)
                             { return std::tan(value * std::numbers::pi_v<double> / 180.0); });
            parser.DefineFun("calcAsinDegrees", [](double value)
                             { return std::asin(value) * 180.0 / std::numbers::pi_v<double>; });
            parser.DefineFun("calcAcosDegrees", [](double value)
                             { return std::acos(value) * 180.0 / std::numbers::pi_v<double>; });
            parser.DefineFun("calcAtanDegrees", [](double value)
                             { return std::atan(value) * 180.0 / std::numbers::pi_v<double>; });
            parser.DefineFun("calcAtan2Degrees", [](double y, double x)
                             { return std::atan2(y, x) * 180.0 / std::numbers::pi_v<double>; });
            parser.DefineFun("calcLn", [](double value) { return std::log(value); });
            parser.DefineFun("calcLog10", [](double value) { return std::log10(value); });
            parser.DefineFun("calcExp", [](double value) { return std::exp(value); });
            parser.DefineFun("calcAbs", [](double value) { return std::abs(value); });
        }

        std::optional<std::string> translateExpression(std::string const & expression,
                                                       bool angleInDegrees,
                                                       std::string & error)
        {
            std::string translated;
            translated.reserve(expression.size() + 16);

            for (std::size_t position = 0; position < expression.size();)
            {
                unsigned char const character = static_cast<unsigned char>(expression[position]);
                if (std::isspace(character))
                {
                    translated.push_back(expression[position++]);
                    continue;
                }

                if (std::isdigit(character) || expression[position] == '.')
                {
                    char * end = nullptr;
                    std::strtod(expression.c_str() + position, &end);
                    if (end != expression.c_str() + position)
                    {
                        std::size_t const numberLength =
                          static_cast<std::size_t>(end - (expression.c_str() + position));
                        translated.append(expression, position, numberLength);
                        position += numberLength;
                        continue;
                    }
                }

                if (std::isalpha(character) || expression[position] == '_')
                {
                    std::size_t const identifierStart = position;
                    while (position < expression.size())
                    {
                        unsigned char const current =
                          static_cast<unsigned char>(expression[position]);
                        if (!std::isalnum(current) && expression[position] != '_')
                        {
                            break;
                        }
                        ++position;
                    }

                    std::string const identifier =
                      expression.substr(identifierStart, position - identifierStart);
                    if (identifier == "pi")
                    {
                        translated += "calcPi";
                    }
                    else if (identifier == "tau")
                    {
                        translated += "calcTau";
                    }
                    else if (identifier == "e")
                    {
                        translated += "calcE";
                    }
                    else
                    {
                        auto const function = std::find_if(
                          FUNCTION_ALIASES.begin(),
                          FUNCTION_ALIASES.end(),
                          [&identifier](FunctionAlias const & alias)
                          { return alias.inputName == identifier; });
                        if (function == FUNCTION_ALIASES.end() || function->radiansName.empty())
                        {
                            error = fmt::format("Unknown calculator name '{}'.", identifier);
                            return std::nullopt;
                        }

                        std::size_t lookahead = position;
                        while (lookahead < expression.size() &&
                               std::isspace(static_cast<unsigned char>(expression[lookahead])))
                        {
                            ++lookahead;
                        }
                        if (lookahead >= expression.size() || expression[lookahead] != '(')
                        {
                            error = fmt::format("Function '{}' needs parentheses.", identifier);
                            return std::nullopt;
                        }

                        translated += angleInDegrees ? function->degreesName : function->radiansName;
                    }
                    continue;
                }

                constexpr std::string_view allowedOperators = "+-*/^(),";
                if (allowedOperators.find(expression[position]) == std::string_view::npos)
                {
                    error = fmt::format("Unsupported character '{}'.", expression[position]);
                    return std::nullopt;
                }
                translated.push_back(expression[position++]);
            }

            return translated;
        }

        void defineCalculatorConstants(mu::Parser & parser, bool angleInDegrees)
        {
            double const pi = angleInDegrees ? 180.0 : std::numbers::pi_v<double>;
            parser.DefineConst("calcPi", pi);
            parser.DefineConst("calcTau", angleInDegrees ? 360.0 : 2.0 * pi);
            parser.DefineConst("calcE", std::numbers::e_v<double>);
        }
    } // namespace

    NumericExpressionResult evaluateNumericExpression(std::string_view expression,
                                                      bool angleInDegrees)
    {
        if (expression.empty())
        {
            return {std::nullopt, "Enter an expression."};
        }

        std::string const expressionText{expression};
        std::string translationError;
        auto const translated = translateExpression(expressionText,
                                                    angleInDegrees,
                                                    translationError);
        if (!translated)
        {
            return {std::nullopt, std::move(translationError)};
        }

        try
        {
            mu::Parser parser;
            defineCalculatorFunctions(parser);
            defineCalculatorConstants(parser, angleInDegrees);
            parser.SetExpr(*translated);
            double const result = parser.Eval();
            if (!std::isfinite(result))
            {
                return {std::nullopt, "The result is not finite."};
            }

            double const floatLimit = static_cast<double>(std::numeric_limits<float>::max());
            if (result > floatLimit || result < -floatLimit)
            {
                return {std::nullopt, "The result is outside the supported numeric range."};
            }

            float const floatResult = static_cast<float>(result);
            if (!std::isfinite(floatResult) || (result != 0.0 && floatResult == 0.f))
            {
                return {std::nullopt, "The result is outside the supported numeric range."};
            }
            return {floatResult, {}};
        }
        catch (mu::Parser::exception_type const & error)
        {
            return {std::nullopt, error.GetMsg()};
        }
        catch (std::exception const & error)
        {
            return {std::nullopt, error.what()};
        }
    }

    std::string formatEngineeringValue(float value)
    {
        if (!std::isfinite(value))
        {
            return fmt::format("{:.6g}", value);
        }
        if (value == 0.f)
        {
            return "0";
        }

        double const magnitude = std::abs(static_cast<double>(value));
        if (magnitude >= 1e6 || magnitude < 1e-3)
        {
            int const exponent =
              3 * static_cast<int>(std::floor(std::log10(magnitude) / 3.0));
            double const mantissa = static_cast<double>(value) /
                                    std::pow(10.0, static_cast<double>(exponent));
            return fmt::format("{:.6g}e{:+03}", mantissa, exponent);
        }
        return fmt::format("{:.6g}", value);
    }
} // namespace gladius::ui::numeric_input_detail