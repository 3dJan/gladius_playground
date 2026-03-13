#pragma once

#include "../nodes/NodeBase.h"
#include "Style.h"

#include <imgui.h>
#include <string>

namespace gladius::ui
{
    class ModelEditor;

    /// Renders eligible nodes as compact circles with port bubbles.
    /// Eligible nodes have ≤2 visible inputs, ≤2 visible outputs,
    /// all inputs require source connections (no input widgets),
    /// and the node is not Begin/End/FunctionCall.
    namespace circle_node
    {
        /// Check whether a node qualifies for compact circle rendering.
        [[nodiscard]] bool isEligible(nodes::NodeBase & node);

        /// Get the short operator symbol for a node (e.g. "+" for Addition).
        [[nodiscard]] std::string getOperatorSymbol(nodes::NodeBase const & node);

        /// Get the type-based color for a port.
        [[nodiscard]] ImVec4 portColor(std::type_index typeIndex);
    } // namespace circle_node
} // namespace gladius::ui
