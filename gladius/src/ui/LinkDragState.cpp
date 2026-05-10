#include "LinkDragState.h"

namespace gladius::ui
{
    void LinkDragState::beginDrag(nodes::PortId sourcePort, std::type_index sourceType, bool isOutput)
    {
        isDragging = true;
        sourcePortId = sourcePort;
        sourcePortType = sourceType;
        sourceIsOutput = isOutput;
        compatiblePorts.clear();
        compatibilityComputed = false;
    }

    void LinkDragState::setCompatiblePorts(CompatiblePortSet compatiblePortIds)
    {
        compatiblePorts = std::move(compatiblePortIds);
        compatibilityComputed = true;
    }

    void LinkDragState::computeCompatibility(nodes::Model & model)
    {
        compatiblePorts.clear();
        compatibilityComputed = false;
        if (!isDragging)
        {
            return;
        }

        if (sourcePortType == std::type_index{typeid(void)})
        {
            return;
        }

        setCompatiblePorts(model.collectCompatibleLinkCandidates(sourcePortId, sourceIsOutput));
    }

    bool LinkDragState::hasComputedCompatibility() const
    {
        return compatibilityComputed;
    }

    bool LinkDragState::isCompatible(int64_t portOrParamId) const
    {
        if (!isDragging)
        {
            return true; // Not dragging — no dimming
        }
        if (!compatibilityComputed)
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
        compatibilityComputed = false;
    }
} // namespace gladius::ui
