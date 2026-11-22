#pragma once

#include "DesignCatalogue.h"
#include "DesignGraph.h"
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
    };

    /// @brief Register all canonical design-space, lifecycle, evaluation, and viewport tools
    void registerDesignTools(tools::ToolRegistry & registry, DesignToolContext & ctx);
} // namespace gladius::design
