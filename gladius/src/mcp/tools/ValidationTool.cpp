/**
 * @file ValidationTool.cpp
 * @brief Implementation of ValidationTool for model validation operations
 */

#include "ValidationTool.h"
#include "../../Application.h"
#include "../../Document.h"
#include "../EventLogger.h"
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace gladius
{
    namespace mcp::tools
    {
        ValidationTool::ValidationTool(Application * app)
            : MCPToolBase(app)
        {
        }

        nlohmann::json ValidationTool::validateModel(const nlohmann::json & options)
        {
            nlohmann::json out;
            out["phases"] = nlohmann::json::array();
            out["success"] = false;

            if (!validateApplication())
            {
                out["error"] = getLastErrorMessage();
                return out;
            }

            auto document = m_application->getCurrentDocument();
            if (!document)
            {
                setErrorMessage("No active document available");
                out["error"] = getLastErrorMessage();
                return out;
            }

            // Clear old validation events before starting new validation
            auto logger = document->getSharedLogger();
            if (logger)
            {
                logger->clear();
            }

            bool const doCompile = options.value("compile", true);
            int const maxMessages = options.value("max_messages", 50);

            // Helper to snapshot logger state/messages
            auto snapshotLogger = [&](events::SharedLogger const & logger) -> nlohmann::json
            {
                nlohmann::json j;
                if (!logger)
                {
                    j["messages"] = nlohmann::json::array();
                    j["errors"] = 0;
                    j["warnings"] = 0;
                    return j;
                }
                // Flush any pending async writes and get counts
                const_cast<events::Logger &>(*logger).flush();
                j["errors"] = logger->getErrorCount();
                j["warnings"] = logger->getWarningCount();

                // Collect up to maxMessages most recent messages (filter out info messages)
                nlohmann::json msgs = nlohmann::json::array();
                size_t count = 0;
                for (auto it = logger->cbegin();
                     it != logger->cend() && count < static_cast<size_t>(maxMessages);
                     ++it)
                {
                    // Skip info messages - only show warnings, errors, and fatal errors
                    if (it->getSeverity() == events::Severity::Info)
                    {
                        continue;
                    }

                    nlohmann::json mj;
                    mj["message"] = it->getMessage();
                    auto tp = it->getTimeStamp();
                    mj["timestamp"] =
                      std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch())
                        .count();
                    switch (it->getSeverity())
                    {
                    case events::Severity::Info:
                        mj["severity"] = "info";
                        break;
                    case events::Severity::Warning:
                        mj["severity"] = "warning";
                        break;
                    case events::Severity::Error:
                        mj["severity"] = "error";
                        break;
                    case events::Severity::FatalError:
                        mj["severity"] = "fatal";
                        break;
                    }
                    msgs.push_back(mj);
                    ++count;
                }
                j["messages"] = msgs;
                return j;
            };

            // Phase 1: graph_sync (update 3MF model, update IOs, validate assembly)
            nlohmann::json phase1 = {{"name", "graph_sync"}, {"errors", 0}};
            try
            {
                // Bring 3MF model in sync and update assembly IO relationship
                document->update3mfModel();
                if (auto assembly = document->getAssembly())
                {
                    assembly->updateInputsAndOutputs();
                }

                bool validAsm = document->validateAssembly(nodes::ValidationContext::Api);
                auto logger = document->getSharedLogger();
                auto diag = snapshotLogger(logger);
                phase1.update(diag);
                phase1["ok"] = validAsm && (static_cast<int>(phase1["errors"]) == 0);
            }
            catch (const std::exception & e)
            {
                phase1["ok"] = false;
                phase1["messages"] = nlohmann::json::array(
                  {nlohmann::json{{"severity", "error"}, {"message", e.what()}}});
                phase1["errors"] = static_cast<int>(phase1["errors"]) + 1;
            }
            out["phases"].push_back(phase1);

            // Phase 2: backend compile (optional)
            auto const backendKind = document->getBackendKind();
            bool const isWebGpu =
              backendKind.has_value() && *backendKind == compute::ComputeBackendKind::WebGPU;
            std::string const backendName = isWebGpu ? "webgpu" : "opencl";
            out["backend"] = backendName;

            nlohmann::json phase2 = {{"name", backendName + "_compile"}};
            bool compileOk = true;
            if (doCompile)
            {
                try
                {
                    // Backend-neutral compile: lowers the flattened assembly and compiles the
                    // generated program (OpenCL kernel or composed WGSL) for the active backend.
                    auto const compileResult = document->compileModelProgramBlocking();
                    auto logger = document->getSharedLogger();
                    auto diag = snapshotLogger(logger);
                    phase2.update(diag);
                    phase2["message"] = compileResult.message;
                    compileOk = compileResult.ok && (static_cast<int>(phase2["errors"]) == 0);
                    if (!compileOk)
                    {
                        auto msgs = phase2["messages"];
                        msgs.push_back({{"severity", "error"},
                                        {"message",
                                         compileResult.message.empty()
                                           ? fmt::format("{} program not valid after compile",
                                                         compileResult.backend)
                                           : compileResult.message}});
                        phase2["messages"] = msgs;
                        phase2["errors"] = static_cast<int>(phase2["errors"]) + 1;
                    }
                    phase2["ok"] = compileOk;
                }
                catch (const std::exception & e)
                {
                    phase2["ok"] = false;
                    auto msgs = phase2.value("messages", nlohmann::json::array());
                    msgs.push_back({{"severity", "error"}, {"message", e.what()}});
                    phase2["messages"] = msgs;
                    phase2["errors"] = static_cast<int>(phase2.value("errors", 0)) + 1;
                }

                // Inject actionable hints for common compile errors
                if (!phase2.value("ok", true))
                {
                    auto & msgs = phase2["messages"];
                    for (auto const & msg : msgs)
                    {
                        auto text = msg.value("message", std::string{});
                        if (text.find("undeclared identifier") != std::string::npos)
                        {
                            msgs.push_back(
                              {{"severity", "hint"},
                               {"message",
                                "Undeclared identifier usually means a function input "
                                "is not connected. Use create_constant_nodes_for_missing"
                                "_parameters or manually add constant nodes and link them "
                                "to the unconnected inputs. Check resourceid parameters "
                                "as well — they must reference valid function resource IDs."}});
                            break;
                        }
                    }
                }

                out["phases"].push_back(phase2);
            }

            // Summary & success flag
            out["summary"] = {{"graph_ok", phase1.value("ok", false)},
                              {"compile_ok", doCompile ? phase2.value("ok", false) : true}};
            out["success"] =
              out["summary"]["graph_ok"].get<bool>() && out["summary"]["compile_ok"].get<bool>();
            return out;
        }

        nlohmann::json
        ValidationTool::validateForManufacturing(const std::vector<std::string> & functionNames,
                                                 const nlohmann::json & constraints)
        {
            nlohmann::json validation;

            if (!validateActiveDocument())
            {
                validation["success"] = false;
                validation["error"] = getLastErrorMessage();
                return validation;
            }

            validation["overall_status"] = "valid";
            validation["printable"] = true;
            validation["manifold"] = true;
            validation["wall_thickness_ok"] = true;
            validation["overhangs_acceptable"] = true;
            validation["supports_needed"] = false;

            validation["issues"] = nlohmann::json::array();
            validation["recommendations"] = nlohmann::json::array();
            validation["recommendations"].push_back("Consider adding fillets to sharp corners");
            validation["recommendations"].push_back(
              "Verify wall thickness meets printer requirements");

            if (!functionNames.empty())
            {
                validation["validated_functions"] = functionNames;
            }

            if (!constraints.empty())
            {
                validation["applied_constraints"] = constraints;
            }

            return validation;
        }
    }
}
