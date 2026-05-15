#include "RealtimeRaymarchController.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace gladius::ui::async_rendering
{
    namespace
    {
        [[nodiscard]] std::string normalizeModeString(std::string value)
        {
            std::transform(value.begin(),
                           value.end(),
                           value.begin(),
                           [](unsigned char character)
                           { return static_cast<char>(std::tolower(character)); });
            return value;
        }
    }

    RealtimeRaymarchMode realtimeRaymarchModeFromString(std::string const & value)
    {
        auto const normalized = normalizeModeString(value);
        if (normalized == "off" || normalized == "disabled" || normalized == "false")
        {
            return RealtimeRaymarchMode::Off;
        }
        if (normalized == "force" || normalized == "forced" || normalized == "on")
        {
            return RealtimeRaymarchMode::Force;
        }
        return RealtimeRaymarchMode::Auto;
    }

    char const * realtimeRaymarchModeToString(RealtimeRaymarchMode mode) noexcept
    {
        switch (mode)
        {
        case RealtimeRaymarchMode::Off:
            return "off";
        case RealtimeRaymarchMode::Force:
            return "force";
        case RealtimeRaymarchMode::Auto:
        default:
            return "auto";
        }
    }

    void RealtimeRaymarchController::configure(RealtimeRaymarchConfig config)
    {
        config.targetFrameTimeMs = std::max(config.targetFrameTimeMs, 1.0f);
        config.enterBudgetRatio = std::clamp(config.enterBudgetRatio, 0.1f, 1.0f);
        config.exitBudgetRatio = std::max(config.exitBudgetRatio, config.enterBudgetRatio);
        config.severeMissRatio = std::max(config.severeMissRatio, config.exitBudgetRatio);
        config.ewmaAlpha = std::clamp(config.ewmaAlpha, 0.01f, 1.0f);
        config.requiredFastSamples = std::max(config.requiredFastSamples, 1);
        config.maxSlowSamples = std::max(config.maxSlowSamples, 1);
        config.cooldownFrames = std::max(config.cooldownFrames, 0);
        config.staticFullFrameBudgetMs = std::max(config.staticFullFrameBudgetMs, config.targetFrameTimeMs);
        config.realtimeDropBudgetMs = std::max(config.realtimeDropBudgetMs, config.targetFrameTimeMs);

        m_config = config;
        if (m_config.mode == RealtimeRaymarchMode::Off)
        {
            m_realtimeActive = false;
        }
    }

    RealtimeRaymarchConfig const & RealtimeRaymarchController::config() const noexcept
    {
        return m_config;
    }

    void RealtimeRaymarchController::reset()
    {
        m_estimatedFullFrameMs.reset();
        m_width = 0;
        m_height = 0;
        m_fastSampleStreak = 0;
        m_slowSampleStreak = 0;
        m_cooldownFramesRemaining = 0;
        m_realtimeActive = false;
        m_staticFullFramePreferred = false;
    }

    void RealtimeRaymarchController::resetForResolution(uint32_t width, uint32_t height)
    {
        if (m_width == width && m_height == height)
        {
            return;
        }

        auto const previousEstimate = m_estimatedFullFrameMs;
        auto const previousWidth = m_width;
        auto const previousHeight = m_height;
        bool const wasRealtimeActive = m_realtimeActive;

        reset();
        m_width = width;
        m_height = height;

        if (!previousEstimate.has_value() || previousWidth == 0 || previousHeight == 0 || width == 0 || height == 0)
        {
            return;
        }

        auto const previousPixels = static_cast<double>(previousWidth) * static_cast<double>(previousHeight);
        auto const currentPixels = static_cast<double>(width) * static_cast<double>(height);
        if (previousPixels <= 0.0 || currentPixels <= 0.0)
        {
            return;
        }

        auto const scale = std::clamp(currentPixels / previousPixels, 0.1, 10.0);
        auto const scaledEstimate = static_cast<float>(*previousEstimate * scale);
        if (!std::isfinite(scaledEstimate) || scaledEstimate <= 0.0f)
        {
            return;
        }

        m_estimatedFullFrameMs = scaledEstimate;

        auto const enterBudget = m_config.targetFrameTimeMs * m_config.enterBudgetRatio;
        auto const exitBudget = m_config.targetFrameTimeMs * m_config.exitBudgetRatio;
        if (scaledEstimate <= enterBudget || (wasRealtimeActive && scaledEstimate <= exitBudget))
        {
            m_fastSampleStreak = m_config.requiredFastSamples;
            m_realtimeActive = m_config.mode == RealtimeRaymarchMode::Auto;
            m_staticFullFramePreferred = true;
        }
        else if (scaledEstimate <= m_config.staticFullFrameBudgetMs)
        {
            m_staticFullFramePreferred = true;
        }
    }

    void RealtimeRaymarchController::beginFrame()
    {
        if (m_cooldownFramesRemaining > 0)
        {
            --m_cooldownFramesRemaining;
        }
    }

    void RealtimeRaymarchController::recordStaticProgressiveSample(RealtimeRaymarchSample const & sample)
    {
        if (m_config.mode != RealtimeRaymarchMode::Auto || sample.cancelled)
        {
            return;
        }

        if (sample.width == 0 || sample.height == 0)
        {
            return;
        }

        if (!hasValidResolution(sample.width, sample.height))
        {
            resetForResolution(sample.width, sample.height);
        }

        auto const estimatedMs = estimateSampleFullFrameTimeMs(sample);
        if (!estimatedMs.has_value())
        {
            return;
        }

        recordEstimatedTime(*estimatedMs);
        m_staticFullFramePreferred = *estimatedMs <= m_config.staticFullFrameBudgetMs;
        if (m_staticFullFramePreferred)
        {
            m_slowSampleStreak = 0;
        }
    }

    void RealtimeRaymarchController::recordStaticFullFrameSample(RealtimeRaymarchSample const & sample)
    {
        if (m_config.mode != RealtimeRaymarchMode::Auto || sample.cancelled || !sample.completedFrame)
        {
            return;
        }

        if (sample.width == 0 || sample.height == 0)
        {
            return;
        }

        if (!hasValidResolution(sample.width, sample.height))
        {
            resetForResolution(sample.width, sample.height);
        }

        auto const estimatedMs = estimateSampleFullFrameTimeMs(sample);
        if (!estimatedMs.has_value())
        {
            return;
        }

        recordEstimatedTime(*estimatedMs);
        m_staticFullFramePreferred = *estimatedMs <= m_config.staticFullFrameBudgetMs;

        if (*estimatedMs <= m_config.targetFrameTimeMs * m_config.enterBudgetRatio)
        {
            ++m_fastSampleStreak;
            m_slowSampleStreak = 0;
            if (m_fastSampleStreak >= m_config.requiredFastSamples)
            {
                m_realtimeActive = true;
            }
            return;
        }

        m_fastSampleStreak = 0;
        if (*estimatedMs > m_config.realtimeDropBudgetMs)
        {
            deactivateRealtime();
        }
    }

    void RealtimeRaymarchController::recordInteractiveRealtimeSample(RealtimeRaymarchSample const & sample)
    {
        if (m_config.mode != RealtimeRaymarchMode::Auto || sample.cancelled || !sample.completedFrame)
        {
            return;
        }

        if (sample.width == 0 || sample.height == 0)
        {
            return;
        }

        if (!hasValidResolution(sample.width, sample.height))
        {
            resetForResolution(sample.width, sample.height);
        }

        auto const estimatedMs = estimateSampleFullFrameTimeMs(sample);
        if (!estimatedMs.has_value())
        {
            return;
        }

        recordEstimatedTime(*estimatedMs);
        if (*estimatedMs > m_config.realtimeDropBudgetMs)
        {
            deactivateRealtime();
        }
    }

    void RealtimeRaymarchController::recordRejectedAttempt()
    {
        deactivateRealtime();
        m_fastSampleStreak = 0;
        ++m_slowSampleStreak;
        if (m_slowSampleStreak >= m_config.maxSlowSamples)
        {
            enterCooldown();
        }
    }

    bool RealtimeRaymarchController::canAttemptRealtime(
      uint32_t width,
      uint32_t height,
      RealtimeRaymarchGuards const & guards) const noexcept
    {
        if (m_config.mode == RealtimeRaymarchMode::Off)
        {
            return false;
        }
        if (!hasValidResolution(width, height))
        {
            return false;
        }
        if (!guardsAllowAttempt(guards))
        {
            return false;
        }
        if (m_config.mode == RealtimeRaymarchMode::Force)
        {
            return true;
        }
        if (m_cooldownFramesRemaining > 0)
        {
            return false;
        }
        return m_realtimeActive;
    }

    bool RealtimeRaymarchController::canAttemptStaticFullFrame(
      uint32_t width,
      uint32_t height,
      RealtimeRaymarchGuards const & guards) const noexcept
    {
        if (m_config.mode != RealtimeRaymarchMode::Auto)
        {
            return false;
        }
        if (!m_staticFullFramePreferred || m_cooldownFramesRemaining > 0)
        {
            return false;
        }
        if (!hasValidResolution(width, height))
        {
            return false;
        }
        return guardsAllowAttempt(guards);
    }

    bool RealtimeRaymarchController::isRealtimeActive() const noexcept
    {
        return m_realtimeActive;
    }

    bool RealtimeRaymarchController::isCoolingDown() const noexcept
    {
        return m_cooldownFramesRemaining > 0;
    }

    std::optional<float> RealtimeRaymarchController::estimatedFullFrameTimeMs() const noexcept
    {
        return m_estimatedFullFrameMs;
    }

    char const * RealtimeRaymarchController::modeLabel() const noexcept
    {
        if (m_config.mode == RealtimeRaymarchMode::Force)
        {
            return "RT Force";
        }
        if (m_config.mode == RealtimeRaymarchMode::Off)
        {
            return "RT Off";
        }
        if (m_realtimeActive)
        {
            return "RT Auto";
        }
        if (m_cooldownFramesRemaining > 0)
        {
            return "RT Cooldown";
        }
        return "RT Learning";
    }

    bool RealtimeRaymarchController::hasValidResolution(uint32_t width, uint32_t height) const noexcept
    {
        return width > 0 && height > 0 && m_width == width && m_height == height;
    }

    bool RealtimeRaymarchController::guardsAllowAttempt(
      RealtimeRaymarchGuards const & guards) const noexcept
    {
        return !guards.hardBlocker && !guards.renderJobInFlight && !guards.previewJobInFlight &&
               !guards.streamingActive && !guards.streamingJobInFlight && !guards.resizePending;
    }

    std::optional<float> RealtimeRaymarchController::estimateSampleFullFrameTimeMs(
      RealtimeRaymarchSample const & sample) const noexcept
    {
        if (sample.durationMs <= 0.0f || !std::isfinite(sample.durationMs))
        {
            return std::nullopt;
        }

        if (sample.completedFrame)
        {
            return sample.durationMs;
        }

        if (sample.renderedLines == 0 || sample.totalLines == 0)
        {
            return std::nullopt;
        }

        auto const clampedLines = std::min(sample.renderedLines, sample.totalLines);
        if (clampedLines == 0)
        {
            return std::nullopt;
        }

        auto const scale = static_cast<float>(sample.totalLines) / static_cast<float>(clampedLines);
        return sample.durationMs * scale;
    }

    void RealtimeRaymarchController::recordEstimatedTime(float estimatedMs)
    {
        if (!m_estimatedFullFrameMs.has_value())
        {
            m_estimatedFullFrameMs = estimatedMs;
        }
        else
        {
            m_estimatedFullFrameMs = (*m_estimatedFullFrameMs * (1.0f - m_config.ewmaAlpha)) +
                                     (estimatedMs * m_config.ewmaAlpha);
        }
    }

    void RealtimeRaymarchController::deactivateRealtime()
    {
        m_realtimeActive = false;
        m_fastSampleStreak = 0;
        m_staticFullFramePreferred = false;
    }

    void RealtimeRaymarchController::enterCooldown()
    {
        deactivateRealtime();
        m_fastSampleStreak = 0;
        m_slowSampleStreak = 0;
        m_cooldownFramesRemaining = m_config.cooldownFrames;
    }
}
