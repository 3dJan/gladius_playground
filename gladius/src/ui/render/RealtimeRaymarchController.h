#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace gladius::ui::async_rendering
{
    enum class RealtimeRaymarchMode
    {
        Off,
        Auto,
        Force
    };

    [[nodiscard]] RealtimeRaymarchMode realtimeRaymarchModeFromString(std::string const & value);
    [[nodiscard]] char const * realtimeRaymarchModeToString(RealtimeRaymarchMode mode) noexcept;

    struct RealtimeRaymarchConfig
    {
        RealtimeRaymarchMode mode{RealtimeRaymarchMode::Auto};
        float targetFrameTimeMs{25.0f};
        float enterBudgetRatio{0.8f};
        float exitBudgetRatio{1.1f};
        float severeMissRatio{2.0f};
        float ewmaAlpha{0.35f};
        int requiredFastSamples{3};
        int maxSlowSamples{2};
        int cooldownFrames{30};
    };

    struct RealtimeRaymarchGuards
    {
        bool hardBlocker{false};
        bool renderJobInFlight{false};
        bool previewJobInFlight{false};
        bool streamingActive{false};
        bool streamingJobInFlight{false};
        bool resizePending{false};
    };

    struct RealtimeRaymarchSample
    {
        float durationMs{0.0f};
        uint32_t width{0};
        uint32_t height{0};
        size_t renderedLines{0};
        size_t totalLines{0};
        bool completedFrame{false};
        bool cancelled{false};
    };

    class RealtimeRaymarchController
    {
      public:
        void configure(RealtimeRaymarchConfig config);
        [[nodiscard]] RealtimeRaymarchConfig const & config() const noexcept;

        void reset();
        void resetForResolution(uint32_t width, uint32_t height);
        void beginFrame();

        void recordSample(RealtimeRaymarchSample const & sample);
        void recordRejectedAttempt();

        [[nodiscard]] bool canAttemptRealtime(uint32_t width,
                                              uint32_t height,
                                              RealtimeRaymarchGuards const & guards) const noexcept;
        [[nodiscard]] bool isRealtimeActive() const noexcept;
        [[nodiscard]] bool isCoolingDown() const noexcept;
        [[nodiscard]] std::optional<float> estimatedFullFrameTimeMs() const noexcept;
        [[nodiscard]] char const * modeLabel() const noexcept;

      private:
        [[nodiscard]] bool hasValidResolution(uint32_t width, uint32_t height) const noexcept;
        [[nodiscard]] bool guardsAllowAttempt(RealtimeRaymarchGuards const & guards) const noexcept;
        [[nodiscard]] std::optional<float> estimateSampleFullFrameTimeMs(
          RealtimeRaymarchSample const & sample) const noexcept;
        void recordEstimatedTime(float estimatedMs);
        void enterCooldown();

        RealtimeRaymarchConfig m_config{};
        std::optional<float> m_estimatedFullFrameMs{};
        uint32_t m_width{0};
        uint32_t m_height{0};
        int m_fastSampleStreak{0};
        int m_slowSampleStreak{0};
        int m_cooldownFramesRemaining{0};
        bool m_realtimeActive{false};
    };
}
