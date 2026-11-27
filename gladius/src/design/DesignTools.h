#pragma once

#include "DesignCatalogue.h"
#include "DesignGraph.h"
#include "TodoStore.h"
#include "../agent/KnowledgeStore.h"
#include "../agent/SkillLoader.h"
#include "../agent/UserDecision.h"
#include "../agent/providers/ImageProvider.h"
#include "../tools/CheckpointManager.h"
#include "../tools/ToolRegistry.h"
#include "../tools/TransactionManager.h"

#include <coro/coro.hpp>
#include <cstddef>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gladius
{
    class Document;
}

namespace gladius::design
{
    /// @brief Callback for capturing the current 3D viewport as in-memory PNG bytes
    using ViewportCaptureCallback = std::function<std::vector<std::byte>(
      int width, int height, std::string const & cameraPreset)>;

    /// @brief Callback for running deterministic model/manufacturing validation
    using ModelValidationCallback = std::function<Evaluation(DesignGraph const &, Configuration const &)>;

    /// @brief Callback for generating a reference image asynchronously via the configured image provider
    using ImageGenerationCallback = std::function<coro::task<agent::ImageGenerationResponse>(
      agent::ImageGenerationRequest const & request,
      io::CancellationToken const & cancellation)>;

    /// @brief How much of the tool surface a host exposes to its model
    /// @details Every tool definition is re-sent on every model turn, so the tool list itself is a
    ///          large part of the prompt. A host that only needs geometry iteration can leave out
    ///          the planning, knowledge, checkpoint, and variant-selection tools without touching
    ///          the behaviour of the tools that remain.
    enum class DesignToolSurface
    {
        /// @brief The complete canonical tool set
        Full,
        /// @brief Only model read/write, viewport, library, requirements, and finish tools
        GeometryOnly,
        /// @brief Requirement engineering: requirements and acceptance checklist, no geometry writes
        Requirements,
        /// @brief Decomposition: design tree, variants and interface contracts, no geometry writes
        Decomposition,
        /// @brief Component design: model read/write and library, without finish_design
        Component,
        /// @brief Review gate: reads, one viewport capture and verdict tools, no geometry writes
        Review
    };

    /// @brief Tools the given surface keeps; an empty span means "everything"
    /// @details Named role surfaces never offer finish_design: completing the design belongs to the
    ///          orchestrator, and a definition the agent cannot legitimately use is pure prompt cost.
    [[nodiscard]] std::span<char const * const> toolSurfaceAllowlist(DesignToolSurface surface);

    /// @brief Self-report submitted by a component designer at the end of its scoped run
    struct ComponentReport
    {
        std::string nodeId;
        std::vector<std::string> satisfied;
        std::vector<std::string> unsatisfied;
        nlohmann::json evidence = nlohmann::json::object();
    };

    /// @brief Shared context for design-space, transaction, checkpoint, and evaluation tools
    struct DesignToolContext
    {
        DesignGraph & graph;
        DesignCatalogue & catalogue;
        tools::TransactionManager & transactions;
        tools::CheckpointManager & checkpoints;
        Document * document = nullptr;
        ViewportCaptureCallback captureViewport;
        ModelValidationCallback validateModel;
        std::function<void()> onDocumentModified;
        agent::SkillLoader * skills = nullptr;
        TodoStore * todos = nullptr;
        agent::KnowledgeStore * knowledge = nullptr;

        /// @brief Let finish_design fill in the design graph it would otherwise demand
        /// @details Without an active configuration finish_design fails and names the four tools
        ///          needed to fix it, which costs the model several turns per design. When the
        ///          geometry is what matters, deriving the missing node, variant, and configuration
        ///          from the document keeps the same validated end state in zero turns.
        bool autoCompleteDesignGraphOnFinish = false;
        /// @brief Viewport images a run may request; 0 is unlimited
        /// @details Each image costs a fixed share of every later turn, so a bounded review budget
        ///          is cheaper than letting the model re-inspect the same model repeatedly.
        std::size_t maxViewportReviews = 0;
        /// @brief Viewport images already handed out; the host resets this per run
        std::size_t viewportReviewsUsed = 0;
        ImageGenerationCallback generateImage;

        /// @brief Decision-mode transport: show a question to the user and await the answer
        /// @details Without a callback `ask_user` refuses, which keeps headless or restricted hosts
        ///          from ever waiting on a human that is not there.
        agent::AskUserCallback askUser;
        /// @brief Questions asked so far in this run; the host resets this per run
        std::size_t userQuestionsAsked = 0;
        /// @brief Component self-reports keyed by `nodeId` (missing report ⇒ work incomplete)
        std::unordered_map<std::string, ComponentReport> componentReports;
    };

    /// @brief Register the canonical design-space, lifecycle, evaluation, and viewport tools
    void registerDesignTools(tools::ToolRegistry & registry,
                             DesignToolContext & ctx,
                             DesignToolSurface surface = DesignToolSurface::Full);
} // namespace gladius::design
