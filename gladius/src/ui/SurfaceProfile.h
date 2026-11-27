#pragma once

#include <imgui.h>

#include <string>

namespace gladius::ui
{
    /**
     * @brief Which class of surface the UI is currently drawn on.
     * @details Layout sensitive code asks for this instead of measuring the window itself, so the
     *          threshold lives in one place. @c Compact covers phones and small browser areas: the
     *          Design Agent panel takes the whole main dock space, the preview stays hidden and
     *          touch targets grow.
     */
    enum class SurfaceProfile
    {
        Desktop,
        Compact
    };

    /// @brief User selectable layout mode; @c Auto derives the profile from the surface.
    enum class LayoutModeOverride
    {
        Auto,
        Desktop,
        Compact
    };

    /// @brief The surface the UI is drawn on, in device independent (CSS) pixels.
    struct SurfaceDescription
    {
        ImVec2 size{0.f, 0.f};
        bool coarsePointer{false};
    };

    namespace surfaceMetrics
    {
        /// @brief A surface this narrow is always treated as compact, regardless of height
        constexpr float compactMaxWidthPx = 700.f;
        /// @brief A surface this short is compact; phone landscape falls under this
        constexpr float compactMaxHeightPx = 420.f;
        /// @brief Width at which the touch scale is 1.0; smaller surfaces scale up from it
        constexpr float touchReferenceWidthPx = 430.f;
        /// @brief Upper bound of the touch scale so a very narrow surface does not blow up layout
        constexpr float maxTouchScale = 1.25f;
        /// @brief Minimum scrollbar width on a compact surface; it is the only scroll handle
        constexpr float minScrollbarSizePx = 20.f;
    } // namespace surfaceMetrics

    /**
     * @brief Resolve the profile for a surface.
     * @param surface Size in CSS pixels plus whether the shell reported a coarse pointer.
     * @param overrideMode Manual selection from settings; @c Auto derives the profile.
     * @details A small surface or a coarse pointer is enough to go compact, so a touch capable
     *          laptop with a small window still gets the touch layout.
     */
    [[nodiscard]] SurfaceProfile resolveSurfaceProfile(SurfaceDescription const & surface,
                                                       LayoutModeOverride overrideMode);

    /**
     * @brief Multiplier applied to touch sensitive sizing on a compact surface.
     * @param profile Profile the layout is being built for.
     * @param widthPx Surface width in CSS pixels.
     * @return Always 1.0 for desktop, so desktop layout is untouched.
     */
    [[nodiscard]] float touchScaleFor(SurfaceProfile profile, float widthPx);

    /**
     * @brief Scrollbar width that is still grabbable with a finger.
     * @param baseSizePx Scrollbar width the current theme and UI scale produced.
     * @param profile Profile the style is being built for.
     * @param touchScale Touch multiplier of the surface.
     * @return The larger of the scaled size and @c surfaceMetrics::minScrollbarSizePx on a compact
     *         surface, @c baseSizePx untouched otherwise.
     */
    [[nodiscard]] float scrollbarSizeFor(float baseSizePx, SurfaceProfile profile, float touchScale);

    /// @brief Persistable name of a layout override
    [[nodiscard]] char const * layoutModeOverrideToString(LayoutModeOverride mode) noexcept;

    /// @brief Parses a persisted layout override; unknown values fall back to @c Auto
    [[nodiscard]] LayoutModeOverride layoutModeOverrideFromString(std::string const & value);

    /**
     * @brief The surface profile of the running UI, readable from any view.
     * @details Views cannot reach the platform layer, so the profile is published once per frame
     *          here instead of being threaded through every constructor. Nothing else writes it.
     */
    class SurfaceProfileStore
    {
      public:
        static SurfaceProfileStore & instance();

        void publish(SurfaceProfile profile, float touchScale);

        [[nodiscard]] SurfaceProfile profile() const noexcept
        {
            return m_profile;
        }

        [[nodiscard]] float touchScale() const noexcept
        {
            return m_touchScale;
        }

        [[nodiscard]] bool isCompact() const noexcept
        {
            return m_profile == SurfaceProfile::Compact;
        }

      private:
        SurfaceProfile m_profile{SurfaceProfile::Desktop};
        float m_touchScale{1.0f};
    };
} // namespace gladius::ui
