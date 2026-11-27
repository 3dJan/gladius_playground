#include "SurfaceProfile.h"

#include <algorithm>

namespace gladius::ui
{
    SurfaceProfile resolveSurfaceProfile(SurfaceDescription const & surface,
                                         LayoutModeOverride overrideMode)
    {
        switch (overrideMode)
        {
          case LayoutModeOverride::Desktop:
            return SurfaceProfile::Desktop;
          case LayoutModeOverride::Compact:
            return SurfaceProfile::Compact;
          case LayoutModeOverride::Auto:
            break;
        }

        bool const sizeIsSmall = surface.size.x <= surfaceMetrics::compactMaxWidthPx ||
                                 surface.size.y <= surfaceMetrics::compactMaxHeightPx;
        return (sizeIsSmall || surface.coarsePointer) ? SurfaceProfile::Compact
                                                      : SurfaceProfile::Desktop;
    }

    float touchScaleFor(SurfaceProfile profile, float widthPx)
    {
        if (profile != SurfaceProfile::Compact || widthPx <= 0.f)
        {
            return 1.0f;
        }

        // Below the reference width the same UI has to stay finger sized, so grow with the
        // deficit instead of letting the font size shrink the hit areas.
        float const scale = surfaceMetrics::touchReferenceWidthPx / widthPx;
        return std::clamp(scale, 1.0f, surfaceMetrics::maxTouchScale);
    }

    float scrollbarSizeFor(float baseSizePx, SurfaceProfile profile, float touchScale)
    {
        if (profile != SurfaceProfile::Compact || touchScale <= 1.0f)
        {
            return baseSizePx;
        }

        // Themes are tuned for a mouse cursor; on a touch surface the same grab is too thin to hit.
        return std::max(baseSizePx * touchScale, surfaceMetrics::minScrollbarSizePx);
    }

    char const * layoutModeOverrideToString(LayoutModeOverride mode) noexcept
    {
        switch (mode)
        {
          case LayoutModeOverride::Desktop:
            return "desktop";
          case LayoutModeOverride::Compact:
            return "compact";
          case LayoutModeOverride::Auto:
            break;
        }
        return "auto";
    }

    LayoutModeOverride layoutModeOverrideFromString(std::string const & value)
    {
        if (value == "desktop")
        {
            return LayoutModeOverride::Desktop;
        }
        if (value == "compact")
        {
            return LayoutModeOverride::Compact;
        }
        return LayoutModeOverride::Auto;
    }

    SurfaceProfileStore & SurfaceProfileStore::instance()
    {
        static SurfaceProfileStore store;
        return store;
    }

    void SurfaceProfileStore::publish(SurfaceProfile profile, float touchScale)
    {
        m_profile = profile;
        m_touchScale = touchScale;
    }
} // namespace gladius::ui
