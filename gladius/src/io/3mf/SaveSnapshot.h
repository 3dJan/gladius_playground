#pragma once

#include "nodes/Assembly.h"

#include <lib3mf_implicit.hpp>

#include <cstdint>
#include <memory>

namespace gladius::io
{

    /**
     * @brief Immutable document data captured for an asynchronous native 3MF save.
     *
     * The assembly and Lib3MF model are independent copies. Code operating on a snapshot must
     * never access the live Document or its ComputeCore.
     */
    struct SaveSnapshot
    {
        std::shared_ptr<const nodes::Assembly> assembly;
        Lib3MF::PModel model;
        uint64_t version{0};
    };

} // namespace gladius::io