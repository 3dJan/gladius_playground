#pragma once

#include "../EventLogger.h"

#include "imgui.h"

namespace gladius::ui
{
    /**
     * @brief Event viewer component displaying log events with filtering and copy functionality.
     *
     * Provides a UI for viewing, filtering, and copying log events. Supports both an expanded
     * view with full event details and a collapsed view showing severity counts.
     *
     * Features:
     * - Severity-based filtering (Info, Warning, Error, Fatal)
     * - Text-based filtering via search box
     * - Right-click context menu to copy individual events
     * - Ctrl+C keyboard shortcut to copy selected event
     * - "Copy All" button to copy all visible/filtered events
     * - Collapsed view with per-severity copy buttons
     *
     * By default, Info-level messages are hidden; only Warnings, Errors, and Fatal errors
     * are shown. Users can enable Info display via the filter checkboxes.
     */
    class LogView
    {
      public:
        /// @brief Show the event viewer window.
        void show();

        /// @brief Hide the event viewer window.
        void hide();

        /**
         * @brief Render the event viewer UI.
         * @param logger The event logger to display events from.
         *
         * Renders either the expanded or collapsed view based on current state.
         * Handles filter updates, clipboard copy operations, and user interactions.
         */
        void render(events::Logger & logger);

      private:
        /**
         * @brief Rebuild the filtered events cache based on current filter settings.
         * @param logger The event logger to filter events from.
         */
        void updateCache(events::Logger & logger);

        /**
         * @brief Render the collapsed view showing severity counts.
         * @param logger The event logger to display events from.
         *
         * Shows clickable buttons for each severity level that open popups
         * with event lists and copy functionality.
         */
        void renderCollapsedView(events::Logger & logger);

        /**
         * @brief Render the expanded view showing all events in a list.
         * @param logger The event logger to display events from.
         *
         * Displays events with timestamps, severity icons, and messages.
         * Supports right-click copy and keyboard shortcuts.
         */
        void renderExpandedView(events::Logger & logger);

        /**
         * @brief Check if any severity filter is active (not all severities enabled).
         * @return True if at least one severity type is hidden.
         */
        bool isSeverityFilterActive() const
        {
            return !(m_showInfo && m_showWarnings && m_showErrors && m_showFatal);
        }

        bool m_visible = false;     ///< Whether the window is visible
        bool m_autoScroll = true;   ///< Whether to auto-scroll to the latest event
        bool m_collapsed = false;   ///< Whether to show collapsed or expanded view
        ImGuiTextFilter m_filter;   ///< Text-based filter for event messages
        events::Events m_filteredEvents;  ///< Cached list of filtered events
        size_t m_logSizeWhenCacheWasGenerated = 0u;  ///< Log size when cache was last updated
        bool m_cacheNeedsInitialUpdate = true;  ///< Whether cache needs initial population
        int m_selectedEventIndex = -1;  ///< Index of currently selected event (-1 = none)

        // Severity filter state (Warnings, Errors, Fatal enabled by default; Info hidden)
        bool m_showInfo = false;    ///< Show Info-level events (default: hidden)
        bool m_showWarnings = true; ///< Show Warning-level events
        bool m_showErrors = true;   ///< Show Error-level events
        bool m_showFatal = true;    ///< Show FatalError-level events
    };
}
