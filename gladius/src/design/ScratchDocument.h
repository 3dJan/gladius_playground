#pragma once

#include "CadGates.h"
#include "Renderability.h"
#include "../EventLogger.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gladius
{
    class Document;

    namespace compute
    {
        class IBackendRuntime;
    }
} // namespace gladius

namespace gladius::design
{
    /// @brief Deterministic checks run on a standalone leaf scratch document before merging
    struct ScratchValidationResult
    {
        bool passed = false;
        std::string error;
        RenderabilityFacts facts;
        std::size_t flatNodeCount = 0;
        GateResult domainGate;
        GateResult budgetGate;
    };

    /**
     * @brief Isolated `Document` for designing and validating one leaf component.
     * @details Seeded with `seedVolumetricTemplate` so a component agent works in a clean,
     *          renderable 3MF without seeing or mutating sibling functions. Once validated, the
     *          leaf function is merged into the main assembly via `Document::mergeAndResolve`.
     */
    class ScratchComponentDocument
    {
      public:
        ScratchComponentDocument(events::SharedLogger logger,
                                 compute::IBackendRuntime * runtime);
        ~ScratchComponentDocument();

        ScratchComponentDocument(ScratchComponentDocument const &) = delete;
        ScratchComponentDocument & operator=(ScratchComponentDocument const &) = delete;

        /// @brief Seed the scratch document with the shipped volumetric template
        [[nodiscard]] bool initialize(std::string & errorOut);

        [[nodiscard]] Document * document() noexcept
        {
            return m_document.get();
        }

        [[nodiscard]] Document const * document() const noexcept
        {
            return m_document.get();
        }

        /**
         * @brief Run deterministic implicit-CAD checks on the scratch document.
         * @details Verifies renderability facts, domain bounds (using `facts`), and flattened
         *          program node budget before the component is allowed to merge.
         */
        [[nodiscard]] ScratchValidationResult validateStandalone(
          std::size_t maxFlatNodes = kDefaultFlatNodeBudget) const;

        /**
         * @brief Merge a function from this scratch document into `targetDoc`.
         * @param targetDoc Assembly document receiving the leaf function
         * @param targetFunctionName Function display name to resolve (empty uses `"main_3"` or
         *                           the first available function)
         * @param importedDisplayNameOut Populated with the resolved display name in `targetDoc`
         * @param errorOut Populated on failure
         */
        [[nodiscard]] bool mergeInto(Document & targetDoc,
                                     std::string const & targetFunctionName,
                                     std::string & importedDisplayNameOut,
                                     std::string & errorOut) const;

      private:
        events::SharedLogger m_logger;
        compute::IBackendRuntime * m_runtime = nullptr;
        std::unique_ptr<Document> m_document;
    };

    /**
     * @brief Compose imported leaf functions into `main_3` inside `targetDoc` as a CSG union.
     * @details Rewrites the root `main_3` function via `ProgramSnippetApplier` so each leaf is
     *          evaluated at `pos` and combined with `min(...)`.
     */
    [[nodiscard]] bool composeMain3Union(Document & targetDoc,
                                         std::vector<std::string> const & functionNames,
                                         std::string & errorOut);
} // namespace gladius::design
