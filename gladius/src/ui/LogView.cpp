#include "LogView.h"

#include <fmt/chrono.h>
#include <time.h>

#include "IconsFontAwesome4.h"
#include "wordwarp.h"

namespace gladius::ui
{
    /// Helper function to convert severity enum to string label for clipboard output.
    char const* getSeverityLabel(events::Severity severity)
    {
        switch (severity)
        {
        case events::Severity::Warning:
            return "WARNING";
        case events::Severity::Error:
            return "ERROR";
        case events::Severity::FatalError:
            return "FATAL";
        case events::Severity::Info:
        default:
            return "INFO";
        }
    }
}

namespace gladius::ui::tests
{
    /// Format a single event for clipboard output.
    /// Format: [YYYY-MM-DD HH:MM:SS] [SEVERITY] Message
    std::string formatEventForClipboard(events::Event const& event)
    {
        auto inTime = std::chrono::system_clock::to_time_t(event.getTimeStamp());
        std::tm tm{};
#ifdef _MSVC_LANG
        localtime_s(&tm, &inTime);
#else
        localtime_r(&inTime, &tm);
#endif
        return fmt::format("[{:%Y-%m-%d %H:%M:%S}] [{}] {}",
                          tm,
                          getSeverityLabel(event.getSeverity()),
                          event.getMessage());
    }
}

namespace gladius::ui
{
    void LogView::show()
    {
        m_visible = true;
    }

    void LogView::hide()
    {
        m_visible = false;
    }

    void LogView::render(gladius::events::Logger & logger)
    {
        if (!m_visible)
        {
            return;
        }

        // Ensure cache is initialized on first render (applies default severity filter)
        if (m_cacheNeedsInitialUpdate)
        {
            updateCache(logger);
            m_cacheNeedsInitialUpdate = false;
        }

        // Render appropriate view based on collapsed state
        if (m_collapsed)
        {
            renderCollapsedView(logger);
        }
        else
        {
            // we need at least 400px height to show the toolbar and the log

            ImGui::SetNextWindowSizeConstraints(
              ImVec2(-1, 400),
              ImVec2(-1, FLT_MAX)); // Min height of 400px, no max height constraint
            ImGui::SetNextWindowSize(ImVec2(0, 400));

            ImGui::Begin("Events", &m_visible);
            // Top toolbar - Row 1: Settings and Filters
            ImGui::Checkbox("Auto-scroll", &m_autoScroll);
            ImGui::SameLine();

            // Severity filter controls
            bool severityChanged = false;
            ImGui::TextUnformatted("Show:");
            ImGui::SameLine();
            if (ImGui::Checkbox("Info", &m_showInfo))
            {
                severityChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Warnings", &m_showWarnings))
            {
                severityChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Errors", &m_showErrors))
            {
                severityChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Fatal", &m_showFatal))
            {
                severityChanged = true;
            }

            if (severityChanged)
            {
                updateCache(logger);
            }

            ImGui::SameLine();

            if (ImGui::Button("Collapse"))
            {
                m_collapsed = true;
                // Force update of cache when switching view modes
                updateCache(logger);
            }

            // Top toolbar - Row 2: Search and Actions
            ImGui::SetNextItemWidth(200.0f);
            if (m_filter.Draw("Filter") || (m_logSizeWhenCacheWasGenerated != logger.size()))
            {
                if (m_filter.IsActive() || isSeverityFilterActive())
                {
                    updateCache(logger);
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Clear log"))
            {
                logger.clear();
            }

            // Copy All button - copies all visible (filtered) events to clipboard
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_CLIPBOARD " Copy All"))
            {
                auto const& copyEventsBegin = (m_filter.IsActive() || isSeverityFilterActive())
                                               ? m_filteredEvents.cbegin()
                                               : logger.cbegin();
                auto const& copyEventsEnd = (m_filter.IsActive() || isSeverityFilterActive())
                                             ? m_filteredEvents.cend()
                                             : logger.cend();

                if (copyEventsBegin != copyEventsEnd)
                {
                    std::string clipboardText;
                    for (auto iter = copyEventsBegin; iter != copyEventsEnd; ++iter)
                    {
                        clipboardText += tests::formatEventForClipboard(*iter) + "\n";
                    }
                    ImGui::SetClipboardText(clipboardText.c_str());
                }
            }
            if (ImGui::IsItemHovered())
            {
                auto const eventCount = (m_filter.IsActive() || isSeverityFilterActive())
                                          ? m_filteredEvents.size()
                                          : logger.size();
                if (eventCount == 0)
                {
                    ImGui::SetTooltip("No events to copy");
                }
                else
                {
                    ImGui::SetTooltip("Copy %zu event(s) to clipboard", eventCount);
                }
            }

            // Copy Errors & Warnings button - copies only errors and warnings to clipboard
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_EXCLAMATION_TRIANGLE " Copy Errors && Warnings"))
            {
                std::string clipboardText;
                size_t count = 0;
                for (auto const& event : logger)
                {
                    auto const severity = event.getSeverity();
                    if (severity == events::Severity::Error ||
                        severity == events::Severity::Warning ||
                        severity == events::Severity::FatalError)
                    {
                        clipboardText += tests::formatEventForClipboard(event) + "\n";
                        ++count;
                    }
                }
                if (!clipboardText.empty())
                {
                    ImGui::SetClipboardText(clipboardText.c_str());
                }
            }
            if (ImGui::IsItemHovered())
            {
                size_t count = 0;
                for (auto const& event : logger)
                {
                    auto const severity = event.getSeverity();
                    if (severity == events::Severity::Error ||
                        severity == events::Severity::Warning ||
                        severity == events::Severity::FatalError)
                    {
                        ++count;
                    }
                }
                if (count == 0)
                {
                    ImGui::SetTooltip("No errors or warnings to copy");
                }
                else
                {
                    ImGui::SetTooltip("Copy %zu error(s) and warning(s) to clipboard", count);
                }
            }

            renderExpandedView(logger);
            ImGui::End();
        }
    }

    void LogView::renderCollapsedView(gladius::events::Logger & logger)
    {
        auto const & eventsBegin = (m_filter.IsActive() || isSeverityFilterActive())
                                     ? m_filteredEvents.cbegin()
                                     : logger.cbegin();
        auto const & eventsEnd = (m_filter.IsActive() || isSeverityFilterActive())
                                   ? m_filteredEvents.cend()
                                   : logger.cend();

        // remove window size constraints to allow resizing
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                            ImVec2(-1, FLT_MAX)); // No min/max constraints
        ImGui::SetNextWindowSize(ImVec2(500, 50));

        ImGui::Begin("Events", &m_visible);

        // Count warnings, errors and fatal errors
        size_t infoCount = 0;
        size_t warningCount = 0;
        size_t errorCount = 0;
        size_t fatalErrorCount = 0;
        bool hasFatalError = false;

        for (auto iter = eventsBegin; iter != eventsEnd; ++iter)
        {
            switch (iter->getSeverity())
            {
            case events::Severity::Info:
                infoCount++;
                break;
            case events::Severity::Warning:
                warningCount++;
                break;
            case events::Severity::Error:
                errorCount++;
                break;
            case events::Severity::FatalError:
                fatalErrorCount++;
                hasFatalError = true;
                break;
            default:;
            }
        }
        if (fatalErrorCount > 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.f, 1.0f));
            if (ImGui::SmallButton(
                  fmt::format(ICON_FA_EXCLAMATION " Fatal Errors: {}", fatalErrorCount).c_str()))
            {
                ImGui::OpenPopup("fatal_errors_popup");
            }
            ImGui::PopStyleColor();

            // Popup with fatal error messages and copy button
            if (ImGui::BeginPopup("fatal_errors_popup"))
            {
                // Copy button at the top
                if (ImGui::Button(ICON_FA_CLIPBOARD " Copy All Fatal Errors"))
                {
                    std::string clipboardText;
                    for (auto iter = eventsBegin; iter != eventsEnd; ++iter)
                    {
                        if (iter->getSeverity() == events::Severity::FatalError)
                        {
                            clipboardText += tests::formatEventForClipboard(*iter) + "\n";
                        }
                    }
                    ImGui::SetClipboardText(clipboardText.c_str());
                }
                ImGui::Separator();

                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 50.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.f, 1.0f));
                ImGui::TextUnformatted("Fatal Errors:");
                ImGui::PopStyleColor();
                ImGui::Separator();

                for (auto iter = eventsBegin; iter != eventsEnd; ++iter)
                {
                    if (iter->getSeverity() == events::Severity::FatalError)
                    {
                        ImGui::TextUnformatted(iter->getMessage().c_str());
                        ImGui::Separator();
                    }
                }
                ImGui::PopTextWrapPos();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
        }

        if (errorCount > 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
            if (ImGui::SmallButton(
                  fmt::format(ICON_FA_EXCLAMATION_TRIANGLE " Errors: {}", errorCount).c_str()))
            {
                ImGui::OpenPopup("errors_popup");
            }
            ImGui::PopStyleColor();

            // Popup with error messages and copy button
            if (ImGui::BeginPopup("errors_popup"))
            {
                // Copy button at the top
                if (ImGui::Button(ICON_FA_CLIPBOARD " Copy All Errors"))
                {
                    std::string clipboardText;
                    for (auto iter = eventsBegin; iter != eventsEnd; ++iter)
                    {
                        if (iter->getSeverity() == events::Severity::Error)
                        {
                            clipboardText += tests::formatEventForClipboard(*iter) + "\n";
                        }
                    }
                    ImGui::SetClipboardText(clipboardText.c_str());
                }
                ImGui::Separator();

                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 50.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                ImGui::TextUnformatted("Errors:");
                ImGui::PopStyleColor();
                ImGui::Separator();

                for (auto iter = eventsBegin; iter != eventsEnd; ++iter)
                {
                    if (iter->getSeverity() == events::Severity::Error)
                    {
                        ImGui::TextUnformatted(iter->getMessage().c_str());
                        ImGui::Separator();
                    }
                }
                ImGui::PopTextWrapPos();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
        }

        if (warningCount > 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.f, 1.0f));
            if (ImGui::SmallButton(
                  fmt::format(ICON_FA_EXCLAMATION_CIRCLE " Warnings: {}", warningCount).c_str()))
            {
                ImGui::OpenPopup("warnings_popup");
            }
            ImGui::PopStyleColor();

            // Popup with warning messages and copy button
            if (ImGui::BeginPopup("warnings_popup"))
            {
                // Copy button at the top
                if (ImGui::Button(ICON_FA_CLIPBOARD " Copy All Warnings"))
                {
                    std::string clipboardText;
                    for (auto iter = eventsBegin; iter != eventsEnd; ++iter)
                    {
                        if (iter->getSeverity() == events::Severity::Warning)
                        {
                            clipboardText += tests::formatEventForClipboard(*iter) + "\n";
                        }
                    }
                    ImGui::SetClipboardText(clipboardText.c_str());
                }
                ImGui::Separator();

                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 50.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.f, 1.0f));
                ImGui::TextUnformatted("Warnings:");
                ImGui::PopStyleColor();
                ImGui::Separator();

                for (auto iter = eventsBegin; iter != eventsEnd; ++iter)
                {
                    if (iter->getSeverity() == events::Severity::Warning)
                    {
                        ImGui::TextUnformatted(iter->getMessage().c_str());
                        ImGui::Separator();
                    }
                }
                ImGui::PopTextWrapPos();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
        }

        if (ImGui::Button("Clear"))
        {
            logger.clear();
            hide();
        }

        if (ImGui::Button("Show Log"))
        {
            m_collapsed = false;
            // Force update of cache when switching view modes
            updateCache(logger);
        }

        // Show the fatal error dialog if needed
        if (hasFatalError)
        {
            auto lastFatalErrorIter = eventsEnd;
            for (auto iter = eventsBegin; iter != eventsEnd; ++iter)
            {
                if (iter->getSeverity() == events::Severity::FatalError)
                {
                    lastFatalErrorIter = iter;
                }
            }

            if (lastFatalErrorIter != eventsEnd)
            {
                ImGui::Begin("Something went terribly wrong");
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.f, 1.0f));
                ImGui::Text(reinterpret_cast<const char *>(ICON_FA_EXCLAMATION
                                                           "\t\tA fatal error has occurred\t"));
                ImGui::PopStyleColor();
                ImGui::NewLine();
                ImGui::TextUnformatted(warpTextAfter(lastFatalErrorIter->getMessage(), 80).c_str());
                ImGui::NewLine();

                if (ImGui::Button("Quit application"))
                {
                    exit(EXIT_FAILURE);
                }
                ImGui::End();
            }
        }
        ImGui::End();
    }

    void LogView::renderExpandedView(gladius::events::Logger & logger)
    {
        auto const & eventsBegin = (m_filter.IsActive() || isSeverityFilterActive())
                                     ? m_filteredEvents.cbegin()
                                     : logger.cbegin();
        auto const & eventsEnd = (m_filter.IsActive() || isSeverityFilterActive())
                                   ? m_filteredEvents.cend()
                                   : logger.cend();

        ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

        auto const logSize = std::distance(eventsBegin, eventsEnd);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(logSize));

        auto lastFatalErrorIter = logger.cend();
        while (clipper.Step())
        {
            auto displayBegin = eventsBegin + clipper.DisplayStart;
            auto displayEnd = eventsBegin + clipper.DisplayEnd;

            for (auto iter = displayBegin; iter != eventsEnd && iter != displayEnd; ++iter)
            {
                auto const eventIndex = static_cast<int>(std::distance(eventsBegin, iter));
                ImGui::PushID(eventIndex);

                auto inTime = std::chrono::system_clock::to_time_t(iter->getTimeStamp());

                std::tm tm{};
#ifdef _MSVC_LANG
                localtime_s(&tm, &inTime);
#else
                localtime_r(&inTime, &tm);
#endif
                // Make row selectable for context menu and keyboard copy
                bool const isSelected = (m_selectedEventIndex == eventIndex);
                if (ImGui::Selectable("##event_row", isSelected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowItemOverlap))
                {
                    m_selectedEventIndex = eventIndex;
                }

                // Right-click context menu for individual event copy
                if (ImGui::BeginPopupContextItem("event_context_menu"))
                {
                    if (ImGui::MenuItem(ICON_FA_CLIPBOARD " Copy"))
                    {
                        ImGui::SetClipboardText(
                            tests::formatEventForClipboard(*iter).c_str());
                    }
                    ImGui::EndPopup();
                }

                ImGui::SameLine();
                ImGui::TextUnformatted(fmt::format("{:%Y-%m-%d %H:%M:%S}", tm).c_str());
                ImGui::SameLine();

                switch (iter->getSeverity())
                {
                case events::Severity::Info:
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.2f, 1.f, 1.0f));
                    ImGui::Text(reinterpret_cast<const char *>("\tINFO\t" ICON_FA_INFO));
                    ImGui::PopStyleColor();
                    break;
                case events::Severity::Warning:
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.f, 1.0f));
                    ImGui::Text(
                      reinterpret_cast<const char *>("\tWARNING\t" ICON_FA_EXCLAMATION_CIRCLE));
                    ImGui::PopStyleColor();
                    break;
                case events::Severity::Error:
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text(
                      reinterpret_cast<const char *>("\tERROR\t" ICON_FA_EXCLAMATION_TRIANGLE));
                    ImGui::PopStyleColor();
                    break;
                case events::Severity::FatalError:
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.f, 1.0f));
                    ImGui::Text(
                      reinterpret_cast<const char *>("\t\tFATAL ERROR:\t" ICON_FA_EXCLAMATION));
                    ImGui::PopStyleColor();
                    lastFatalErrorIter = iter;

                    break;
                default:;
                }

                ImGui::SameLine();
                ImGui::TextUnformatted(iter->getMessage().c_str());

                ImGui::PopID();
            }
        }

        // Ctrl+C keyboard shortcut to copy selected event
        if (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_C) && m_selectedEventIndex >= 0 &&
            m_selectedEventIndex < static_cast<int>(logSize))
        {
            auto const& selectedEvent = *(eventsBegin + m_selectedEventIndex);
            ImGui::SetClipboardText(tests::formatEventForClipboard(selectedEvent).c_str());
        }

        if (lastFatalErrorIter != logger.cend())
        {
            ImGui::Begin("Something went terribly wrong");
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.f, 1.0f));
            ImGui::Text(reinterpret_cast<const char *>(ICON_FA_EXCLAMATION
                                                       "\t\tA fatal error has occurred\t"));
            ImGui::PopStyleColor();
            ImGui::NewLine();
            ImGui::TextUnformatted(warpTextAfter(lastFatalErrorIter->getMessage(), 80).c_str());
            ImGui::NewLine();

            if (ImGui::Button("Quit application"))
            {
                exit(EXIT_FAILURE);
            }
            ImGui::End();
        }

        if (m_autoScroll)
        {
            ImGui::SetScrollHereY(0.9999f); // workaround for bug in SetScrollHereY see
                                            // https://github.com/ocornut/imgui/issues/1804
        }
        ImGui::EndChild();
    }

    void LogView::updateCache(gladius::events::Logger & logger)
    {
        m_logSizeWhenCacheWasGenerated = logger.size();
        m_filteredEvents.clear();
        for (auto & item : logger)
        {
            bool passSeverity = true;
            switch (item.getSeverity())
            {
            case events::Severity::Info:
                passSeverity = m_showInfo;
                break;
            case events::Severity::Warning:
                passSeverity = m_showWarnings;
                break;
            case events::Severity::Error:
                passSeverity = m_showErrors;
                break;
            case events::Severity::FatalError:
                passSeverity = m_showFatal;
                break;
            default:
                break;
            }
            if (passSeverity &&
                (!m_filter.IsActive() || m_filter.PassFilter(item.getMessage().c_str())))
            {
                m_filteredEvents.push_back(item);
            }
        }
    }
}
