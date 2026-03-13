#include "Style.h"

#include "imguinodeeditor.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <nodes/DerivedNodes.h>
#include <nodes/nodesfwd.h>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>

namespace ed = ax::NodeEditor;

namespace gladius::ui
{

    // Function to generate a unique color for each node type
    ImVec4 generateUniqueColor(size_t index)
    {
        // Generate a color using a simple algorithm to ensure uniqueness
        const size_t numColors = 50;
        const size_t hueStep = 360 / numColors;
        size_t hue = (index * hueStep) % 360;
        size_t saturation = 80 + (index % 20) * 20; // Vary saturation slightly
        size_t value = 60 + (index % 5) * 20;       // Vary value slightly

        // Convert HSV to RGB (simple approximation)
        float c = (value / 100.0f) * (saturation / 100.0f);
        float x = c * (1.0f - abs(fmod(hue / 60.0f, 2.0f) - 1.0f));
        float m = (value / 100.0f) - c;

        float r, g, b;
        if (hue < 60)
        {
            r = c, g = x, b = 0;
        }
        else if (hue < 120)
        {
            r = x, g = c, b = 0;
        }
        else if (hue < 180)
        {
            r = 0, g = c, b = x;
        }
        else if (hue < 240)
        {
            r = 0, g = x, b = c;
        }
        else if (hue < 300)
        {
            r = x, g = 0, b = c;
        }
        else
        {
            r = c, g = 0, b = x;
        }

        return ImVec4(r + m, g + m, b + m, 1.0f);
    }

    NodeTypeToColor createNodeTypeToColors()
    {
        NodeTypeToColor map;
        size_t index = 0;

        // Helper lambda to add node type to the map
        auto addNodeType = [&map, &index](auto nodeType)
        { map[typeid(nodeType)] = generateUniqueColor(index++); };

        // Add each node type to the map
        std::apply([&](auto... nodeTypes) { (addNodeType(nodeTypes), ...); },
                   gladius::nodes::NodeTypes{});

        // T037: Visual distinction for begin/end nodes — override with specific accent colors
        map[typeid(gladius::nodes::Begin)] = ImVec4(0.2f, 0.7f, 0.4f, 1.0f);  // green accent
        map[typeid(gladius::nodes::End)]   = ImVec4(0.7f, 0.3f, 0.3f, 1.0f);  // red accent

        return map;
    };

    void pushNodeStyle(NodeRenderConfig const & config, ImVec4 categoryColor)
    {
        ed::PushStyleVar(ed::StyleVar_NodeRounding, config.rounding);
        ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, config.borderWidth);
        ed::PushStyleColor(ed::StyleColor_NodeBorder, categoryColor);
    }

    void popNodeStyle()
    {
        ed::PopStyleColor(1);
        ed::PopStyleVar(2);
    }

    float computeMinNodeWidth(float headerTextWidth, float contentWidth, float uiScale)
    {
        float constexpr PADDING = 40.f;
        float constexpr MIN_WIDTH = 120.f;
        float const measuredWidth = std::max(headerTextWidth, contentWidth) + PADDING * uiScale;
        return std::max(measuredWidth, MIN_WIDTH * uiScale);
    }

    ImVec4 generateColorFromTypeTag(std::string const & typeTag)
    {
        // Deterministic hash → HSV color
        size_t const hash = std::hash<std::string>{}(typeTag);
        float const hue = static_cast<float>(hash % 360);
        float constexpr SATURATION = 0.6f;
        float constexpr VALUE = 0.5f;

        // HSV → RGB conversion
        float const c = VALUE * SATURATION;
        float const x = c * (1.f - std::abs(std::fmod(hue / 60.f, 2.f) - 1.f));
        float const m = VALUE - c;

        float r, g, b;
        if (hue < 60.f)
        {
            r = c; g = x; b = 0.f;
        }
        else if (hue < 120.f)
        {
            r = x; g = c; b = 0.f;
        }
        else if (hue < 180.f)
        {
            r = 0.f; g = c; b = x;
        }
        else if (hue < 240.f)
        {
            r = 0.f; g = x; b = c;
        }
        else if (hue < 300.f)
        {
            r = x; g = 0.f; b = c;
        }
        else
        {
            r = c; g = 0.f; b = x;
        }

        return ImVec4(r + m, g + m, b + m, 1.f);
    }

} // namespace gladius
