#include "AgentTrace.h"

#include "../Conversation.h"

#include <algorithm>
#include <system_error>

namespace gladius::agent::trace
{
    namespace
    {
        /// @brief Chrome trace events are in microseconds since the epoch of the trace
        constexpr std::int64_t kMicrosecondsPerMillisecond = 1000;

        [[nodiscard]] std::int64_t readInt(nlohmann::json const & obj, char const * key)
        {
            auto const it = obj.find(key);
            if (it == obj.end() || !it->is_number())
            {
                return 0;
            }
            return it->get<std::int64_t>();
        }

        /// @brief Timeline lane for a record type, so harness work and agent work do not overlap
        [[nodiscard]] char const * timelineThread(char const * type)
        {
            return std::string_view{type} == "phase" ? "harness" : "agent";
        }
    } // namespace

    nlohmann::json RequestSizeBreakdown::toJson() const
    {
        return {{"systemPromptBytes", systemPromptBytes},
                {"toolSchemaBytes", toolSchemaBytes},
                {"userTextBytes", userTextBytes},
                {"assistantTextBytes", assistantTextBytes},
                {"toolCallBytes", toolCallBytes},
                {"toolResultBytes", toolResultBytes},
                {"imageBytes", imageBytes},
                {"messages", messages},
                {"toolDefinitions", toolDefinitions},
                {"images", images},
                {"totalBytes", totalBytes()}};
    }

    std::int64_t RequestSizeBreakdown::totalBytes() const noexcept
    {
        return systemPromptBytes + toolSchemaBytes + userTextBytes + assistantTextBytes
             + toolCallBytes + toolResultBytes + imageBytes;
    }

    RequestSizeBreakdown breakdownRequest(CompletionRequest const & request)
    {
        RequestSizeBreakdown out;
        out.systemPromptBytes = static_cast<std::int64_t>(request.systemPrompt.size());
        out.toolDefinitions = request.tools.size();
        for (auto const & tool : request.tools)
        {
            out.toolSchemaBytes += static_cast<std::int64_t>(tool.name.size() + tool.description.size()
                                   + tool.inputSchema.dump().size());
        }

        out.messages = request.messages.size();
        for (auto const & msg : request.messages)
        {
            for (auto const & part : msg.parts)
            {
                if (auto const * text = std::get_if<TextContent>(&part))
                {
                    // Assistant text is what the model already paid to produce; user text is context
                    // the harness injects. Keeping them apart shows whether history or scaffolding
                    // is what grows the prompt.
                    auto & target = msg.role == MessageRole::Assistant ? out.assistantTextBytes
                                                                       : out.userTextBytes;
                    target += static_cast<std::int64_t>(text->text.size());
                }
                else if (auto const * image = std::get_if<ImageContent>(&part))
                {
                    out.imageBytes += static_cast<std::int64_t>(image->data.size());
                    ++out.images;
                }
                else if (auto const * call = std::get_if<ToolCall>(&part))
                {
                    out.toolCallBytes += static_cast<std::int64_t>(call->arguments.dump().size());
                }
                else if (auto const * result = std::get_if<ToolResult>(&part))
                {
                    out.toolResultBytes += static_cast<std::int64_t>(result->value.dump().size());
                }
            }
        }
        return out;
    }

    std::int64_t systemPromptHash(std::string const & systemPrompt)
    {
        // FNV-1a 64-bit. Only used to spot turns where the prompt prefix changed, which is what
        // throws away the provider's prompt cache.
        std::uint64_t hash = 1469598103934665603ULL;
        for (char c : systemPrompt)
        {
            hash ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
            hash *= 1099511628211ULL;
        }
        return static_cast<std::int64_t>(hash);
    }

    AgentTraceRecorder::AgentTraceRecorder(std::filesystem::path outputFile)
        : m_outputFile(std::move(outputFile))
    {
        if (m_outputFile.empty())
        {
            return;
        }
        if (auto const parent = m_outputFile.parent_path(); !parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
        m_stream.open(m_outputFile, std::ios::out | std::ios::trunc);
    }

    void AgentTraceRecorder::setPromptDumpEnabled(bool enabled) noexcept
    {
        m_promptDumpEnabled = enabled;
    }

    bool AgentTraceRecorder::promptDumpEnabled() const noexcept
    {
        return m_promptDumpEnabled;
    }

    void AgentTraceRecorder::setRunId(std::string runId)
    {
        m_runId = std::move(runId);
    }

    std::int64_t AgentTraceRecorder::mark() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - m_start)
          .count();
    }

    void AgentTraceRecorder::emit(nlohmann::json record)
    {
        record["tMs"] = mark();
        if (!m_runId.empty())
        {
            record["runId"] = m_runId;
        }
        m_records.push_back(record);
        if (m_stream.is_open())
        {
            m_stream << record.dump() << '\n';
            m_stream.flush();
        }
    }

    void AgentTraceRecorder::recordRunMeta(nlohmann::json fields)
    {
        fields["type"] = "run_meta";
        fields["promptDump"] = m_promptDumpEnabled;
        emit(std::move(fields));
    }

    void AgentTraceRecorder::recordTurn(nlohmann::json fields)
    {
        fields["type"] = "turn";
        emit(std::move(fields));
    }

    void AgentTraceRecorder::recordToolCall(nlohmann::json fields)
    {
        fields["type"] = "tool_call";
        emit(std::move(fields));
    }

    void AgentTraceRecorder::recordNote(std::string const & kind, nlohmann::json fields)
    {
        fields["type"] = "note";
        fields["kind"] = kind;
        emit(std::move(fields));
    }

    void AgentTraceRecorder::recordPhase(std::string const & name,
                                         std::int64_t startedMark,
                                         nlohmann::json fields)
    {
        fields["type"] = "phase";
        fields["name"] = name;
        fields["startedMarkMs"] = startedMark;
        fields["durationMs"] = mark() - startedMark;
        emit(std::move(fields));
    }

    std::vector<nlohmann::json> const & AgentTraceRecorder::records() const noexcept
    {
        return m_records;
    }

    std::size_t AgentTraceRecorder::recordCount() const noexcept
    {
        return m_records.size();
    }

    std::filesystem::path AgentTraceRecorder::timelinePath() const
    {
        if (m_outputFile.empty())
        {
            return {};
        }
        return std::filesystem::path{m_outputFile.string() + ".timeline.json"};
    }

    bool AgentTraceRecorder::writeTimeline(std::filesystem::path const & path,
                                           std::string & error) const
    {
        nlohmann::json events = nlohmann::json::array();
        for (auto const & record : m_records)
        {
            std::string const type = record.value("type", std::string{});
            if (type == "run_meta")
            {
                continue;
            }

            std::string label = type;
            std::int64_t tMs = readInt(record, "tMs");
            std::int64_t durationMs = readInt(record, "durationMs");
            nlohmann::json args = record;

            if (type == "turn")
            {
                label = "turn " + std::to_string(readInt(record, "turnIndex"));
                durationMs = readInt(record, "durationMs");
                if (durationMs <= 0)
                {
                    durationMs = readInt(record, "completionMs");
                }
                if (durationMs <= 0)
                {
                    continue;
                }

                // Context assembly, prefill/queueing and generation are the three parts an
                // optimization can attack, so show them as nested slices of the turn.
                std::int64_t const turnStart = tMs - durationMs;
                std::int64_t const buildMs = std::clamp(readInt(record, "buildMs"),
                                                        std::int64_t{0},
                                                        durationMs);
                std::int64_t const completionMs = durationMs - buildMs;
                // The first SSE event can arrive before the server has prefilled anything, so the
                // prefill slice runs until the first output token; hidden reasoning gets its own slice.
                std::int64_t const firstTokenMs = readInt(record, "firstTokenMs");
                // Traces written before the reasoning split existed have no `firstTokenMs`; for those
                // the visible output is the only prefill boundary available.
                std::int64_t const prefillBoundary = firstTokenMs > 0 ? firstTokenMs
                                                                     : readInt(record, "firstContentMs");
                std::int64_t const prefillMs =
                  std::clamp(std::max(readInt(record, "ttftMs"), prefillBoundary),
                             std::int64_t{0},
                             completionMs);
                std::int64_t const visibleMs =
                  std::clamp(std::max(prefillMs, readInt(record, "firstContentMs")),
                             std::int64_t{0},
                             completionMs);
                char const * turnThread = timelineThread(type.c_str());
                if (buildMs > 0)
                {
                    events.push_back({{"name", "context build"},
                                      {"ph", "X"},
                                      {"ts", turnStart * kMicrosecondsPerMillisecond},
                                      {"dur", buildMs * kMicrosecondsPerMillisecond},
                                      {"pid", 1},
                                      {"tid", turnThread}});
                }
                if (prefillMs > 0)
                {
                    events.push_back({{"name", "queue+prefill"},
                                      {"ph", "X"},
                                      {"ts", (turnStart + buildMs) * kMicrosecondsPerMillisecond},
                                      {"dur", prefillMs * kMicrosecondsPerMillisecond},
                                      {"pid", 1},
                                      {"tid", turnThread},
                                      {"args", {{"turnIndex", readInt(record, "turnIndex")}}}});
                }
                if (visibleMs - prefillMs > 0)
                {
                    events.push_back({{"name", "thinking"},
                                      {"ph", "X"},
                                      {"ts", (turnStart + buildMs + prefillMs) * kMicrosecondsPerMillisecond},
                                      {"dur", (visibleMs - prefillMs) * kMicrosecondsPerMillisecond},
                                      {"pid", 1},
                                      {"tid", turnThread},
                                      {"args", {{"turnIndex", readInt(record, "turnIndex")}}}});
                }
                if (completionMs - visibleMs > 0)
                {
                    events.push_back({{"name", "generate"},
                                      {"ph", "X"},
                                      {"ts", (turnStart + buildMs + visibleMs) * kMicrosecondsPerMillisecond},
                                      {"dur", (completionMs - visibleMs) * kMicrosecondsPerMillisecond},
                                      {"pid", 1},
                                      {"tid", turnThread},
                                      {"args", {{"turnIndex", readInt(record, "turnIndex")}}}});
                }
            }
            else if (type == "tool_call")
            {
                label = "tool " + record.value("name", std::string{"?"});
            }
            else if (type == "phase")
            {
                label = record.value("name", std::string{"phase"});
            }
            else if (type == "note")
            {
                label = record.value("kind", std::string{"note"});
                events.push_back({{"name", label},
                                  {"ph", "i"},
                                  {"s", "g"},
                                  {"ts", tMs * kMicrosecondsPerMillisecond},
                                  {"pid", 1},
                                  {"tid", timelineThread(type.c_str())},
                                  {"args", std::move(args)}});
                continue;
            }

            if (durationMs <= 0)
            {
                continue;
            }
            events.push_back({{"name", label},
                              {"ph", "X"},
                              {"ts", (tMs - durationMs) * kMicrosecondsPerMillisecond},
                              {"dur", durationMs * kMicrosecondsPerMillisecond},
                              {"pid", 1},
                              {"tid", timelineThread(type.c_str())},
                              {"args", std::move(args)}});
        }

        nlohmann::json timeline = {
          {"traceEvents", std::move(events)},
          {"displayTimeUnit", "ms"},
          {"otherData", {{("runId"), m_runId}}}};

        if (auto const parent = path.parent_path(); !parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            error = "Cannot open timeline file " + path.string();
            return false;
        }
        out << timeline.dump() << '\n';
        if (!out.good())
        {
            error = "Failed writing timeline file " + path.string();
            return false;
        }
        return true;
    }

    ScopedTracePhase::ScopedTracePhase(AgentTraceRecorder * recorder, std::string name)
        : m_recorder(recorder)
        , m_name(std::move(name))
        , m_startedMark(m_recorder ? m_recorder->mark() : 0)
    {
    }

    ScopedTracePhase::~ScopedTracePhase()
    {
        if (m_recorder)
        {
            m_recorder->recordPhase(m_name, m_startedMark, std::move(m_fields));
        }
    }

    void ScopedTracePhase::addFields(nlohmann::json fields)
    {
        m_fields.merge_patch(std::move(fields));
    }
} // namespace gladius::agent::trace
