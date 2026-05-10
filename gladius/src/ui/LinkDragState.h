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
        using CompatiblePortSet = std::unordered_set<int64_t>;

        bool isDragging = false;
        nodes::PortId sourcePortId{0};
        std::type_index sourcePortType{typeid(void)};
        bool sourceIsOutput = false;
        CompatiblePortSet compatiblePorts;
        bool compatibilityComputed = false;

        /// Initialize a new drag session from a source port.
        void beginDrag(nodes::PortId sourcePort, std::type_index sourceType, bool isOutput);

        /// Install a resolved compatibility set for the active drag session.
        void setCompatiblePorts(CompatiblePortSet compatiblePortIds);

        /// Recompute compatible ports from the model for the current source port.
        void computeCompatibility(nodes::Model & model);

        /// Whether an explicit compatibility set was computed for the active drag.
        [[nodiscard]] bool hasComputedCompatibility() const;

        /// Check if a specific port/parameter is compatible with the current drag source.
        [[nodiscard]] bool isCompatible(int64_t portOrParamId) const;

        /// Reset to idle state.
        void reset();
    };
} // namespace gladius::ui
