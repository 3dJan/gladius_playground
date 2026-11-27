#pragma once

#include "Renderability.h"
#include "TodoStore.h"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gladius
{
    class Document;
}

namespace gladius::design
{
    /// @brief Half extent of the implicit evaluation domain in millimetres
    /// @details The default domain is a cube of this half extent around the origin. Geometry that
    ///          reaches or exceeds it is clipped silently: the render looks like a clean model while
    ///          whole features have been cut off, so the gate has to refuse it.
    inline constexpr double kDomainHalfExtentMm = 50.0;

    /// @brief Nodes a flattened implicit program may contain before sampling becomes too slow
    /// @details Flattening inlines every function call into one program, and the renderer samples
    ///          each resulting node. A runaway duplication is otherwise only visible as a stutter.
    inline constexpr std::size_t kDefaultFlatNodeBudget = 2000;

    class DesignGraph;
    struct Configuration;

    /**
     * @brief Deterministic pass/fail result of one implicit-CAD gate, with JSON evidence.
     */
    struct GateResult
    {
        bool passed = false;
        /// @brief Why the model failed (or why the gate could not be measured)
        std::string message;
        /// @brief Measured values, stored on the checklist entry as proof
        nlohmann::json evidence = nlohmann::json::object();

        [[nodiscard]] static GateResult pass(nlohmann::json evidence)
        {
            return GateResult{true, "", std::move(evidence)};
        }

        [[nodiscard]] static GateResult fail(std::string message, nlohmann::json evidence = nlohmann::json::object())
        {
            return GateResult{false, std::move(message), std::move(evidence)};
        }
    };

    /**
     * @brief Refuses geometry that touches or exceeds the evaluation domain.
     * @details Reuses measured bounds instead of running a second blocking probe. Unmeasurable bounds
     *          are reported as a failure: "we don't know" must not read as "it fits".
     */
    [[nodiscard]] GateResult checkDomainBounds(RenderabilityFacts const & facts,
                                              double halfExtentMm = kDomainHalfExtentMm);

    /**
     * @brief Reports the number of nodes the flattened program would contain.
     * @details A flattened implicit model is sampled per node, so the count is the render cost.
     */
    [[nodiscard]] GateResult checkFlatNodeBudget(Document const & document, std::size_t maxNodes);

    /**
     * @brief Everything a deterministic gate may need about the live design.
     * @details The renderability facts are collected once per gate run and shared by every entry:
     *          measuring them runs a blocking scene probe, so per-entry probing would multiply the
     *          cost of a single `finish_design`.
     */
    struct GateContext
    {
        RenderabilityFacts const & facts;
        Document * document = nullptr;
        DesignGraph const * graph = nullptr;
        Configuration const * configuration = nullptr;
        std::size_t maxFlatNodes = kDefaultFlatNodeBudget;
    };

    /**
     * @brief Applies the deterministic gate for one checklist kind to already-collected facts.
     * @details Kinds that need more than the facts (manufacturing, interfaces, node budget) fall back
     *          to a failure when their context is missing rather than reporting a false pass.
     */
    [[nodiscard]] GateResult evaluateGate(ChecklistKind kind, GateContext const & context);

    /**
     * @brief Refuses geometry that has no measurable interior volume.
     * @details A model whose bounds are empty or degenerate renders an empty viewport, which a visual
     *          review can easily mistake for "thin but correct".
     */
    [[nodiscard]] GateResult checkGeometryPresence(RenderabilityFacts const & facts);

    /**
     * @brief Checks the interfaces of the variant selected for `scopeNodeId` against the rest of
     *        the active configuration, including envelope containment in its parent.
     * @details An unscoped interface entry has nothing to be compatible with, so it is reported as a
     *          failure: the orchestrator has to name the component whose contract it wants proved.
     */
    [[nodiscard]] GateResult checkScopedInterfaces(DesignGraph const & graph,
                                                  Configuration const & configuration,
                                                  std::string const & scopeNodeId);

    /**
     * @brief Gate evaluator over every deterministic checklist entry of a design.
     * @param facts Renderability facts measured once for the whole gate run (`includeBounds` must be
     *              true, otherwise geometry and domain-bound requirements cannot be proved)
     */
    [[nodiscard]] TodoStore::GateEvaluator buildChecklistGateEvaluator(DesignGraph const & graph,
                                                                      Configuration const & configuration,
                                                                      Document * document,
                                                                      RenderabilityFacts const & facts,
                                                                      std::size_t maxFlatNodes = kDefaultFlatNodeBudget);
} // namespace gladius::design
