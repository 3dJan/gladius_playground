#include "ScratchDocument.h"

#include "TemplateSeed.h"
#include "../Document.h"
#include "../ExpressionParser.h"
#include "../ProgramSnippetApplier.h"
#include "../compute/ApplicationComputeRuntime.h"
#include "../nodes/Assembly.h"
#include "../nodes/DerivedNodes.h"
#include "../nodes/GraphFlattener.h"
#include "../nodes/Model.h"

#include <atomic>
#include <filesystem>
#include <sstream>
#include <system_error>

namespace gladius::design
{
    namespace
    {
        struct TempFileCleanup
        {
            std::filesystem::path path;
            ~TempFileCleanup()
            {
                if (!path.empty())
                {
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                }
            }
        };

        std::filesystem::path uniqueScratchPath()
        {
            static std::atomic<std::uint64_t> s_counter{0};
            auto const n = s_counter.fetch_add(1, std::memory_order_relaxed);
            return std::filesystem::temp_directory_path()
                   / ("gladius_scratch_component_" + std::to_string(n) + ".3mf");
        }
    } // namespace

    ScratchComponentDocument::ScratchComponentDocument(events::SharedLogger logger,
                                                       compute::IBackendRuntime * runtime)
        : m_logger(std::move(logger))
        , m_runtime(runtime)
    {
    }

    ScratchComponentDocument::~ScratchComponentDocument() = default;

    bool ScratchComponentDocument::initialize(std::string & errorOut)
    {
        if (!m_logger)
        {
            m_logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        }
        m_document = std::make_unique<Document>(m_logger);
        if (m_runtime != nullptr)
        {
            m_document->setBackendRuntime(m_runtime);
        }
        auto const seed = seedVolumetricTemplate(m_document.get());
        if (!seed.success)
        {
            errorOut = seed.error.empty() ? "Failed to seed scratch document" : seed.error;
            return false;
        }
        return true;
    }

    ScratchValidationResult ScratchComponentDocument::validateStandalone(
      std::size_t maxFlatNodes) const
    {
        ScratchValidationResult result;
        if (!m_document)
        {
            result.error = "Scratch document is not initialized";
            return result;
        }

        result.facts = collectRenderabilityFacts(m_document.get(), /*includeBounds=*/m_runtime != nullptr);
        if (!result.facts.isRenderable())
        {
            result.error = result.facts.blockersAsText();
            return result;
        }

        if (m_runtime != nullptr || result.facts.boundsNumeric)
        {
            result.domainGate = checkDomainBounds(result.facts);
            if (!result.domainGate.passed)
            {
                result.error = result.domainGate.message;
                return result;
            }
        }
        else
        {
            result.domainGate = GateResult::pass({{"skipped", "no_backend_runtime"}});
        }

        result.budgetGate = checkFlatNodeBudget(*m_document, maxFlatNodes);
        result.flatNodeCount = result.budgetGate.evidence.value("expectedNodeCount", std::size_t{0});
        if (!result.budgetGate.passed)
        {
            result.error = result.budgetGate.message;
            return result;
        }

        result.passed = true;
        return result;
    }

    bool ScratchComponentDocument::mergeInto(Document & targetDoc,
                                             std::string const & targetFunctionName,
                                             std::string & importedDisplayNameOut,
                                             std::string & errorOut) const
    {
        if (!m_document || !m_document->getAssembly())
        {
            errorOut = "Scratch document has no assembly";
            return false;
        }
        std::string resolvedTarget = targetFunctionName;
        if (resolvedTarget.empty())
        {
            for (auto const & [fid, model] : m_document->getAssembly()->getFunctions())
            {
                std::ignore = fid;
                if (!model)
                {
                    continue;
                }
                auto const name = model->getDisplayName().value_or("");
                if (!name.empty())
                {
                    resolvedTarget = name;
                    if (name == "main_3")
                    {
                        break;
                    }
                }
            }
        }
        if (resolvedTarget.empty())
        {
            errorOut = "Scratch document has no named function to merge";
            return false;
        }

        TempFileCleanup cleanup{uniqueScratchPath()};
        m_document->saveAs(cleanup.path, /*writeThumbnail=*/false);
        if (!std::filesystem::exists(cleanup.path))
        {
            errorOut = "Failed to serialize scratch document to " + cleanup.path.string();
            return false;
        }

        auto const match = targetDoc.mergeAndResolve(cleanup.path, resolvedTarget);
        if (match.id == 0)
        {
            errorOut = "mergeAndResolve failed for function '" + resolvedTarget + "'";
            return false;
        }

        std::string const baseName =
          match.model ? match.model->getDisplayName().value_or(resolvedTarget) : resolvedTarget;
        importedDisplayNameOut = baseName + "_" + std::to_string(match.id);

        targetDoc.update3mfModel();
        targetDoc.rebuildResourceDependencyGraph();
        targetDoc.refreshModelBlocking();
        targetDoc.markFileAsChanged();
        targetDoc.signalStructuralEdit();
        return true;
    }

    bool composeMain3Union(Document & targetDoc,
                           std::vector<std::string> const & functionNames,
                           std::string & errorOut)
    {
        if (functionNames.empty())
        {
            errorOut = "No component functions to compose";
            return false;
        }
        auto assembly = targetDoc.getAssembly();
        if (!assembly)
        {
            errorOut = "Target document has no assembly";
            return false;
        }

        nodes::ResourceId mainId = 0;
        for (auto const & [fid, model] : assembly->getFunctions())
        {
            if (!model)
            {
                continue;
            }
            auto const name = model->getDisplayName().value_or("");
            if (name == "main" || name == "main_3" || fid == 3)
            {
                mainId = fid;
                if (name == "main" || name == "main_3")
                {
                    break;
                }
            }
        }
        if (mainId == 0)
        {
            errorOut = "Target document has no root function";
            return false;
        }

        auto hasShapePort = [&](std::string const & fnName) -> bool {
            for (auto const & [fid, model] : assembly->getFunctions())
            {
                if (!model)
                {
                    continue;
                }
                std::string const dname = model->getDisplayName().value_or("");
                std::string const fullName = dname + "_" + std::to_string(fid);
                if (fnName == dname || fnName == fullName)
                {
                    if (auto * endNode = model->getEndNode())
                    {
                        return endNode->parameter().size() > 1
                               && endNode->parameter().find("shape") != endNode->parameter().end();
                    }
                }
            }
            return false;
        };

        std::ostringstream snippet;
        snippet << "// Function: main (ID: " << mainId << ") [root]\n";
        snippet << "(vec3 color, float shape) main_" << mainId << "(vec3 pos) {\n";
        snippet << "    color = vec3(0.5, 0.5, 0.5);\n";
        for (std::size_t i = 0; i < functionNames.size(); ++i)
        {
            snippet << "    float d" << i << " = " << functionNames[i] << "(pos)"
                    << (hasShapePort(functionNames[i]) ? ".shape" : "") << ";\n";
        }
        snippet << "    float combined = d0;\n";
        for (std::size_t i = 1; i < functionNames.size(); ++i)
        {
            snippet << "    combined = min(combined, d" << i << ");\n";
        }
        snippet << "    shape = combined;\n}\n";

        {
            auto const token = targetDoc.waitForAssemblyToken();
            ExpressionParser parser;
            auto const report = ProgramSnippetApplier::apply(snippet.str(), *assembly, parser);
            if (!report.failed.empty())
            {
                errorOut = report.failed.front().message;
                return false;
            }
        }

        targetDoc.update3mfModel();
        targetDoc.rebuildResourceDependencyGraph();
        targetDoc.refreshModelBlocking();
        targetDoc.markFileAsChanged();
        targetDoc.signalStructuralEdit();
        return true;
    }
} // namespace gladius::design
