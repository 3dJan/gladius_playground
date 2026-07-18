#pragma once

#include "compute/RenderContracts.h"

namespace gladius
{
    class ResourceContext;
}

namespace gladius::compute
{
    /**
     * @brief Converts the current OpenCL resource state into an API-neutral render request.
     *
     * This is a transition boundary: callers receive no OpenCL camera or settings types, while
     * the existing OpenCL renderer can continue using its native payloads until its adapter is
     * introduced.
     */
    class OpenCLRenderRequestFactory
    {
      public:
        [[nodiscard]] static RenderRequest create(ResourceContext & resources,
                                                  RenderViewport viewport,
                                                  RenderFreshnessStamp freshness);
    };
}