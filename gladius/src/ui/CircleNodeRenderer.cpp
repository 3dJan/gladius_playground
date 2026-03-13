#include "CircleNodeRenderer.h"

#include "LinkColors.h"
#include "Parameter.h"

namespace gladius::ui::circle_node
{
    namespace
    {
        struct OperatorEntry
        {
            char const * nodeName;
            char const * symbol;
        };

        // Map from node base name to compact operator symbol.
        constexpr OperatorEntry OPERATOR_SYMBOLS[] = {
          {"Addition", "+"},
          {"Subtraction", "-"},
          {"Multiplication", "\xc3\x97"},            // ×
          {"Division", "\xc3\xb7"},                  // ÷
          {"Sine", "sin"},
          {"Cosine", "cos"},
          {"Tangent", "tan"},
          {"SinH", "sinh"},
          {"CosH", "cosh"},
          {"TanH", "tanh"},
          {"ArcSin", "asin"},
          {"ArcCos", "acos"},
          {"ArcTan", "atan"},
          {"ArcTan2", "atan2"},
          {"Abs", "|x|"},
          {"Sqrt", "\xe2\x88\x9a"},                  // √
          {"Log", "log"},
          {"Log2", "log2"},
          {"Log10", "lg"},
          {"Exp", "exp"},
          {"Pow", "pow"},
          {"Min", "min"},
          {"Max", "max"},
          {"Clamp", "clamp"},
          {"Round", "round"},
          {"Ceil", "ceil"},
          {"Floor", "floor"},
          {"Sign", "sgn"},
          {"Fract", "fract"},
          {"Fmod", "fmod"},
          {"Mod", "mod"},
          {"Length", "|v|"},
          {"DotProduct", "\xc2\xb7"},                // ·
          {"CrossProduct", "\xc3\x97"},              // ×
          {"VectorFromScalar", "s\xe2\x86\x92v"},    // s→v
          {"Select", "sel"},
          {"Mix", "mix"},
        };

        std::string findSymbol(std::string const & nodeName)
        {
            for (auto const & entry : OPERATOR_SYMBOLS)
            {
                if (nodeName == entry.nodeName)
                {
                    return entry.symbol;
                }
            }
            return {};
        }

        int countVisibleInputs(nodes::NodeBase & node)
        {
            int count = 0;
            for (auto & [name, param] : node.parameter())
            {
                if (param.isVisible())
                {
                    ++count;
                }
            }
            return count;
        }

        int countVisibleOutputs(nodes::NodeBase & node)
        {
            int count = 0;
            for (auto const & [name, port] : node.getOutputs())
            {
                if (port.isVisible())
                {
                    ++count;
                }
            }
            return count;
        }

        bool allInputsRequireSource(nodes::NodeBase & node)
        {
            for (auto & [name, param] : node.parameter())
            {
                if (param.isVisible() && !param.isInputSourceRequired())
                {
                    return false;
                }
            }
            return true;
        }

        constexpr int MAX_INPUTS_FOR_CIRCLE = 3;
        constexpr int MAX_OUTPUTS_FOR_CIRCLE = 3;
    } // namespace

    bool isEligible(nodes::NodeBase & node)
    {
        auto const cat = node.getCategory();
        if (cat != nodes::Category::Math && cat != nodes::Category::_3mf)
        {
            return false;
        }

        if (findSymbol(node.name()).empty())
        {
            return false;
        }

        if (countVisibleInputs(node) >= MAX_INPUTS_FOR_CIRCLE)
        {
            return false;
        }
        if (countVisibleOutputs(node) >= MAX_OUTPUTS_FOR_CIRCLE)
        {
            return false;
        }

        if (!allInputsRequireSource(node))
        {
            return false;
        }

        return true;
    }

    std::string getOperatorSymbol(nodes::NodeBase const & node)
    {
        auto sym = findSymbol(node.name());
        return sym.empty() ? node.name() : sym;
    }

    ImVec4 portColor(std::type_index typeIndex)
    {
        if (typeIndex == nodes::ParameterTypeIndex::Float)
        {
            return LinkColors::ColorFloat;
        }
        if (typeIndex == nodes::ParameterTypeIndex::Float3)
        {
            return LinkColors::ColorFloat3;
        }
        if (typeIndex == nodes::ParameterTypeIndex::Matrix4)
        {
            return LinkColors::ColorMatrix;
        }
        if (typeIndex == nodes::ParameterTypeIndex::ResourceId)
        {
            return LinkColors::ColorResource;
        }
        if (typeIndex == nodes::ParameterTypeIndex::String)
        {
            return LinkColors::ColorString;
        }
        if (typeIndex == nodes::ParameterTypeIndex::Int)
        {
            return LinkColors::ColorInt;
        }
        return {1.f, 1.f, 1.f, 1.f};
    }
} // namespace gladius::ui::circle_node
