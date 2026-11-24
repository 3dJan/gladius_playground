#include "ContextBuilder.h"

#include <sstream>

#include <algorithm>

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
               "- Every snippet you send to set_program_snippet lists one block per function, and each block MUST start with the "
               "header comment `// Function: <name> (ID: <n>)` on the line directly above its signature. A snippet without these "
               "headers is rejected as a whole. Copy the IDs from get_program_snippet / get_function_snippet; reuse existing IDs "
               "for existing functions and pick unused IDs for new ones.\n"
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

    namespace
    {
        /// @brief True when a message carries blocks of the given kind
        template<typename Block>
        [[nodiscard]] bool hasBlock(Message const & msg)
        {
            return std::any_of(msg.parts.begin(), msg.parts.end(),
                               [](Content const & part)
                               {
                                   return std::holds_alternative<Block>(part);
                               });
        }

        /// @brief True when history may start at `index` without breaking a tool exchange
        /// @details A `tool_result` may never be sent without the `tool_use` it answers, so a prune
        ///          boundary cannot directly follow a message that issued calls.
        [[nodiscard]] bool isPruneBoundary(std::vector<Message> const & messages, std::size_t index)
        {
            if (index == 0 || index >= messages.size())
            {
                return false;
            }
            return !hasBlock<ToolResult>(messages[index]) && !hasBlock<ToolCall>(messages[index - 1]);
        }

        /// @brief Snaps a prune boundary to the nearest whole exchange, preferring the next one
        /// @details Pruning in chunks keeps the sent prefix byte-identical between prunes; snapping to
        ///          an exchange keeps the prompt valid for the provider.
        [[nodiscard]] std::size_t alignToPruneBoundary(std::vector<Message> const & messages,
                                                       std::size_t index)
        {
            auto const last = messages.size() - 1;
            index = std::clamp<std::size_t>(index, 1, last);
            while (index < last && !isPruneBoundary(messages, index))
            {
                ++index;
            }
            while (index > 1 && !isPruneBoundary(messages, index))
            {
                --index;
            }
            return index;
        }
    } // namespace

    std::vector<Message> ContextBuilder::filterHistory(Conversation const & conversation,
                                                       PrivacySettings const & privacy,
                                                       ContextBuildStats * stats) const
    {
        auto const & messages = conversation.messages();

        // History is append-only: a message that has been sent once is sent again unchanged. Sliding
        // windows (keep the newest N images, compact everything older) rewrite the oldest messages
        // every turn, which breaks the endpoint's prompt cache right after the tool block and makes it
        // re-prefill the whole conversation. Old content is dropped in whole messages instead, and in
        // chunks, so the sent prefix stays byte-identical between prunes.
        //
        // Pruning must not split a tool_use from its tool_result, so the cut moves back to the start
        // of the exchange the boundary falls inside.
        std::size_t firstSent = 0;
        if (m_policy.maxHistoryMessages > 0 && messages.size() > m_policy.maxHistoryMessages
            && messages.size() > 1)
        {
            std::size_t const excess = messages.size() - m_policy.maxHistoryMessages;
            std::size_t const chunks
              = (excess + m_policy.pruneChunkMessages - 1) / m_policy.pruneChunkMessages;
            std::size_t const requested = chunks * m_policy.pruneChunkMessages;
            // The opening message is the design request itself: it is cheap, and losing it makes the
            // model forget the task, so it survives every prune.
            firstSent = alignToPruneBoundary(messages, requested);
        }

        std::vector<Message> filtered;
        filtered.reserve(messages.size() - firstSent);
        for (std::size_t i = firstSent; i < messages.size(); ++i)
        {
            Message const & msg = messages[i];
            Message out;
            out.role = msg.role;
            for (auto const & part : msg.parts)
            {
                if (std::holds_alternative<ImageContent>(part))
                {
                    bool const allowed = privacy.allowViewportImages || privacy.allowReferenceImages;
                    if (!allowed)
                    {
                        continue;
                    }
                    out.parts.push_back(part);
                    if (stats != nullptr)
                    {
                        ++stats->imagesKept;
                    }
                    continue;
                }

                if (std::holds_alternative<ToolResult>(part))
                {
                    out.parts.push_back(part);
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

        if (stats != nullptr)
        {
            stats->messagesSent = filtered.size();
            stats->messagesPruned = firstSent;
        }
        return filtered;
    }

} // namespace gladius::agent
