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
               "1b. A new user message is never already solved. Even if the design was completed and validated by an earlier\n"
               "    request, treat every new message as a change request: add the new requirement, update the plan, iterate on\n"
               "    the design, and finish again. Never answer a new request by simply reporting the previous design as done.\n"
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
               "- Keep `main` modular by calling helper functions.\n\n"
               "ROOT FUNCTION AND RESOURCE IDS (the model only renders when these are respected):\n"
               "- The rendered solid is whatever the root function writes to its `shape` output (negative inside, zero surface).\n"
               "- The root is always `main (ID: 3)` and keeps its signature `(vec3 color, float shape) main_3(vec3 pos)`. "
               "Write both outputs: `color = vec3(0.1, 0.2, 1.0);` and your geometry into `shape`.\n"
               "- Never declare a function with ID 0 and never rename or drop the `shape` output, otherwise nothing is rendered.\n"
               "- Call helpers by their qualified name (`sphere_5(pos, 5.0)`). Reuse the helper IDs the document already provides "
               "(see get_program_snippet) instead of re-declaring them, and pick unused IDs for genuinely new helpers.\n"
               "\nTOOL ARGUMENTS:\n"
               "- Match the declared argument type exactly. A field declared as an array must be a JSON array even for a single "
               "element (`\"tags\": [\"gyroid\"]`, never `\"tags\": \"gyroid\"`), an object must be an object, and an integer must be "
               "an integer. Omit optional fields instead of passing a value of the wrong type.\n"
               "\nFINISH CEREMONY (required before finish_design succeeds):\n"
               "- add_function for each part, then create_variant for every design node, then create_configuration whose "
               "`selectedVariants` has an entry for every design node, then activate_configuration, and only then finish_design. "
               "finish_design without an active configuration always fails.\n";
    }


    namespace
    {
        /// @brief Marker text left where a superseded viewport image used to be
        constexpr char const * const kDroppedImageStub = "[capture dropped: an older viewport image "
                                                         "was removed from history to save prompt "
                                                         "tokens; call capture_viewport for a fresh view]";

        /// @brief Marks where a compacted tool result used to be
        [[nodiscard]] nlohmann::json compactedToolResult(std::string const & callId)
        {
            return {{"compacted", true},
                    {"note", "result removed from history to save prompt tokens; call the tool again "
                             "if you need it"},
                    {"toolUseId", callId}};
        }

        void appendSection(std::ostringstream & out, char const * header, std::string const & body)
        {
            if (body.empty())
            {
                return;
            }
            out << "\n" << header << ":\n" << body << "\n";
        }
    } // namespace

    CompletionRequest ContextBuilder::build(Conversation const & conversation,
                                            tools::ToolRegistry const & registry,
                                            PrivacySettings const & privacy,
                                            DesignContextSnapshot const & snapshot,
                                            ContextBuildStats * stats) const
    {
        CompletionRequest req;
        req.systemPrompt = buildStaticSystemPrompt(snapshot, privacy);
        req.tools = registry.definitions();
        req.messages = filterHistory(conversation, privacy, stats);

        // The live state goes last: appending to the end of the conversation extends the provider's
        // cached prefix, while changing the system block invalidates it and re-prefills everything.
        std::string const live = buildLiveContext(snapshot, privacy);
        if (!live.empty())
        {
            Message liveMessage;
            liveMessage.role = MessageRole::User;
            liveMessage.parts.push_back(TextContent{std::move(live)});
            req.messages.push_back(std::move(liveMessage));
        }

        return req;
    }

    std::string ContextBuilder::buildStaticSystemPrompt(DesignContextSnapshot const & snapshot,
                                                        PrivacySettings const & privacy) const
    {
        std::ostringstream sys;
        sys << defaultSystemPrompt();
        if (snapshot.autonomy == AutonomyLevel::WorkAutonomously)
        {
            sys << "\nAUTONOMOUS MODE ACTIVE:\n"
                   "The user is away and will NOT reply. Do NOT ask questions, do NOT ask for "
                   "confirmation or permission to continue, and do NOT stop to check in. Make "
                   "reasonable engineering assumptions, state them concisely, execute all steps "
                   "(library check, import/create, set_program_snippet, capture_viewport), and call "
                   "finish_design.\n";
        }
        if (snapshot.isIncremental)
        {
            sys << "\nINCREMENTAL MODE:\n"
                   "You are continuing incrementally on an existing loaded document. Inspect and build "
                   "upon the current model rather than discarding unrelated parts.\n";
        }
        if (privacy.allowModelMetadata && !snapshot.manufacturingConstraints.empty())
        {
            appendSection(sys, "Manufacturing constraints", snapshot.manufacturingConstraints);
        }
        return sys.str();
    }

    std::string ContextBuilder::buildLiveContext(DesignContextSnapshot const & snapshot,
                                                 PrivacySettings const & privacy) const
    {
        std::ostringstream live;
        live << "CURRENT STATE (authoritative, most recent):\n"
             << "- Design phase: " << snapshot.designPhase << "\n";

        if (snapshot.isFollowUpRequest)
        {
            live << "FOLLOW-UP REQUEST ON A FINISHED DESIGN:\n"
                    "A previous design was already completed and validated. The user's newest message "
                    "is a new request that has NOT been considered yet. Do NOT assume the work is done "
                    "and do NOT call finish_design before addressing it: extend the requirements with "
                    "add_requirement for the new wish, revise the plan (todos), variants and "
                    "configuration, apply the model edits, inspect with capture_viewport, and only then "
                    "call finish_design.\n";
        }

        if (!snapshot.requirementsSummary.empty())
        {
            live << "- Requirements:\n" << snapshot.requirementsSummary;
        }
        if (!snapshot.requirementChangesSummary.empty())
        {
            live << "- USER REQUIREMENT CHANGES (update your plan accordingly):\n"
                 << snapshot.requirementChangesSummary;
        }
        if (privacy.allowModelMetadata)
        {
            if (!snapshot.existingModelSummary.empty())
            {
                live << "- Current model functions:\n" << snapshot.existingModelSummary;
            }
            if (!snapshot.designGraphSummary.empty())
            {
                live << "- Design graph summary:\n" << snapshot.designGraphSummary;
            }
            if (!snapshot.activeConfigurationSummary.empty())
            {
                live << "- Active configuration:\n" << snapshot.activeConfigurationSummary;
            }
        }
        if (privacy.allowValidationResults && !snapshot.validationSummary.empty())
        {
            live << "- Deterministic validation results:\n" << snapshot.validationSummary;
        }
        if (!snapshot.relevantCatalogueSummary.empty())
        {
            live << "- Relevant catalogue entries:\n" << snapshot.relevantCatalogueSummary;
        }
        if (!snapshot.availableSkillsSummary.empty())
        {
            live << "- Available skills (use load_skill to read detailed guidance):\n"
                 << snapshot.availableSkillsSummary;
        }
        if (!snapshot.todoSummary.empty())
        {
            live << "- Current planning todos:\n" << snapshot.todoSummary;
        }
        if (!snapshot.knowledgeSummary.empty())
        {
            live << "- Relevant past learnings (from KnowledgeStore):\n" << snapshot.knowledgeSummary;
        }
        return live.str();
    }

    std::vector<Message> ContextBuilder::filterHistory(Conversation const & conversation,
                                                       PrivacySettings const & privacy,
                                                       ContextBuildStats * stats) const
    {
        auto const & messages = conversation.messages();

        // Only the newest captures are worth their tokens: an image costs a fixed block of prompt
        // tokens no matter how large the encoded file is, so keeping ten views costs ten times one.
        std::size_t const totalImages = std::count_if(
          messages.begin(), messages.end(),
          [](Message const & msg)
          {
              return std::any_of(msg.parts.begin(), msg.parts.end(),
                                 [](Content const & part)
                                 {
                                     return std::holds_alternative<ImageContent>(part);
                                 });
          });
        std::size_t const imagesToKeep = std::min(m_policy.keptImages, totalImages);
        std::size_t const firstKeptImage = totalImages - imagesToKeep;

        // Successful results go stale quickly; the model has usually acted on them already.
        std::size_t const totalResults = std::count_if(
          messages.begin(), messages.end(),
          [](Message const & msg)
          {
              return std::any_of(msg.parts.begin(), msg.parts.end(),
                                 [](Content const & part)
                                 {
                                     return std::holds_alternative<ToolResult>(part);
                                 });
          });
        std::size_t const firstKeptResult
          = totalResults > m_policy.keptToolResults ? totalResults - m_policy.keptToolResults : 0;

        std::vector<Message> filtered;
        filtered.reserve(messages.size());
        std::size_t imageIndex = 0;
        std::size_t resultIndex = 0;

        for (auto const & msg : messages)
        {
            Message out;
            out.role = msg.role;
            for (auto const & part : msg.parts)
            {
                if (auto const * image = std::get_if<ImageContent>(&part))
                {
                    bool const allowed = privacy.allowViewportImages || privacy.allowReferenceImages;
                    bool const keep = allowed && imageIndex >= firstKeptImage;
                    if (keep)
                    {
                        out.parts.push_back(part);
                        if (stats != nullptr)
                        {
                            ++stats->imagesKept;
                        }
                    }
                    else if (allowed)
                    {
                        out.parts.push_back(TextContent{kDroppedImageStub});
                        if (stats != nullptr)
                        {
                            ++stats->imagesDropped;
                        }
                    }
                    ++imageIndex;
                    continue;
                }

                if (auto const * result = std::get_if<ToolResult>(&part))
                {
                    bool const failed = result->value.is_object()
                                     && result->value.value("success", true) == false;
                    bool const keep = failed || resultIndex >= firstKeptResult;
                    if (keep)
                    {
                        out.parts.push_back(part);
                        if (stats != nullptr)
                        {
                            ++stats->toolResultsKept;
                        }
                    }
                    else
                    {
                        out.parts.push_back(ToolResult{result->callId,
                                                       compactedToolResult(result->callId)});
                        if (stats != nullptr)
                        {
                            ++stats->toolResultsCompacted;
                        }
                    }
                    ++resultIndex;
                    continue;
                }

                if (std::holds_alternative<TextContent>(part) && !privacy.allowConversationText)
                {
                    continue;
                }
                out.parts.push_back(part);
            }
            if (!out.parts.empty())
            {
                filtered.push_back(std::move(out));
            }
        }
        return filtered;
    }
} // namespace gladius::agent
