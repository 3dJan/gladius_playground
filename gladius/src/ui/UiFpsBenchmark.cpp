#include "UiFpsBenchmark.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace gladius::ui
{
    namespace
    {
        void writeOptional(std::ostream & output, std::optional<double> const & value)
        {
            if (value.has_value())
            {
                output << *value;
            }
            else
            {
                output << "null";
            }
        }
    }

    UiFpsBenchmark::UiFpsBenchmark(std::filesystem::path outputPath,
                                   std::chrono::seconds measurementDuration,
                                   std::chrono::seconds warmupDuration,
                                   std::chrono::seconds readinessTimeout)
        : m_outputPath{std::move(outputPath)}
        , m_measurementDuration{measurementDuration}
        , m_warmupDuration{warmupDuration}
        , m_readinessTimeout{readinessTimeout}
        , m_startTime{Clock::now()}
        , m_unsettledSince{m_startTime}
    {
        if (m_outputPath.empty() || m_measurementDuration <= std::chrono::seconds::zero() ||
            m_warmupDuration < std::chrono::seconds::zero() ||
            m_readinessTimeout <= std::chrono::seconds::zero())
        {
            throw std::invalid_argument("Invalid UI FPS benchmark configuration");
        }

        auto const reserveCount = static_cast<std::size_t>(
          std::min<std::int64_t>(m_measurementDuration.count() * 120 + 1000, 100000));
        m_samples.reserve(reserveCount);
    }

    bool UiFpsBenchmark::recordFrame(Clock::time_point frameStart,
                                     Clock::time_point frameEnd,
                                     UiFpsBenchmarkState const & state,
                                     std::uint32_t displayWidth,
                                     std::uint32_t displayHeight,
                                     std::array<float, 2> framebufferScale,
                                     bool animationMode)
    {
        if (m_completed)
        {
            return true;
        }

        auto const elapsedMs = std::chrono::duration<double, std::milli>(frameEnd - m_startTime).count();
        if (state.documentLoaded && !m_documentLoadedMs.has_value())
        {
            m_documentLoadedMs = elapsedMs;
        }
        if (state.previewPresented && !m_firstPreviewMs.has_value())
        {
            m_firstPreviewMs = elapsedMs;
        }
        if (state.backend != "unknown")
        {
            m_backend = state.backend;
        }

        bool const settled = state.documentLoaded && state.previewPresented && state.renderSettled;
        auto const phase = phaseForFrame(frameStart, frameEnd, state);
        if (settled)
        {
            m_unsettledSince.reset();
            m_measurementInterrupted = false;
        }
        else if (!m_unsettledSince.has_value())
        {
            m_unsettledSince = frameStart;
        }
        if (settled && !m_firstSettledMs.has_value())
        {
            m_firstSettledMs = elapsedMs;
        }
        if (settled && !m_measurementStart.has_value())
        {
            m_measurementStart = frameEnd + m_warmupDuration;
            m_measurementEnd = *m_measurementStart + m_measurementDuration;
        }
        else if (!settled && m_measurementStart.has_value())
        {
            m_measurementInterrupted = m_measurementInterrupted || frameStart >= *m_measurementStart;
            m_measurementStart.reset();
            m_measurementEnd.reset();
        }

        FrameSample sample;
        sample.frameIndex = static_cast<std::uint64_t>(m_samples.size());
        sample.elapsedMs = elapsedMs;
        if (m_previousFrameStart.has_value())
        {
            sample.intervalMs =
              std::chrono::duration<double, std::milli>(frameStart - *m_previousFrameStart).count();
        }
        sample.renderMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
        sample.phase = phase;
        sample.documentLoaded = state.documentLoaded;
        sample.previewPresented = state.previewPresented;
        sample.renderSettled = state.renderSettled;
        sample.animationMode = animationMode;
        sample.displaySize = {displayWidth, displayHeight};
        sample.previewViewportPx = state.previewViewportPx;
        sample.framebufferScale = framebufferScale;
        sample.uiScale = state.uiScale;
        sample.stageTimings = state.stageTimings;
        sample.measured = settled && m_measurementStart.has_value() && m_measurementEnd.has_value() &&
                          frameStart >= *m_measurementStart && frameStart < *m_measurementEnd;
        sample.measuredInterval = sample.measured && m_previousFrameMeasured;
        m_samples.push_back(std::move(sample));
        m_previousFrameStart = frameStart;
        m_previousFrameMeasured = m_samples.back().measured;

        if (m_unsettledSince.has_value() && frameEnd - *m_unsettledSince >= m_readinessTimeout)
        {
            m_timedOut = true;
            m_completed = true;
            writeReport("readiness_timeout");
            return true;
        }
        if (m_measurementEnd.has_value() && frameEnd >= *m_measurementEnd)
        {
            m_completed = true;
            writeReport("complete");
            return true;
        }
        return false;
    }

    std::filesystem::path const & UiFpsBenchmark::outputPath() const noexcept
    {
        return m_outputPath;
    }

    bool UiFpsBenchmark::didTimeOut() const noexcept
    {
        return m_timedOut;
    }

    double UiFpsBenchmark::percentile(std::vector<double> values, double fraction)
    {
        if (values.empty())
        {
            return 0.0;
        }
        std::sort(values.begin(), values.end());
                auto const rank = std::ceil(std::clamp(fraction, 0.0, 1.0) *
                                                                        static_cast<double>(values.size()));
                auto const index = rank <= 1.0 ? 0u : static_cast<std::size_t>(rank - 1.0);
        return values[std::min(index, values.size() - 1u)];
    }

    std::string UiFpsBenchmark::jsonEscape(std::string const & value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (auto const character : value)
        {
            switch (character)
            {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += character;
                break;
            }
        }
        return escaped;
    }

    std::string UiFpsBenchmark::phaseForFrame(Clock::time_point frameStart,
                                               Clock::time_point frameEnd,
                                               UiFpsBenchmarkState const & state) const
    {
        if (!state.documentLoaded)
        {
            return "loading";
        }
        if (!state.previewPresented)
        {
            return "waiting_for_preview";
        }
        if (!state.renderSettled)
        {
                        return m_measurementInterrupted ||
                                             (m_measurementStart.has_value() && frameStart >= *m_measurementStart)
                                         ? "interrupted"
                                         : "rendering";
        }
        if (!m_measurementStart.has_value() || frameEnd < *m_measurementStart)
        {
            return "warmup";
        }
        if (!m_measurementEnd.has_value() || frameStart < *m_measurementEnd)
        {
            return "measurement";
        }
        return "complete";
    }

    void UiFpsBenchmark::writeReport(char const * status) const
    {
        if (m_outputPath.has_parent_path())
        {
            std::filesystem::create_directories(m_outputPath.parent_path());
        }

        std::ofstream output{m_outputPath, std::ios::out | std::ios::trunc};
        if (!output)
        {
            throw std::runtime_error("Unable to open UI FPS benchmark report: " + m_outputPath.string());
        }
        output << std::fixed << std::setprecision(4);

        std::vector<double> frameIntervals;
        std::vector<double> renderDurations;
        std::array<std::vector<double>, 14> renderingStageDurations;
        std::size_t renderingFrameCount = 0u;
        std::size_t over16Ms = 0u;
        std::size_t over8Ms = 0u;
        for (auto const & sample : m_samples)
        {
            if (sample.phase == "rendering")
            {
                ++renderingFrameCount;
                auto const & stages = sample.stageTimings;
                renderingStageDurations[0].push_back(stages.preDisplayHookMs);
                renderingStageDurations[1].push_back(stages.viewCallbacksMs);
                renderingStageDurations[2].push_back(stages.uiDisplayMs);
                renderingStageDurations[3].push_back(stages.neutralBackendMs);
                renderingStageDurations[4].push_back(stages.sceneSynchronizationMs);
                renderingStageDurations[5].push_back(stages.boundsUpdateMs);
                renderingStageDurations[6].push_back(stages.schedulerPollMs);
                renderingStageDurations[7].push_back(stages.completedTaskLatencyMs);
                renderingStageDurations[8].push_back(stages.framePresentationMs);
                renderingStageDurations[9].push_back(stages.requestFactoryMs);
                renderingStageDurations[10].push_back(stages.backendSubmitMs);
                renderingStageDurations[11].push_back(stages.taskSubmissionMs);
                renderingStageDurations[12].push_back(stages.backendSceneReplacementMs);
                renderingStageDurations[13].push_back(stages.bufferSwapMs);
            }
            if (sample.measured)
            {
                renderDurations.push_back(sample.renderMs);
            }
            if (!sample.measuredInterval || !sample.intervalMs.has_value() ||
                !std::isfinite(*sample.intervalMs) || *sample.intervalMs <= 0.0)
            {
                continue;
            }
            frameIntervals.push_back(*sample.intervalMs);
            over16Ms += *sample.intervalMs > 16.6667 ? 1u : 0u;
            over8Ms += *sample.intervalMs > 8.3333 ? 1u : 0u;
        }

        auto const meanIntervalMs = frameIntervals.empty()
                                      ? 0.0
                                      : std::accumulate(frameIntervals.begin(), frameIntervals.end(), 0.0) /
                                          static_cast<double>(frameIntervals.size());
        auto const effectiveFps = meanIntervalMs > 0.0 ? 1000.0 / meanIntervalMs : 0.0;

        output << "{\n"
               << "  \"status\": \"" << status << "\",\n"
               << "  \"backend\": \"" << jsonEscape(m_backend) << "\",\n"
               << "  \"warmupSeconds\": " << m_warmupDuration.count() << ",\n"
               << "  \"measurementSeconds\": " << m_measurementDuration.count() << ",\n"
               << "  \"milestonesMs\": {\n"
               << "    \"documentLoaded\": ";
        writeOptional(output, m_documentLoadedMs);
        output << ",\n    \"firstPreviewPresented\": ";
        writeOptional(output, m_firstPreviewMs);
        output << ",\n    \"firstSettledFrame\": ";
        writeOptional(output, m_firstSettledMs);
        output << "\n  },\n"
               << "  \"measurement\": {\n"
               << "    \"intervalCount\": " << frameIntervals.size() << ",\n"
               << "    \"effectiveFps\": " << effectiveFps << ",\n"
               << "    \"frameIntervalMs\": {\n"
               << "      \"mean\": " << meanIntervalMs << ",\n"
               << "      \"p50\": " << percentile(frameIntervals, 0.50) << ",\n"
               << "      \"p95\": " << percentile(frameIntervals, 0.95) << ",\n"
               << "      \"p99\": " << percentile(frameIntervals, 0.99) << ",\n"
               << "      \"max\": " << percentile(frameIntervals, 1.0) << "\n"
               << "    },\n"
               << "    \"renderDurationMs\": {\n"
               << "      \"p50\": " << percentile(renderDurations, 0.50) << ",\n"
               << "      \"p95\": " << percentile(renderDurations, 0.95) << ",\n"
               << "      \"p99\": " << percentile(renderDurations, 0.99) << ",\n"
               << "      \"max\": " << percentile(renderDurations, 1.0) << "\n"
               << "    },\n"
               << "    \"intervalsOver16_6667ms\": " << over16Ms << ",\n"
               << "    \"intervalsOver8_3333ms\": " << over8Ms << "\n"
               << "  },\n"
               << "  \"renderingStageDurationsMs\": {\n"
               << "    \"frameCount\": " << renderingFrameCount << ",\n";

                        std::array<char const *, 14> const stageNames{
              "preDisplayHook", "viewCallbacks", "uiDisplay", "neutralBackend",
              "sceneSynchronization", "boundsUpdate", "schedulerPoll", "completedTaskLatency",
                              "framePresentation", "requestFactory", "backendSubmit", "taskSubmission",
                              "backendSceneReplacement", "bufferSwap"};
        for (std::size_t index = 0u; index < stageNames.size(); ++index)
            {
            auto const & durations = renderingStageDurations[index];
            output << "    \"" << stageNames[index] << "\": {\"p50\": "
                   << percentile(durations, 0.50) << ", \"p95\": "
                   << percentile(durations, 0.95) << ", \"p99\": "
                   << percentile(durations, 0.99) << ", \"max\": "
                   << percentile(durations, 1.0) << "}"
                   << (index + 1u == stageNames.size() ? "\n" : ",\n");
        }
        output << "  },\n"
               << "  \"frames\": [\n";

        for (std::size_t index = 0u; index < m_samples.size(); ++index)
        {
            auto const & sample = m_samples[index];
            output << "    {\"frame\": " << sample.frameIndex << ", \"elapsedMs\": "
                   << sample.elapsedMs << ", \"intervalMs\": ";
            if (sample.intervalMs.has_value())
            {
                output << *sample.intervalMs;
            }
            else
            {
                output << "null";
            }
            output << ", \"renderMs\": " << sample.renderMs
                   << ", \"phase\": \"" << sample.phase << "\", \"documentLoaded\": "
                   << (sample.documentLoaded ? "true" : "false")
                   << ", \"previewPresented\": " << (sample.previewPresented ? "true" : "false")
                   << ", \"renderSettled\": " << (sample.renderSettled ? "true" : "false")
                   << ", \"measured\": " << (sample.measured ? "true" : "false")
                   << ", \"measuredInterval\": " << (sample.measuredInterval ? "true" : "false")
                   << ", \"animationMode\": " << (sample.animationMode ? "true" : "false")
                   << ", \"displaySize\": [" << sample.displaySize[0] << ", " << sample.displaySize[1]
                   << "], \"previewViewportPx\": [" << sample.previewViewportPx[0] << ", "
                   << sample.previewViewportPx[1] << "], \"framebufferScale\": [" << sample.framebufferScale[0] << ", "
                   << sample.framebufferScale[1] << "], \"uiScale\": " << sample.uiScale
                   << ", \"stagesMs\": {\"preDisplayHook\": "
                   << sample.stageTimings.preDisplayHookMs << ", \"viewCallbacks\": "
                   << sample.stageTimings.viewCallbacksMs << ", \"uiDisplay\": "
                   << sample.stageTimings.uiDisplayMs << ", \"neutralBackend\": "
                   << sample.stageTimings.neutralBackendMs << ", \"sceneSynchronization\": "
                   << sample.stageTimings.sceneSynchronizationMs << ", \"boundsUpdate\": "
                   << sample.stageTimings.boundsUpdateMs << ", \"schedulerPoll\": "
                   << sample.stageTimings.schedulerPollMs << ", \"completedTaskLatency\": "
                   << sample.stageTimings.completedTaskLatencyMs << ", \"framePresentation\": "
                   << sample.stageTimings.framePresentationMs << ", \"requestFactory\": "
                   << sample.stageTimings.requestFactoryMs << ", \"backendSubmit\": "
                   << sample.stageTimings.backendSubmitMs << ", \"taskSubmission\": "
                   << sample.stageTimings.taskSubmissionMs << ", \"backendSceneReplacement\": "
                   << sample.stageTimings.backendSceneReplacementMs << ", \"bufferSwap\": "
                   << sample.stageTimings.bufferSwapMs << "}}"
                   << (index + 1u == m_samples.size() ? "\n" : ",\n");
        }
        output << "  ]\n}\n";
        if (!output)
        {
            throw std::runtime_error("Failed while writing UI FPS benchmark report: " + m_outputPath.string());
        }
    }
}
