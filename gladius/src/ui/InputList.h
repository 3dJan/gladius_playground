#pragma once

#include "../nodes/Model.h"
#include <functional>
#include <optional>
#include <typeindex>

namespace gladius::ui
{
    struct InputMenuSelection
    {
        nodes::PortId sourcePortId{};
        std::optional<std::type_index> newConstantType;
    };

    using OptionalInputMenuSelection = std::optional<InputMenuSelection>;
    using OptionalParameterId = std::optional<nodes::ParameterId>;
        using ConnectionPreviewCallback =
            std::function<void(nodes::PortId sourcePortId, nodes::ParameterId targetParameterId)>;

        auto inputMenu(nodes::Model & model,
                                     nodes::ParameterId targetId,
                                     ConnectionPreviewCallback previewCandidate = {}) -> OptionalInputMenuSelection;

    auto outputMenu(nodes::Model & model,
                    nodes::PortId sourcePortId,
                                        std::function<bool()> renderCreateNodeMenu = {},
                                        ConnectionPreviewCallback previewCandidate = {}) -> OptionalParameterId;
} // namespace gladius::ui
