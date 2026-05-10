#include "FunctionArgument.h"
#include <regex>
#include <set>

namespace gladius
{
    bool ArgumentUtils::isValidComponent(std::string const & component)
    {
        return component == "x" || component == "y" || component == "z";
    }

    ComponentAccess ArgumentUtils::parseComponentAccess(std::string const & expression)
    {
        // Pattern: argumentName.component (e.g., "A.x", "position.y")
        std::regex componentPattern(R"(^([A-Za-z_][A-Za-z0-9_]*)\.([xyz])$)");
        std::smatch matches;

        if (std::regex_match(expression, matches, componentPattern))
        {
            std::string argumentName = matches[1].str();
            std::string component = matches[2].str();

            if (isValidArgumentName(argumentName) && isValidComponent(component))
            {
                return ComponentAccess(argumentName, component);
            }
        }

        return ComponentAccess(); // Invalid or no component access
    }

    bool ArgumentUtils::hasComponentAccess(std::string const & expression)
    {
        // Look for patterns like "name.x", "name.y", "name.z"
        std::regex componentPattern(R"([A-Za-z_][A-Za-z0-9_]*\.[xyz])");
        return std::regex_search(expression, componentPattern);
    }

    std::string ArgumentUtils::argumentTypeToString(ArgumentType type)
    {
        switch (type)
        {
        case ArgumentType::Scalar:
            return "Scalar";
        case ArgumentType::Vector:
            return "Vector";
        default:
            return "Unknown";
        }
    }

    ArgumentType ArgumentUtils::stringToArgumentType(std::string const & typeStr)
    {
        if (typeStr == "Vector")
        {
            return ArgumentType::Vector;
        }
        return ArgumentType::Scalar; // Default to scalar
    }

    bool ArgumentUtils::isValidArgumentName(std::string const & name)
    {
        if (name.empty())
        {
            return false;
        }

        // Check for valid identifier pattern (starts with letter or underscore)
        std::regex identifierPattern(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
        if (!std::regex_match(name, identifierPattern))
        {
            return false;
        }

        // Reserved mathematical function names that should not be used as arguments
        static const std::set<std::string> reservedNames = {
          "sin",   "cos", "tan",  "asin",  "acos", "atan", "atan2", "sinh",  "cosh", "tanh",
          "exp",   "log", "log2", "log10", "sqrt", "abs",  "sign",  "floor", "ceil", "round",
          "fract", "pow", "fmod", "min",   "max",  "pi",   "e", // Mathematical constants
          "x",     "y",   "z", // Common coordinate names (could be allowed but might be confusing)
        };

        return reservedNames.find(name) == reservedNames.end();
    }

    bool ArgumentUtils::isReservedKeyword(std::string const & name)
    {
        static std::set<std::string> const reservedKeywords = {
          // Types
          "float", "vec2", "vec3", "vec4", "mat3", "mat4", "int", "void", "bool",
          // Control flow
          "return", "if", "else", "for", "while", "do", "break", "continue",
          // Trigonometric functions
          "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
          // Hyperbolic functions
          "sinh", "cosh", "tanh",
          // Exponential / logarithmic
          "exp", "log", "log2", "log10", "sqrt",
          // Common math
          "abs", "sign", "floor", "ceil", "round", "fract",
          "length", "dot", "cross", "normalize",
          "pow", "min", "max", "clamp", "mod", "fmod",
          "select", "mix", "step", "smoothstep",
          // Constants
          "pi", "e",
        };
        return reservedKeywords.find(name) != reservedKeywords.end();
    }

} // namespace gladius
