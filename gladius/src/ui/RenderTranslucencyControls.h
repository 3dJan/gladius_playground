#pragma once

#include "compute/TranslucencyConfig.h"

namespace gladius::ui
{
    /**
     * @brief Draw the translucency checkbox and tuning sliders into the current ImGui window.
     * @param config edited in place
     * @param itemWidth width of the sliders in pixels
     * @return true when the user changed any value
     */
    bool renderTranslucencyControls(compute::TranslucencyConfig & config, float itemWidth);
} // namespace gladius::ui
