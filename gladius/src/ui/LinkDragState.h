#pragma once

#include "nodes/Model.h"

#include <cstdint>
#include <typeindex>
#include <unordered_set>

namespace gladius::ui
{
    /// Tracks port compatibility information during link creation drag.
    /// Computed once when a drag starts; reset when the drag ends or is cancelled.
    struct LinkDragState
    {
        bool isDragging = false;
        nodes::PortId sourcePortId{0};
        std::type_index sourcePortType{typeid(void)};
        bool sourceIsOutput = false;
        std::unordered_set<int64_t> compatiblePorts;

        /// Recompute compatible ports from the model for the current source port.
        void computeCompatibility(nodes::Model const & model);

        /// Check if a specific port/parameter is compatible with the current drag source.
        [[nodiscard]] bool isCompatible(int64_t portOrParamId) const;

        /// Reset to idle state.
        void reset();
    };
} // namespace gladius::ui
