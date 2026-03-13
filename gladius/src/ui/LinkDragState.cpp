#include "LinkDragState.h"

namespace gladius::ui
{
    void LinkDragState::computeCompatibility(nodes::Model const & model)
    {
        compatiblePorts.clear();
        if (!isDragging)
        {
            return;
        }

        // TODO: Iterate model ports and check type compatibility
        // For now, mark all ports as compatible until the Model API
        // for individual port compatibility checking is integrated.
    }

    bool LinkDragState::isCompatible(int64_t portOrParamId) const
    {
        if (!isDragging)
        {
            return true; // Not dragging — no dimming
        }
        if (compatiblePorts.empty())
        {
            return true; // Fallback: treat all as compatible if not computed
        }
        return compatiblePorts.count(portOrParamId) > 0;
    }

    void LinkDragState::reset()
    {
        isDragging = false;
        sourcePortId = nodes::PortId{0};
        sourcePortType = std::type_index{typeid(void)};
        sourceIsOutput = false;
        compatiblePorts.clear();
    }
} // namespace gladius::ui
