#include "AgentTraceProfile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

namespace gladius::agent::trace
{
    namespace
    {
        [[nodiscard]] std::int64_t readInt(nlohmann::json const & obj, char const * key)
        {
            auto const it = obj.find(key);
            if (it == obj.end() || !it->is_number())
            {
                return 0;
            }
            return it->get<std::int64_t>();
        }

        [[nodiscard]] std::int64_t readNestedInt(nlohmann::json const & obj,
                                                 char const * outer,
                                                 char const * key)
        {
            auto const found = obj.find(outer);
            if (found == obj.end() || !found->is_object())
            {
                return 0;
            }
            return readInt(*found, key);
        }

        [[nodiscard]] std::string readString(nlohmann::json const & obj, char const * key)
        {
            auto const it = obj.find(key);
            if (it == obj.end() || !it->is_string())
            {
                return {};
            }
            return it->get<std::string>();
        }

        /// @brief Nearest-rank percentile of a sorted sample set
        [[nodiscard]] std::int64_t percentile(std::vector<std::int64_t> const & sorted, double q)
        {
            if (sorted.empty())
            {
                return 0;
            }
            auto const rank = std::ceil(q * static_cast<double>(sorted.size())) - 1.0;
            auto const idx = static_cast<std::size_t>(
              std::clamp<std::int64_t>(static_cast<std::int64_t>(rank), 0,
                                       static_cast<std::int64_t>(sorted.size()) - 1));
            return sorted[idx];
        }

        /// @brief Rate per second from a token count and a duration, 0 when not measurable
        [[nodiscard]] double tokensPerSecond(std::int64_t tokens, std::int64_t ms)
        {
            if (tokens <= 0 || ms <= 0)
            {
                return 0.0;
            }
            return static_cast<double>(tokens) / (static_cast<double>(ms) / 1000.0);
        }
    } // namespace

    nlohmann::json ToolProfileEntry::toJson() const
    {
        return {{"name", name},
                {"calls", calls},
                {"failures", failures},
                {"totalMs", totalMs},
                {"maxMs", maxMs},
                {"avgMs", calls > 0 ? totalMs / static_cast<std::int64_t>(calls) : 0},
                {"totalResultBytes", totalResultBytes}};
    }

    nlohmann::json AgentTraceProfile::toJson() const
    {
        nlohmann::json tools = nlohmann::json::array();
        for (auto const & tool : this->tools)
        {
            tools.push_back(tool.toJson());
        }
        auto const pct = [wall = wallMs](std::int64_t value)
        {
            return wall > 0 ? static_cast<double>(value) * 100.0
                 / static_cast<double>(wall) : 0.0;
        };
        return {{"wallMs", wallMs},
                {"turns", turns},
                {"toolCalls", toolCalls},
                {"timeSplit",
                 {{"contextBuildMs", contextBuildMs},
                  {"modelMs", modelMs},
                  {"modelPrefillMs", modelPrefillMs},
                  {"modelGenerateMs", modelGenerateMs},
                  {"toolMs", toolMs},
                  {"harnessMs", harnessMs},
                  {"unaccountedMs", unaccountedMs},
                  {"modelPercent", std::round(pct(modelMs) * 10.0) / 10.0},
                  {"prefillPercent", std::round(pct(modelPrefillMs) * 10.0) / 10.0},
                  {"generatePercent", std::round(pct(modelGenerateMs) * 10.0) / 10.0},
                  {"toolPercent", std::round(pct(toolMs) * 10.0) / 10.0},
                  {"harnessPercent", std::round(pct(harnessMs) * 10.0) / 10.0},
                  {"unaccountedPercent", std::round(pct(unaccountedMs) * 10.0) / 10.0}}},
                {"turnTimings",
                 {{"durationP50Ms", turnDurationP50Ms},
                  {"durationP95Ms", turnDurationP95Ms},
                  {"durationMaxMs", turnDurationMaxMs},
                  {"ttftP50Ms", ttftP50Ms},
                  {"ttftP95Ms", ttftP95Ms},
                  {"meanDecodeTokensPerSec", std::round(meanDecodeTokensPerSec * 100.0) / 100.0},
                  {"meanPrefillTokensPerSec", std::round(meanPrefillTokensPerSec * 100.0) / 100.0},
                  {"thinkingChars", thinkingChars},
                  {"textChars", textChars},
                  {"toolJsonChars", toolJsonChars}}},
                {"context",
                 {{"firstTurnPromptTokens", firstTurnPromptTokens},
                  {"lastTurnPromptTokens", lastTurnPromptTokens},
                  {"peakTurnPromptTokens", peakTurnPromptTokens},
                  {"totalPromptTokens", totalPromptTokens},
                  {"contextTax", std::round(contextTax * 100.0) / 100.0},
                  {"promptCacheHitRate", std::round(promptCacheHitRate * 1000.0) / 1000.0},
                  {"promptBytesByComponent", promptBytesByComponent},
                  {"largestToolResultBytes", largestToolResultBytes},
                  {"largestToolResultTurn", largestToolResultTurn}}},
                {"tools", std::move(tools)},
                {"waste",
                 {{"truncatedTurns", truncatedTurns},
                  {"failedToolCalls", failedToolCalls},
                  {"deniedToolCalls", deniedToolCalls},
                  {"turnsAfterFailure", turnsAfterFailure},
                  {"turnsAfterFailureMs", turnsAfterFailureMs},
                  {"turnsAfterFailurePromptTokens", turnsAfterFailurePromptTokens},
                  {"turnsWithoutToolCalls", turnsWithoutToolCalls},
                  {"readOnlyTurnsMs", readOnlyTurnsMs},
                  {"judgeMs", judgeMs},
                  {"mutatingToolCalls", mutatingToolCalls},
                  {"autonomousNudges", autonomousNudges},
                  {"truncationNudges", truncationNudges}}}};
    }

    AgentTraceProfile analyzeTrace(std::vector<nlohmann::json> const & records,
                                   std::int64_t wallMs)
    {
        AgentTraceProfile profile;

        // Pass 1: notes carry the attribution (which turn was a retry, which one was nudged).
        std::set<std::int64_t> retryTurns;
        for (auto const & record : records)
        {
            if (record.value("type", std::string{}) != "note")
            {
                continue;
            }
            std::string const kind = readString(record, "kind");
            std::int64_t const turnIndex = readInt(record, "turnIndex");
            if (kind == "retry_after_failure")
            {
                retryTurns.insert(turnIndex);
                ++profile.turnsAfterFailure;
            }
            else if (kind == "autonomous_nudge")
            {
                ++profile.autonomousNudges;
            }
            else if (kind == "truncation_nudge")
            {
                ++profile.truncationNudges;
            }
        }

        std::vector<std::int64_t> turnDurations;
        std::vector<std::int64_t> ttftSamples;
        std::vector<std::int64_t> toolDurations;
        std::map<std::string, ToolProfileEntry> toolEntries;
        std::int64_t cacheReadTokens = 0;
        std::size_t decodableTurns = 0;

        for (auto const & record : records)
        {
            std::string const type = record.value("type", std::string{});
            if (type == "turn")
            {
                ++profile.turns;
                std::int64_t const durationMs = readInt(record, "durationMs");
                std::int64_t const buildMs = readInt(record, "buildMs");
                std::int64_t const completionMs = readInt(record, "completionMs");
                std::int64_t const ttftMs = readInt(record, "ttftMs");
                std::int64_t const firstContentMs = readInt(record, "firstContentMs");
                // A server may send its opening SSE event before it has prefilled the prompt, so the
                // prefill phase only really ends when the first visible output arrives.
                std::int64_t const prefillMs = std::max(ttftMs, firstContentMs);
                std::int64_t const turnIndex = readInt(record, "turnIndex");

                turnDurations.push_back(durationMs);
                profile.contextBuildMs += buildMs;
                profile.modelMs += completionMs > 0 ? completionMs : durationMs - buildMs;
                if (prefillMs > 0)
                {
                    ttftSamples.push_back(prefillMs);
                }
                // Splitting model time this way is what tells you whether to shrink the prompt or
                // speed up generation; the two need completely different fixes.
                profile.modelPrefillMs += std::min(prefillMs, completionMs);
                profile.modelGenerateMs += completionMs > prefillMs ? completionMs - prefillMs : 0;

                std::int64_t const promptTokens = readNestedInt(record, "usage", "totalPromptTokens");
                std::int64_t const outputTokens = readNestedInt(record, "usage", "outputTokens");
                profile.totalPromptTokens += promptTokens;
                cacheReadTokens += readNestedInt(record, "usage", "cacheReadTokens");
                if (turnIndex == 1)
                {
                    profile.firstTurnPromptTokens = promptTokens;
                }
                profile.lastTurnPromptTokens = promptTokens;
                profile.peakTurnPromptTokens = std::max(profile.peakTurnPromptTokens, promptTokens);

                double const prefill = tokensPerSecond(promptTokens, prefillMs);
                double const generationMs = completionMs > prefillMs ? completionMs - prefillMs : 0;
                double const decode = tokensPerSecond(outputTokens, static_cast<std::int64_t>(generationMs));
                if (prefill > 0.0)
                {
                    profile.meanPrefillTokensPerSec += prefill;
                }
                if (decode > 0.0)
                {
                    profile.meanDecodeTokensPerSec += decode;
                    ++decodableTurns;
                }

                profile.thinkingChars += static_cast<std::size_t>(readInt(record, "thinkingChars"));
                profile.textChars += static_cast<std::size_t>(readInt(record, "textChars"));
                profile.toolJsonChars += static_cast<std::size_t>(readInt(record, "toolJsonChars"));
                if (readString(record, "stopReason") == "max_tokens"
                    || readString(record, "stopReason") == "length")
                {
                    ++profile.truncatedTurns;
                }
                if (retryTurns.contains(turnIndex))
                {
                    profile.turnsAfterFailureMs += durationMs;
                    profile.turnsAfterFailurePromptTokens += promptTokens;
                }
                if (readInt(record, "toolCallsIssued") == 0)
                {
                    ++profile.turnsWithoutToolCalls;
                    profile.readOnlyTurnsMs += durationMs;
                }

                if (record.contains("promptBytes") && record["promptBytes"].is_object())
                {
                    // Only the per-component fields are summed; `totalBytes` is their sum already.
                    for (auto const & [key, value] : record["promptBytes"].items())
                    {
                        bool const component = key.size() > 5
                                            && key.compare(key.size() - 5, 5, "Bytes") == 0
                                            && key != "totalBytes";
                        if (component && value.is_number())
                        {
                            profile.promptBytesByComponent[key] =
                              profile.promptBytesByComponent.value(key, 0LL) + value.get<std::int64_t>();
                        }
                    }
                }
            }
            else if (type == "tool_call")
            {
                ++profile.toolCalls;
                std::string name = readString(record, "name");
                if (name.empty())
                {
                    name = "?";
                }
                std::int64_t const durationMs = readInt(record, "durationMs");
                std::int64_t const resultBytes = readInt(record, "resultBytes");
                bool const success = record.value("success", true);

                auto & entry = toolEntries[name];
                entry.name = name;
                ++entry.calls;
                entry.totalMs += durationMs;
                entry.maxMs = std::max(entry.maxMs, durationMs);
                entry.totalResultBytes += resultBytes;
                if (!success)
                {
                    ++entry.failures;
                    ++profile.failedToolCalls;
                }
                if (record.value("denied", false))
                {
                    ++profile.deniedToolCalls;
                }
                // Progress in this harness means the model changed its design, so a turn that only
                // reads is where an optimization has to look for lost iterations.
                std::string const policy = readString(record, "policy");
                if (policy == "design_graph_modification" || policy == "model_modification")
                {
                    ++profile.mutatingToolCalls;
                }
                toolDurations.push_back(durationMs);
                if (resultBytes > profile.largestToolResultBytes)
                {
                    profile.largestToolResultBytes = resultBytes;
                    profile.largestToolResultTurn = static_cast<std::size_t>(readInt(record, "turnIndex"));
                }
            }
            else if (type == "phase")
            {
                std::int64_t const durationMs = readInt(record, "durationMs");
                profile.harnessMs += durationMs;
                // Judging happens after the design is frozen: benchmark cost, not design cost.
                if (readString(record, "name") == "judge")
                {
                    profile.judgeMs += durationMs;
                }
            }
        }

        std::sort(turnDurations.begin(), turnDurations.end());
        std::sort(ttftSamples.begin(), ttftSamples.end());
        profile.turnDurationP50Ms = percentile(turnDurations, 0.5);
        profile.turnDurationP95Ms = percentile(turnDurations, 0.95);
        profile.turnDurationMaxMs = turnDurations.empty() ? 0 : turnDurations.back();
        profile.ttftP50Ms = percentile(ttftSamples, 0.5);
        profile.ttftP95Ms = percentile(ttftSamples, 0.95);

        if (profile.meanPrefillTokensPerSec > 0.0 && !ttftSamples.empty())
        {
            profile.meanPrefillTokensPerSec /= static_cast<double>(ttftSamples.size());
        }
        if (profile.meanDecodeTokensPerSec > 0.0 && decodableTurns > 0)
        {
            profile.meanDecodeTokensPerSec /= static_cast<double>(decodableTurns);
        }
        if (profile.totalPromptTokens > 0 && cacheReadTokens > 0)
        {
            // cacheRead is a subset of the billed prompt total in both supported wire formats.
            profile.promptCacheHitRate =
              static_cast<double>(cacheReadTokens) / static_cast<double>(profile.totalPromptTokens);
        }

        for (auto & [name, entry] : toolEntries)
        {
            (void)name;
            profile.tools.push_back(std::move(entry));
        }
        std::sort(profile.tools.begin(),
                  profile.tools.end(),
                  [](ToolProfileEntry const & a, ToolProfileEntry const & b)
                  {
                      if (a.totalMs != b.totalMs)
                      {
                          return a.totalMs > b.totalMs;
                      }
                      return a.name < b.name;
                  });

        if (profile.totalPromptTokens > 0 && profile.lastTurnPromptTokens > 0)
        {
            profile.contextTax =
              static_cast<double>(profile.totalPromptTokens)
              / static_cast<double>(profile.lastTurnPromptTokens);
        }

        profile.toolMs = std::accumulate(toolDurations.begin(), toolDurations.end(), std::int64_t{0});
        profile.wallMs = wallMs;
        if (profile.wallMs <= 0)
        {
            std::int64_t lastMark = 0;
            for (auto const & record : records)
            {
                lastMark = std::max(lastMark, readInt(record, "tMs"));
            }
            profile.wallMs = lastMark;
        }
        auto const spent = profile.contextBuildMs + profile.modelMs + profile.toolMs
                         + profile.harnessMs;
        profile.unaccountedMs = profile.wallMs > spent ? profile.wallMs - spent : 0;
        return profile;
    }

    std::string formatProfileSummary(AgentTraceProfile const & profile)
    {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1);

        auto const share = [wall = profile.wallMs](std::int64_t ms)
        {
            if (wall <= 0)
            {
                return 0.0;
            }
            return static_cast<double>(ms) * 100.0 / static_cast<double>(wall);
        };

        out << "profile: wall=" << profile.wallMs << "ms turns=" << profile.turns
            << " toolCalls=" << profile.toolCalls << "\n";
        out << "  time split: model=" << profile.modelMs << "ms (" << share(profile.modelMs)
            << "%, prefill " << profile.modelPrefillMs << "ms / generate "
            << profile.modelGenerateMs << "ms) tools=" << profile.toolMs << "ms ("
            << share(profile.toolMs) << "%) harness=" << profile.harnessMs << "ms ("
            << share(profile.harnessMs) << "%, judge " << profile.judgeMs
            << "ms) contextBuild=" << profile.contextBuildMs << "ms unaccounted="
            << profile.unaccountedMs << "ms\n";
        out << "  turns: p50=" << profile.turnDurationP50Ms << "ms p95="
            << profile.turnDurationP95Ms << "ms max=" << profile.turnDurationMaxMs
            << "ms | ttft p50=" << profile.ttftP50Ms << "ms p95=" << profile.ttftP95Ms
            << "ms | decode=" << profile.meanDecodeTokensPerSec << " tok/s prefill="
            << profile.meanPrefillTokensPerSec << " tok/s\n";
        out << "  context: promptTokens first=" << profile.firstTurnPromptTokens << " last="
            << profile.lastTurnPromptTokens << " total=" << profile.totalPromptTokens
            << " tax=" << profile.contextTax << "x cacheHit="
            << (profile.promptCacheHitRate * 100.0) << "%\n";
        out << "  waste: failedToolCalls=" << profile.failedToolCalls << " denied="
            << profile.deniedToolCalls << " turnsAfterFailure=" << profile.turnsAfterFailure
            << " (" << profile.turnsAfterFailureMs << "ms, "
            << profile.turnsAfterFailurePromptTokens << " prompt tokens) truncatedTurns="
            << profile.truncatedTurns << " nudges=" << profile.autonomousNudges
            << "/" << profile.truncationNudges
            << " readOnlyTurns=" << profile.turnsWithoutToolCalls << " ("
            << profile.readOnlyTurnsMs << "ms) mutatingToolCalls="
            << profile.mutatingToolCalls << "\n";
        out << "  slowest tools:";
        std::size_t shown = 0;
        for (auto const & tool : profile.tools)
        {
            if (shown++ == 5)
            {
                break;
            }
            out << "\n    " << tool.name << ": " << tool.totalMs << "ms total, " << tool.calls
                << " calls, avg " << (tool.calls > 0 ? tool.totalMs / static_cast<std::int64_t>(tool.calls) : 0)
                << "ms, max " << tool.maxMs << "ms, failures " << tool.failures;
        }
        return out.str();
    }
} // namespace gladius::agent::trace
