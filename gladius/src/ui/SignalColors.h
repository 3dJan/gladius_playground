#pragma once

#include <imgui.h>

namespace gladius::ui
{
    /**
     * @brief The four signal colours used across the UI, and the only colours allowed to carry state.
     * @details A signal colour answers "is something wrong?" and nothing else. Used carelessly the
     *          whole UI screams, so the rules are:
     *          - Red (@ref colorError) is reserved for something that failed and needs a fix.
     *            Nothing else may be painted red: the eye reads red as "broken" before it reads the
     *            label, so a red value, role or setting is always a lie.
     *          - Amber (@ref colorWarn) is for something that needs attention without being broken:
     *            a paused run, an unanswered question, an unusable but recoverable state.
     *          - Green (@ref colorOk) is for something that finished or was verified.
     *          - Blue (@ref colorInfo) is for what the application is doing right now.
     *          Anything that merely reports a setting, a role or a category stays in the normal text
     *          colours and is separated by emphasis, placement or an outlined chip instead of
     *          colour. The theme accent marks selection (borders, bands, active nodes); it is never
     *          a text colour.
     */
    inline ImVec4 colorOk()
    {
        return {0.40f, 0.82f, 0.46f, 1.f};
    }

    inline ImVec4 colorWarn()
    {
        return {1.00f, 0.72f, 0.32f, 1.f};
    }

    inline ImVec4 colorInfo()
    {
        return {0.40f, 0.82f, 0.98f, 1.f};
    }

    inline ImVec4 colorError()
    {
        return {1.00f, 0.42f, 0.38f, 1.f};
    }
} // namespace gladius::ui
