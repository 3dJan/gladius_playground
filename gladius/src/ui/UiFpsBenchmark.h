#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gladius::ui
{
    struct UiFpsBenchmarkStageTimings
    {
        double preDisplayHookMs{0.0};
        double viewCallbacksMs{0.0};
        double uiDisplayMs{0.0};
        double bufferSwapMs{0.0};
        double neutralBackendMs{0.0};
        double sceneSynchronizationMs{0.0};
        double backendSceneReplacementMs{0.0};
        double boundsUpdateMs{0.0};
        double schedulerPollMs{0.0};
        double completedTaskLatencyMs{0.0};
        double framePresentationMs{0.0};
        double requestFactoryMs{0.0};
        double backendSubmitMs{0.0};
        double taskSubmissionMs{0.0};
    };

    struct UiFpsBenchmarkState
    {
        bool documentLoaded{false};
        bool previewPresented{false};
        bool renderSettled{false};
        std::array<std::uint32_t, 2> previewViewportPx{0u, 0u};
        std::string backend{"unknown"};
        float uiScale{1.0f};
        UiFpsBenchmarkStageTimings stageTimings{};
    };

    /**
     * @brief Records native UI frame cadence until a settled-render capture completes.
     *
     * Samples are buffered and written once at the end so file I/O does not run in the
     * frame loop. The settled capture starts only after the document is loaded, a frame
     * has been presented, and the renderer reports no pending work.
     */
    class UiFpsBenchmark
    {
      public:
        using Clock = std::chrono::steady_clock;

        UiFpsBenchmark(std::filesystem::path outputPath,
                       std::chrono::seconds measurementDuration,
                       std::chrono::seconds warmupDuration = std::chrono::seconds{2},
                       std::chrono::seconds readinessTimeout = std::chrono::seconds{180});

        /**
         * @brief Record one complete UI frame. Returns true once the report is written.
         */
        [[nodiscard]] bool recordFrame(Clock::time_point frameStart,
                                       Clock::time_point frameEnd,
                                       UiFpsBenchmarkState const & state,
                                       std::uint32_t displayWidth,
                                       std::uint32_t displayHeight,
                                       std::array<float, 2> framebufferScale,
                                       bool animationMode);

        [[nodiscard]] std::filesystem::path const & outputPath() const noexcept;
        [[nodiscard]] bool didTimeOut() const noexcept;

      private:
        struct FrameSample
        {
            std::uint64_t frameIndex{0};
            double elapsedMs{0.0};
            std::optional<double> intervalMs;
            double renderMs{0.0};
            std::string phase;
            bool documentLoaded{false};
            bool previewPresented{false};
            bool renderSettled{false};
            bool animationMode{false};
            std::array<std::uint32_t, 2> displaySize{0u, 0u};
            std::array<std::uint32_t, 2> previewViewportPx{0u, 0u};
            std::array<float, 2> framebufferScale{1.0f, 1.0f};
            float uiScale{1.0f};
            UiFpsBenchmarkStageTimings stageTimings{};
            bool measured{false};
            bool measuredInterval{false};
        };

        [[nodiscard]] static double percentile(std::vector<double> values, double fraction);
        [[nodiscard]] static std::string jsonEscape(std::string const & value);
        [[nodiscard]] std::string phaseForFrame(Clock::time_point frameStart,
                                                Clock::time_point frameEnd,
                                                UiFpsBenchmarkState const & state) const;
        void writeReport(char const * status) const;

        std::filesystem::path m_outputPath;
        std::chrono::seconds m_measurementDuration;
        std::chrono::seconds m_warmupDuration;
        std::chrono::seconds m_readinessTimeout;
        Clock::time_point m_startTime;
        std::optional<Clock::time_point> m_unsettledSince;
        std::optional<Clock::time_point> m_previousFrameStart;
        std::optional<Clock::time_point> m_measurementStart;
        std::optional<Clock::time_point> m_measurementEnd;
        std::optional<double> m_documentLoadedMs;
        std::optional<double> m_firstPreviewMs;
        std::optional<double> m_firstSettledMs;
        std::vector<FrameSample> m_samples;
        std::string m_backend{"unknown"};
        bool m_previousFrameMeasured{false};
        bool m_measurementInterrupted{false};
        bool m_completed{false};
        bool m_timedOut{false};
    };
}
