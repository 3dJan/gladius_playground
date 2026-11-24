#pragma once

#include "types.h"

#include <lib3mf_implicit.hpp>

#include <string>
#include <vector>

namespace gladius
{
    class Document;
}

namespace gladius::design
{
    /**
     * @brief One reason the current model cannot render, together with the repair that clears it.
     */
    struct RenderabilityBlocker
    {
        std::string code;
        std::string message;
        std::string fix;
    };

    /**
     * @brief What the scene actually contains, independent of design-graph bookkeeping.
     * @details The agent loop only ever saw function and node counts, so a document whose root
     *          output is wired to nothing looked exactly like a finished model. These facts are the
     *          difference between "the program compiles" and "something shows up in the viewport",
     *          and they are meant to be fed back to the agent instead of surfacing in a post-mortem.
     */
    struct RenderabilityFacts
    {
        int buildItemCount{};
        int levelSetCount{};
        ResourceId rootFunctionId{};
        std::string rootFunctionName;
        bool rootFunctionHasShapeOutput{};
        bool rootShapeConsumed{};
        std::string renderChannel;
        std::string modelBounds;
        bool boundsUsable{};
        std::vector<RenderabilityBlocker> blockers;

        [[nodiscard]] bool isRenderable() const
        {
            return blockers.empty();
        }

        /// @brief Blockers as "code: message -> fix" lines, for tool output and the context line.
        [[nodiscard]] std::string blockersAsText() const;
    };

    /**
     * @brief Finds the function a build item's level set evaluates.
     * @return The function's Gladius ResourceId, or 0 when no build item evaluates a function.
     * @details This is the root from the renderer's point of view, which is the only point of view
     *          that matters for renderability. A document can hold dozens of functions; the one a
     *          level set points at is the one that is actually sampled.
     */
    [[nodiscard]] ResourceId findRootFunctionId(Lib3MF::PModel const & model);

    /**
     * @brief True when the assembly's output sink has a source, i.e. the root output is consumed.
     * @details Writing `shape` inside the root function is not enough: the render scene reads the
     *          assembly-level output node, and that stays unsourced when the root function never
     *          reaches it.
     */
    [[nodiscard]] bool isRootShapeConsumed(Document * document);

    /**
     * @brief Collects the renderability facts of the document's current 3MF model.
     * @details Reads the lib3mf resources the renderer and the slicer consume, plus the assembly's
     *          output sink. The probe is read-only: it never writes the model.
     * @param includeBounds Runs the blocking analytic bounds probe. Expensive, so only the terminal
     *                      checks ask for it. Unusable bounds are reported, never a blocker, because
     *                      the analytic backend legitimately reports the build-volume fallback.
     */
    [[nodiscard]] RenderabilityFacts collectRenderabilityFacts(Document * document,
                                                              bool includeBounds = false);
}
