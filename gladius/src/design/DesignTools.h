#pragma once

#include "DesignCatalogue.h"
#include "DesignGraph.h"
#include "TodoStore.h"
#include "../agent/KnowledgeStore.h"
#include "../agent/SkillLoader.h"
#include "../tools/CheckpointManager.h"
#include "../tools/ToolRegistry.h"
#include "../tools/TransactionManager.h"

#include <cstddef>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
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
        GeometryOnly
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
    };

    /// @brief Register the canonical design-space, lifecycle, evaluation, and viewport tools
    void registerDesignTools(tools::ToolRegistry & registry,
                             DesignToolContext & ctx,
                             DesignToolSurface surface = DesignToolSurface::Full);
} // namespace gladius::design
