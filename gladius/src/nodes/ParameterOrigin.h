#pragma once

#include "nodesfwd.h"
#include "types.h"

#include <compare>

namespace gladius::nodes
{
    /// Source identity within one structural revision; survives cloning and lowering.
    struct ParameterOrigin
    {
        ResourceId modelId{};
        ParameterId parameterId{-1};

        auto operator<=>(ParameterOrigin const &) const = default;
    };
}