#include "ContextBuilder.h"

#include <sstream>

namespace gladius::agent
{
    std::string ContextBuilder::defaultSystemPrompt()
    {
        return "You are the Gladius Native Design Agent for hierarchical, variant-driven 3D engineering design.\n\n"
               "ENGINEERING WORKFLOW:\n"
               "1. Inspect the open document (get_model_summary, get_program_snippet) and requirements.\n"
               "   When continuing incrementally on an existing model, preserve and extend existing functions rather than replacing them unless requested.\n"
               "2. For non-trivial tasks, plan your steps with create_todo_list / add_todo_item and track progress with update_todo_item.\n"
               "3. Create a part/function inventory: call add_function for every required part/feature.\n"
               "4. Library reuse check: for each part/function in your list, search the Gladius library "
               "(list_library_items) and decide whether to REUSE an existing library item (import_library_item) "
               "or CREATE a new function. Prefer reusing library items (lattices, primitives, mechanical parts, etc.) "
               "over rewriting math from scratch.\n"
               "5. Explore solution principles (create_variant) and form configurations (create_configuration, activate_configuration, hybridize_configurations).\n"
               "6. Group model edits in begin_edit / commit_edit and apply geometry directly with set_program_snippet.\n"
               "7. When a reusable component is finished, score its quality and reusability (score_component_reusability). "
               "If both scores are high (>= 0.75), export it to the user library with export_library_item (non-destructive to the main document).\n"
               "8. Store concise, reusable design learnings and pitfalls with store_knowledge (and search_knowledge when needed).\n"
               "9. Visually inspect with capture_viewport, validate, and call finish_design when complete.\n\n"
               "GLADIUS SNIPPET LANGUAGE RULES (follow strictly):\n"
               "- Function signatures: `float myFunc(vec3 pos, float r)` or `(vec3 color, float shape) main_3(vec3 pos)`.\n"
               "- Declare local variables with `float name = ...;` (do NOT declare `vec3 name = ...;` as a local variable; "
               "`vec3(...)` is an expression/parameter type).\n"
               "- Reference local variables by their plain name (`name`, NEVER `in_name`). `in_` is only for function parameters.\n"
               "- No `for`/`while` loops or recursion; use `mod`, `clamp`, `round`, `min`, `max`, `abs`, `sin`, `cos`, `length`, `dot`.\n"
               "- Keep `main` modular by calling helper functions.\n";
    }

    CompletionRequest ContextBuilder::build(Conversation const & conversation,
                                            tools::ToolRegistry const & registry,
                                            PrivacySettings const & privacy,
                                            DesignContextSnapshot const & snapshot) const
    {
        CompletionRequest req;

        std::ostringstream sys;
        sys << defaultSystemPrompt();
        if (snapshot.autonomy == AutonomyLevel::WorkAutonomously)
        {
            sys << "\nAUTONOMOUS MODE ACTIVE:\n"
                   "The user is away and will NOT reply. Do NOT ask questions, do NOT ask for confirmation "
                   "or permission to continue, and do NOT stop to check in. Make reasonable engineering "
                   "assumptions, state them concisely, execute all steps (library check, import/create, "
                   "set_program_snippet, capture_viewport), and call finish_design.\n";
        }
        if (!snapshot.designPhase.empty())
        {
            sys << "\nCurrent design phase: " << snapshot.designPhase << "\n";
        }
        if (snapshot.isIncremental)
        {
            sys << "\nINCREMENTAL MODE:\n"
                   "You are continuing incrementally on an existing loaded document. Inspect and build upon "
                   "the current model rather than discarding unrelated parts.\n";
            if (!snapshot.existingModelSummary.empty())
            {
                sys << "Current model functions:\n" << snapshot.existingModelSummary << "\n";
            }
        }
        if (!snapshot.requirementChangesSummary.empty())
        {
            sys << "\nUSER REQUIREMENT CHANGES (update your plan accordingly):\n"
                << snapshot.requirementChangesSummary << "\n";
        }
        if (!snapshot.knowledgeSummary.empty())
        {
            sys << "\nRelevant past learnings (from KnowledgeStore):\n"
                << snapshot.knowledgeSummary << "\n";
        }
        if (!snapshot.todoSummary.empty())
        {
            sys << "\nCurrent planning todos:\n" << snapshot.todoSummary << "\n";
        }
        if (!snapshot.availableSkillsSummary.empty())
        {
            sys << "\nAvailable skills (use load_skill to read detailed guidance):\n"
                << snapshot.availableSkillsSummary << "\n";
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
