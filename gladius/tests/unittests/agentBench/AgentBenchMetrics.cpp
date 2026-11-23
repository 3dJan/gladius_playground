#include "AgentBenchMetrics.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <utility>

namespace gladius::agent::bench
{
    namespace
    {
        constexpr char const * const kDesignGraphTools[] = {"add_requirement",
                                                            "add_function",
                                                            "create_variant",
                                                            "reject_variant",
                                                            "freeze_component",
                                                            "define_interface",
                                                            "create_configuration",
                                                            "activate_configuration",
                                                            "hybridize_variants",
                                                            "finish_design"};

        [[nodiscard]] bool isDesignGraphTool(std::string const & name)
        {
            return std::any_of(std::begin(kDesignGraphTools),
                               std::end(kDesignGraphTools),
                               [&name](char const * const tool) { return name == tool; });
        }

        /// @brief Truncate long provider/tool errors so the report stays readable
        [[nodiscard]] std::string trim(std::string value, std::size_t maxChars = 240)
        {
            if (value.size() <= maxChars)
            {
                return value;
            }
            return value.substr(0, maxChars) + "...";
        }

        [[nodiscard]] std::string entryKey(nlohmann::json const & result,
                                           std::string const & categoryKey,
                                           std::string const & nameKey)
        {
            std::string const category = result.value(categoryKey, std::string{});
            std::string const name = result.value(nameKey, std::string{});
            return category.empty() ? name : category + "/" + name;
        }
    } // namespace

    nlohmann::json AgentBenchMetrics::toJson() const
    {
        nlohmann::json toolCounters = nlohmann::json::object();
        for (auto const & [name, counter] : perTool)
        {
            toolCounters[name] = {{"calls", counter.calls}, {"failures", counter.failures}};
        }
        nlohmann::json failures = nlohmann::json::array();
        for (auto const & f : failureDetails)
        {
            failures.push_back(f);
        }
        return {{"taskId", taskId},
                {"wallMs", wallMs},
                {"modelTurns", modelTurns},
                {"toolCalls", toolCalls},
                {"toolFailures", toolFailures},
                {"unknownToolCalls", unknownToolCalls},
                {"truncatedTurns", truncatedTurns},
                {"providerFailures", providerFailures},
                {"viewportCaptures", viewportCaptures},
                {"librarySearches", librarySearches},
                {"libraryImports", libraryImports},
                {"libraryExports", libraryExports},
                {"designGraphMutations", designGraphMutations},
                {"agentCompleted", agentCompleted},
                {"agentError", agentError},
                {"finalMessage", trim(finalMessage, 400)},
                {"providerName", providerName},
                {"usage",
                 {{"inputTokens", usage.inputTokens},
                  {"outputTokens", usage.outputTokens},
                  {"cacheReadTokens", usage.cacheReadTokens},
                  {"cacheWriteTokens", usage.cacheWriteTokens},
                  {"totalPromptTokens", usage.totalPromptTokens()},
                  {"reported", usage.isReported()}}},
                {"perTool", std::move(toolCounters)},
                {"libraryFilesAdded", libraryFilesAdded},
                {"failureDetails", std::move(failures)}};
    }

    AgentBenchMetricsCollector::AgentBenchMetricsCollector(std::string taskId)
        : m_metrics{.taskId = std::move(taskId)}
    {
    }

    void AgentBenchMetricsCollector::record(AgentEvent const & event)
    {
        if (auto const * started = std::get_if<ToolCallStartedEvent>(&event))
        {
            ++m_metrics.perTool[started->name].calls;
            ++m_metrics.toolCalls;
            m_pendingArguments[started->callId] = started->arguments;
            return;
        }
        if (auto const * completed = std::get_if<ToolCallCompletedEvent>(&event))
        {
            nlohmann::json arguments = nlohmann::json::object();
            auto const it = m_pendingArguments.find(completed->callId);
            if (it != m_pendingArguments.end())
            {
                arguments = it->second;
                m_pendingArguments.erase(it);
            }
            recordToolCompletion(completed->name, completed->result, arguments, completed->success);
            return;
        }
        if (auto const * failed = std::get_if<AgentFailedEvent>(&event))
        {
            m_metrics.agentError = trim(failed->error);
        }
    }

    void AgentBenchMetricsCollector::recordToolCompletion(std::string const & name,
                                                          nlohmann::json const & result,
                                                          nlohmann::json const & arguments,
                                                          bool success)
    {
        auto & counter = m_metrics.perTool[name];
        if (!success)
        {
            ++counter.failures;
            ++m_metrics.toolFailures;
            std::string reason =
              result.is_object() ? result.value("error", std::string{}) : "";
            if (reason.empty() && result.is_object() && result.contains("warnings"))
            {
                // finish_design and friends report a failed validation as warnings, not as an error
                for (auto const & warning : result["warnings"])
                {
                    if (!reason.empty())
                    {
                        reason += "; ";
                    }
                    reason += warning.is_string() ? warning.get<std::string>() : warning.dump();
                }
            }
            m_metrics.failureDetails.push_back(trim(name + ": " + (reason.empty() ? "failed" : reason)));
            if (reason.rfind("Unknown tool:", 0u) == 0u)
            {
                ++m_metrics.unknownToolCalls;
            }
        }
        if (name == "capture_viewport" && success)
        {
            ++m_metrics.viewportCaptures;
        }
        else if (name == "list_library_items" && success)
        {
            ++m_metrics.librarySearches;
        }
        else if (name == "import_library_item" && success)
        {
            m_importedEntries.insert(entryKey(arguments, "category", "name"));
            m_metrics.libraryImports = m_importedEntries.size();
        }
        else if (name == "export_library_item" && success)
        {
            m_exportedEntries.insert(entryKey(result, "category", "name"));
            m_metrics.libraryExports = m_exportedEntries.size();
        }
        else if (isDesignGraphTool(name) && success)
        {
            ++m_metrics.designGraphMutations;
        }
    }

    void AgentBenchMetricsCollector::collectFrom(HeadlessAgentHost & host,
                                                 AgentRunResult const & result,
                                                 std::size_t providerFailures)
    {
        for (auto const & event : host.drainEvents())
        {
            record(event);
        }
        m_metrics.modelTurns = result.modelTurns;
        m_metrics.agentCompleted = result.completed;
        m_metrics.usage = result.usage;
        m_metrics.finalMessage = result.finalMessage;
        m_metrics.providerFailures = providerFailures;
        m_metrics.truncatedTurns = result.truncatedTurns;
        m_metrics.wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - m_startTime)
                             .count();
        if (!result.error.empty() && m_metrics.agentError.empty())
        {
            m_metrics.agentError = trim(result.error);
        }
    }

    void AgentBenchMetricsCollector::diffUserLibrary(std::filesystem::path const & userLibraryDir,
                                                     std::vector<std::string> const & before)
    {
        auto const after = listLibraryFiles(userLibraryDir);
        std::vector<std::string> added;
        std::set_difference(after.begin(),
                            after.end(),
                            before.begin(),
                            before.end(),
                            std::back_inserter(added));
        if (added.size() > m_metrics.libraryExports)
        {
            m_metrics.libraryExports = added.size();
        }
        m_metrics.libraryFilesAdded = added;
    }

    std::vector<std::string> listLibraryFiles(std::filesystem::path const & dir)
    {
        std::vector<std::string> out;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
        {
            return out;
        }
        for (auto const & entry : std::filesystem::recursive_directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".3mf")
            {
                continue;
            }
            out.push_back(std::filesystem::relative(entry.path(), dir, ec).string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }
} // namespace gladius::agent::bench
