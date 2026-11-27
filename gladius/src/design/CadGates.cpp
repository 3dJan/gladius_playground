#include "CadGates.h"

#include "../Document.h"
#include "../nodes/GraphFlattener.h"
#include "DesignGraph.h"
#include "InterfaceCompliance.h"
#include "ManufacturingValidator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gladius::design
{
    GateResult checkDomainBounds(RenderabilityFacts const & facts, double halfExtentMm)
    {
        if (!facts.boundsNumeric)
        {
            return GateResult::fail("The model bounds could not be measured, so clipping cannot be ruled out",
                                    {{"measured", false}});
        }

        nlohmann::json evidence = {
          {"min", {facts.boundsMinMax[0], facts.boundsMinMax[1], facts.boundsMinMax[2]}},
          {"max", {facts.boundsMinMax[3], facts.boundsMinMax[4], facts.boundsMinMax[5]}},
          {"halfExtentMm", halfExtentMm}};

        double overflow = std::numeric_limits<double>::lowest();
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            overflow = std::max(overflow,
                                std::abs(facts.boundsMinMax[axis]) - halfExtentMm);
            overflow = std::max(overflow,
                                std::abs(facts.boundsMinMax[axis + 3]) - halfExtentMm);
        }

        if (overflow >= 0.0)
        {
            evidence["overflowMm"] = overflow;
            return GateResult::fail("Geometry reaches the evaluation domain at +/-" + std::to_string(halfExtentMm) +
                                      "mm, so it is clipped to flat faces there",
                                    std::move(evidence));
        }
        evidence["headroomMm"] = -overflow;
        return GateResult::pass(std::move(evidence));
    }

    GateResult checkFlatNodeBudget(Document const & document, std::size_t maxNodes)
    {
        auto const assembly = document.getAssembly();
        if (!assembly)
        {
            return GateResult::fail("No active assembly to flatten", {{"measured", false}});
        }

        try
        {
            nodes::GraphFlattener flattener(*assembly);
            std::size_t const expected = flattener.calculateExpectedNodeCount();
            nlohmann::json evidence = {{"expectedNodeCount", expected}, {"maxNodes", maxNodes}};
            if (expected > maxNodes)
            {
                return GateResult::fail("The flattened program needs " + std::to_string(expected) +
                                          " nodes, above the budget of " + std::to_string(maxNodes),
                                        std::move(evidence));
            }
            return GateResult::pass(std::move(evidence));
        }
        catch (std::exception const & ex)
        {
            return GateResult::fail(std::string("The flattened node count could not be measured: ") + ex.what(),
                                    {{"measured", false}});
        }
    }

    GateResult checkGeometryPresence(RenderabilityFacts const & facts)
    {
        if (!facts.boundsNumeric)
        {
            return GateResult::fail("The model bounds could not be measured, so no geometry is proven to exist",
                                    {{"measured", false}});
        }

        nlohmann::json evidence = {
          {"min", {facts.boundsMinMax[0], facts.boundsMinMax[1], facts.boundsMinMax[2]}},
          {"max", {facts.boundsMinMax[3], facts.boundsMinMax[4], facts.boundsMinMax[5]}}};

        double smallestSpan = std::numeric_limits<double>::max();
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            smallestSpan = std::min(smallestSpan, facts.boundsMinMax[axis + 3] - facts.boundsMinMax[axis]);
        }
        evidence["smallestSpanMm"] = smallestSpan;

        if (smallestSpan <= 0.0)
        {
            return GateResult::fail("The model has no measurable interior volume along any axis", std::move(evidence));
        }
        return GateResult::pass(std::move(evidence));
    }

    GateResult checkScopedInterfaces(DesignGraph const & graph,
                                    Configuration const & configuration,
                                    std::string const & scopeNodeId)
    {
        if (scopeNodeId.empty())
        {
            return GateResult::fail("An interface requirement must name the component whose contract it proves");
        }
        auto const * node = graph.findNode(scopeNodeId);
        if (!node)
        {
            return GateResult::fail("Unknown design node '" + scopeNodeId + "' for an interface requirement");
        }
        auto const selected = configuration.selectedVariants.find(scopeNodeId);
        if (selected == configuration.selectedVariants.end())
        {
            return GateResult::fail("Node '" + scopeNodeId + "' has no variant selected in the active configuration",
                                    {{"measured", false}});
        }
        auto const * candidate = graph.findVariant(selected->second);
        if (!candidate)
        {
            return GateResult::fail("Selected variant '" + selected->second + "' does not exist");
        }
        if (candidate->interfaces.empty())
        {
            return GateResult::fail("Variant '" + candidate->id + "' declares no interfaces, so nothing is compatible",
                                    {{"variantId", candidate->id}});
        }

        std::vector<Interface> reference;
        for (auto const & [nodeId, variantId] : configuration.selectedVariants)
        {
            if (nodeId == scopeNodeId)
            {
                continue;
            }
            if (auto const * other = graph.findVariant(variantId))
            {
                reference.insert(reference.end(), other->interfaces.begin(), other->interfaces.end());
            }
        }

        nlohmann::json evidence = {{"variantId", candidate->id}, {"checkedAgainst", reference.size()}};
        auto problems = checkInterfacesAgainstConfiguration(candidate->interfaces, reference);
        for (auto const & problem : problems)
        {
            if (!evidence.contains("mismatches"))
            {
                evidence["mismatches"] = nlohmann::json::array();
            }
            evidence["mismatches"].push_back(problem.describe());
        }

        // The parent's reserved envelope is the containment half of the contract: a child may not
        // stick out of the space its parent allocated for it.
        if (auto const * parent = graph.findParentNode(scopeNodeId))
        {
            auto const parentSelection = configuration.selectedVariants.find(parent->id);
            if (parentSelection != configuration.selectedVariants.end())
            {
                if (auto const * parentVariant = graph.findVariant(parentSelection->second))
                {
                    auto containment = checkEnvelopeContainmentBetween(parentVariant->interfaces, candidate->interfaces);
                    if (!containment.compatible)
                    {
                        evidence["containment"] = containment.describe();
                    }
                }
            }
        }

        if (!problems.empty() || evidence.contains("containment"))
        {
            return GateResult::fail("Interfaces of '" + candidate->id + "' do not satisfy the active configuration",
                                    std::move(evidence));
        }
        return GateResult::pass(std::move(evidence));
    }

    namespace
    {
        std::string joinWarnings(std::vector<std::string> const & warnings)
        {
            std::string text;
            for (auto const & warning : warnings)
            {
                if (!text.empty())
                {
                    text += "; ";
                }
                text += warning;
            }
            return text;
        }

        /// @brief Runs the manufacturing analysis of a live document as a gate
        GateResult checkManufacturing(Document & document, DesignGraph const & graph)
        {
            auto const report = ManufacturingValidator::analyzeDocument(document, graph);
            nlohmann::json evidence = report.toJson();
            if (!report.warnings.empty())
            {
                return GateResult::fail("Manufacturing analysis reported: " + joinWarnings(report.warnings),
                                        std::move(evidence));
            }
            return GateResult::pass(std::move(evidence));
        }
    } // namespace

    GateResult evaluateGate(ChecklistKind kind, GateContext const & context)
    {
        switch (kind)
        {
          case ChecklistKind::Renderability:
              if (context.facts.isRenderable())
              {
                  return GateResult::pass({{"buildItemCount", context.facts.buildItemCount},
                                           {"levelSetCount", context.facts.levelSetCount},
                                           {"rootFunctionId", context.facts.rootFunctionId}});
              }
              return GateResult::fail(context.facts.blockersAsText(), {{"blockers", context.facts.blockersAsText()}});

          case ChecklistKind::DomainBounds:
              return checkDomainBounds(context.facts);

          case ChecklistKind::Geometry:
              return checkGeometryPresence(context.facts);

          case ChecklistKind::FlatNodeBudget:
              return context.document != nullptr ? checkFlatNodeBudget(*context.document, context.maxFlatNodes)
                                                 : GateResult::fail("No document to flatten", {{"measured", false}});

          case ChecklistKind::Manufacturing:
              return context.document != nullptr && context.graph != nullptr
                       ? checkManufacturing(*context.document, *context.graph)
                       : GateResult::fail("No document to analyse for manufacturability", {{"measured", false}});

          case ChecklistKind::Interface:
              return context.graph != nullptr && context.configuration != nullptr
                       ? checkScopedInterfaces(*context.graph, *context.configuration, "")
                       : GateResult::fail("No active configuration to check interfaces against", {{"measured", false}});

          case ChecklistKind::Manual:
          case ChecklistKind::User:
              break;
        }
        return GateResult::fail(std::string("Checklist kind ") + toString(kind) + " is not gate checked");
    }

    TodoStore::GateEvaluator buildChecklistGateEvaluator(DesignGraph const & graph,
                                                        Configuration const & configuration,
                                                        Document * document,
                                                        RenderabilityFacts const & facts,
                                                        std::size_t maxFlatNodes)
    {
        // The caller measured the facts once; every entry reuses them instead of probing the scene again.
        return [&graph, &configuration, document, maxFlatNodes, facts](ChecklistEntry const & entry) {
            GateContext context{facts, document, &graph, &configuration, maxFlatNodes};
            GateResult result = entry.kind == ChecklistKind::Interface
                                  ? checkScopedInterfaces(graph, configuration, entry.scopeNodeId)
                                  : evaluateGate(entry.kind, context);
            return TodoStore::GateVerdict{result.passed, result.message, std::move(result.evidence)};
        };
    }
} // namespace gladius::design
