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

    auto inputMenu(nodes::Model & model, nodes::ParameterId targetId) -> OptionalInputMenuSelection;

    auto outputMenu(nodes::Model & model,
                    nodes::PortId sourcePortId,
                    std::function<bool()> renderCreateNodeMenu = {}) -> OptionalParameterId;
} // namespace gladius::ui
