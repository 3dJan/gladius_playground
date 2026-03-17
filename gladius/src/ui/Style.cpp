#include "Style.h"

#include <algorithm>
#include <cmath>
#include <nodes/DerivedNodes.h>
#include <nodes/nodesfwd.h>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>

namespace gladius::ui
{

    // Function to generate a unique color for each node type
    ImVec4 generateUniqueColor(size_t index)
    {
        // Use the golden angle (~137.5°) to maximize hue distance between
        // consecutively registered node types (e.g. sin, cos, tan).
        float constexpr GOLDEN_ANGLE = 137.508f;
        float hue = std::fmod(static_cast<float>(index) * GOLDEN_ANGLE, 360.f);
        float saturation = 40.f + static_cast<float>(index % 4) * 10.f; // 40-70%
        float value = 70.f + static_cast<float>(index % 3) * 10.f;      // 70-90%

        // Convert HSV to RGB
        float c = (value / 100.f) * (saturation / 100.f);
        float x = c * (1.f - std::abs(std::fmod(hue / 60.f, 2.f) - 1.f));
        float m = (value / 100.f) - c;

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

        return map;
    };
} // namespace gladius
