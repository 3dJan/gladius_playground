#include "ContextBuilder.h"

#include <sstream>

namespace gladius::agent
{
    std::string ContextBuilder::defaultSystemPrompt()
    {
        return "You are the Gladius Native Design Agent for hierarchical, variant-driven 3D engineering design.\n"
               "Follow a systematic engineering process:\n"
               "1. Inspect the model and requirements before modifying anything.\n"
               "2. Decompose into functions and interfaces, then explore multiple solution principles.\n"
               "3. Use low-fidelity envelope variants before committing to structural or decorative detail.\n"
               "4. Preserve frozen components and group edits inside transactions (begin_edit / commit_edit).\n"
               "5. Separate deterministic metrics from subjective visual assessments.\n"
               "6. Run validation and call finish_design explicitly when complete.\n";
    }

    CompletionRequest ContextBuilder::build(Conversation const & conversation,
                                            tools::ToolRegistry const & registry,
                                            PrivacySettings const & privacy,
                                            DesignContextSnapshot const & snapshot) const
    {
        CompletionRequest req;

        std::ostringstream sys;
        sys << defaultSystemPrompt();
        if (!snapshot.designPhase.empty())
        {
            sys << "\nCurrent design phase: " << snapshot.designPhase << "\n";
        }
        if (!snapshot.requirementsSummary.empty())
        {
            sys << "\nRequirements:\n" << snapshot.requirementsSummary << "\n";
        }
        if (!snapshot.manufacturingConstraints.empty())
        {
            sys << "\nManufacturing constraints:\n" << snapshot.manufacturingConstraints << "\n";
        }
        if (privacy.allowModelMetadata)
        {
            if (!snapshot.designGraphSummary.empty())
            {
                sys << "\nDesign graph summary:\n" << snapshot.designGraphSummary << "\n";
            }
            if (!snapshot.activeConfigurationSummary.empty())
            {
                sys << "\nActive configuration:\n" << snapshot.activeConfigurationSummary << "\n";
            }
        }
        if (!snapshot.relevantCatalogueSummary.empty())
        {
            sys << "\nRelevant catalogue entries:\n" << snapshot.relevantCatalogueSummary << "\n";
        }
        if (privacy.allowValidationResults && !snapshot.validationSummary.empty())
        {
            sys << "\nDeterministic validation results:\n" << snapshot.validationSummary << "\n";
        }

        req.systemPrompt = sys.str();
        req.tools = registry.definitions();

        for (auto const & msg : conversation.messages())
        {
            Message filtered;
            filtered.role = msg.role;
            for (auto const & part : msg.parts)
            {
                if (std::holds_alternative<TextContent>(part) && !privacy.allowConversationText)
                {
                    continue;
                }
                if (std::holds_alternative<ImageContent>(part) && !privacy.allowViewportImages
                    && !privacy.allowReferenceImages)
                {
                    continue;
                }
                filtered.parts.push_back(part);
            }
            if (!filtered.parts.empty())
            {
                req.messages.push_back(std::move(filtered));
            }
        }

        return req;
    }
} // namespace gladius::agent
