#pragma once

#include <Model.h>
#include <typeindex>

namespace gladius::ui
{
    using OptionalPortId = std::optional<nodes::PortId>;
    auto inputMenu(nodes::Model & nodes,
                   nodes::ParameterId targetId,
                   nodes::NodeId targetParentId,
                   std::type_index targetType,
                   std::string const & targetName) -> OptionalPortId;
} // namespace gladius::ui
