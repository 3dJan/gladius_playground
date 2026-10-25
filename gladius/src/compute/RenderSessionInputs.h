#pragma once

#include "kernel/types.h"

namespace gladius
{
    /**
     * @brief Per-dispatch values for a render session.
     *
     * This value contains only copied camera and rendering parameters. GPU scene payloads remain
     * owned by the retained scene generation and are passed separately to render programs.
     */
    struct RenderSessionInputs
    {
        RenderingSettings settings{};
        cl_float3 eyePosition{};
        cl_float16 modelViewPerspectiveMat{};
    };
} // namespace gladius
