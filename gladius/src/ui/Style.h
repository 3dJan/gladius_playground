#pragma once
#include "../nodes/nodesfwd.h"

#include "imgui.h"

#include <map>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>

namespace gladius::ui
{
    struct NodeStyle
    {
        ImColor color = IM_COL32(20, 120, 20, 255);
        ImColor activeColor = IM_COL32(40, 150, 40, 255);
        ImColor hoveredColor = IM_COL32(60, 180, 60, 255);
        float borderWidth = 4.f;  ///< Category ring/border thickness
        float rounding = 20.f;    ///< Corner rounding radius for heavily rounded rectangles
    };

    /// Node render configuration passed to pushNodeStyle/popNodeStyle.
    struct NodeRenderConfig
    {
        float borderWidth = 4.f;
        float rounding = 20.f;
    };

    /// Push enhanced node styling (rounding, border width, category color).
    /// Must be paired with popNodeStyle().
    void pushNodeStyle(NodeRenderConfig const & config, ImVec4 categoryColor);
    void popNodeStyle();

    /// Compute the minimum node width that avoids clipping content.
    float computeMinNodeWidth(float headerTextWidth, float contentWidth, float uiScale);

    /// Generate a category color from a type tag string via deterministic hash.
    /// Uses HSV: hue = hash(tag) % 360, saturation = 0.6, value = 0.5.
    ImVec4 generateColorFromTypeTag(std::string const & typeTag);

    using NodeStyles = std::map<nodes::Category, NodeStyle>;

    // Category | color | active color | hovered color
    static const NodeStyles NodeColors{
      {nodes::Category::Transformation,
       {IM_COL32(120, 20, 20, 255), IM_COL32(150, 40, 40, 255), IM_COL32(150, 40, 40, 255)}},
      {nodes::Category::Alteration,
       {IM_COL32(120, 120, 20, 255), IM_COL32(150, 150, 40, 255), IM_COL32(180, 180, 60, 255)}},
      {nodes::Category::Primitive,
       {IM_COL32(20, 120, 20, 255), IM_COL32(40, 150, 40, 255), IM_COL32(60, 180, 60, 255)}},
      {nodes::Category::BoolOperation,
       {IM_COL32(20, 20, 120, 255), IM_COL32(40, 40, 150, 255), IM_COL32(60, 60, 150, 255)}},
      {nodes::Category::Internal,
       {IM_COL32(20, 120, 20, 255), IM_COL32(40, 150, 40, 255), IM_COL32(60, 180, 60, 255)}},
      {nodes::Category::Lattice,
       {IM_COL32(40, 120, 120, 255), IM_COL32(80, 150, 140, 255), IM_COL32(120, 180, 160, 255)}},
      {nodes::Category::Math,
       {IM_COL32(120, 40, 120, 255), IM_COL32(150, 80, 140, 255), IM_COL32(180, 120, 160, 255)}},
      {nodes::Category::Misc,
       {IM_COL32(120, 120, 120, 255), IM_COL32(150, 150, 150, 255), IM_COL32(180, 180, 180, 255)}},
    };

    // Function to generate a unique color for each node type
    ImVec4 generateUniqueColor(size_t index);

    using NodeTypeToColor = std::unordered_map<std::type_index, ImVec4>;

    // Factory for maps from typeid to color
    NodeTypeToColor createNodeTypeToColors();

} // namespace gladius
