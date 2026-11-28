#include "ContextBuilder.h"

#include <sstream>

#include <algorithm>

namespace gladius::agent
{
    char const * agentRoleName(AgentRole role) noexcept
    {
        switch (role)
        {
        case AgentRole::Default:
            return "default";
        case AgentRole::Requirements:
            return "requirements";
        case AgentRole::Decomposition:
            return "decomposition";
        case AgentRole::Component:
            return "component";
        case AgentRole::Review:
            return "review";
        }
        return "default";
    }

    namespace
    {
        /// @brief Implicit-CAD snippet language and tool-argument rules shared by every role that writes geometry
        char const * const kSnippetRules
          =
            "\nGLADIUS SNIPPET LANGUAGE RULES (follow strictly):\n"
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
            "an integer. Omit optional fields instead of passing a value of the wrong type.\n";
    } // namespace

    namespace
    {
        /// @brief Workflow preamble of the monolithic (default) design agent
        std::string defaultRolePreamble(bool geometryToolSurface)
        {
            std::string prompt
              = "You are the Gladius Native Design Agent for hierarchical, variant-driven 3D engineering design.\n\n"
                "WORKFLOW:\n"
                "1. Inspect the open document (get_model_summary, get_program_snippet). When continuing on an\n"
                "   existing model, preserve and extend existing functions unless asked to replace them.\n"
                "2. A new user message is never already solved. Even if an earlier request completed and was\n"
                "   validated, treat every new message as a change request and iterate again.\n"
                "3. Check the library before writing math: list_library_items, then import_library_item for\n"
                "   anything that already exists (lattices, primitives, mechanical parts).\n"
                "4. Apply geometry with begin_edit / set_program_snippet / commit_edit, then verify.\n"
                "5. Inspect with capture_viewport at most once or twice; extra views rarely change the answer.\n"
                "6. Call finish_design when the geometry matches the request.\n";

            if (!geometryToolSurface)
            {
                prompt +=
                  "7. For non-trivial tasks, plan with create_todo_list / add_todo_item and track progress with update_todo_item.\n"
                  "8. Inventory required parts with add_function, explore solution principles (create_variant) and\n"
                  "   form configurations (create_configuration, activate_configuration, hybridize_configurations).\n"
                  "9. When a reusable component is finished, score it (score_component_reusability); at >= 0.75 export\n"
                  "   it with export_library_item (non-destructive to the main document).\n"
                  "10. Store concise reusable learnings with store_knowledge (search_knowledge when needed).\n";
            }
            return prompt;
        }
    } // namespace

    std::string ContextBuilder::defaultSystemPrompt(bool geometryToolSurface)
    {
        std::string prompt = defaultRolePreamble(geometryToolSurface);
        prompt += kSnippetRules;

        if (!geometryToolSurface)
        {
            prompt +=
              "\nFINISH CEREMONY (required before finish_design succeeds):\n"
              "- add_function for each part, then create_variant for every design node, then create_configuration whose "
              "`selectedVariants` has an entry for every design node, then activate_configuration, and only then finish_design. "
              "finish_design without an active configuration always fails.\n";
        }
        else
        {
            prompt +=
              "\nfinish_design fills in the design-graph bookkeeping itself; do not plan around it.\n";
        }
        return prompt;
    }


    std::string ContextBuilder::defaultSystemPrompt(RolePrompt const & prompt)
    {
        if (prompt.role == AgentRole::Default)
        {
            return defaultSystemPrompt(prompt.geometryToolSurface);
        }

        std::string out = "You are one specialised agent inside the Gladius Native Design Agent pipeline.\n"
                          "The orchestrator owns the overall design, the schedule, and the final finish_design call.\n"
                          "Never call finish_design, never restate the whole design task, and stop as soon as your own\n"
                          "sub-task is answered. Work only on the state given in CURRENT STATE.\n\n";

        switch (prompt.role)
        {
        case AgentRole::Default:
            break;
        case AgentRole::Requirements:
            out +=
              "ROLE - REQUIREMENT ENGINEER:\n"
              "Turn the user's wish into an explicit, verifiable requirement set and one acceptance checklist.\n"
              "- add_requirement per atomic requirement (functional / aesthetic / manufacturing), mandatory only where the\n"
              "  user actually insisted; it is the only write you need besides create_todo_list.\n"
              "- Every requirement must be checkable on an implicit field: a dimension, a tolerance, a topology property,\n"
              "  or a look. Vague wishes become assumptions you state explicitly.\n"
              "- Then create_todo_list with one checklist item per acceptance criterion, naming the concrete measurement\n"
              "  (span in mm, wall thickness, void volume, surface pattern) instead of \"looks right\".\n"
              "- Do not design geometry, do not write snippets, do not decompose. Report and stop.\n";
            break;
        case AgentRole::Decomposition:
            out +=
              "ROLE - DECOMPOSITION AGENT:\n"
              "Split the requirement set into a recursion tree of components small enough to model in one step.\n"
              "- add_function creates one design node per component (type System / Assembly / Component / Function);\n"
              "  pass parentNodeId so nesting matches reality, and addressedRequirements to record coverage.\n"
              "- Split deeper while a child would still need more than one independent shape; stop at leaves a single\n"
              "  signed-distance function can express. Implicit-CAD limits decide the leaf: repetition has no loops, so\n"
              "  a periodic or lattice pattern is ONE leaf, never N copies.\n"
              "- define_interface on every variant that touches another component: state the mating specification as\n"
              "  numbers (axis/frame, envelope box in mm, bore or stud dimensions, sign discipline), because the\n"
              "  orchestrator checks compatibility against exactly those values.\n"
              "- Where several solution principles are legitimately possible (combustion vs electric engine, spokes vs\n"
              "  disc wheel), create_variant per principle instead of deciding for the user.\n"
              "- Do not write geometry snippets. Report the tree and stop.\n";
            break;
        case AgentRole::Component:
            out +=
              "ROLE - COMPONENT DESIGNER:\n"
              "Model exactly one design node, the one named in CURRENT STATE, as signed-distance math.\n"
              "- Honour its inherited interface contract: mating dimensions, envelope, frame origin, and sign discipline\n"
              "  are hard constraints, not suggestions. Never change another component's geometry.\n"
              "- Begin with list_library_items / import_library_item; only write math for what the library cannot give you.\n"
              "- Apply it in one begin_edit / set_program_snippet / commit_edit cycle, then capture_viewport once to check.\n"
              "- Keep the function modifiable: take dimensions as parameters, keep the field continuous, and stay inside\n"
              "  the stated flat-node budget (inline helpers are cheap, long chains of them are not).\n"
              "- Report what you produced against each requirement and stop. Do not touch other nodes.\n";
            break;
        case AgentRole::Review:
            out +=
              "ROLE - REVIEW GATE:\n"
              "Judge the component named in CURRENT STATE against its contract and checklist. You are an evaluator,\n"
              "not a designer: never edit geometry, never write snippets.\n"
              "- Read the model (get_model_summary, get_program_snippet, get_node) and capture_viewport at most once.\n"
              "- add_variant_feedback or reject_variant on evidence; update the requirement when the requirement itself\n"
              "  is what failed. Say precisely which dimension or property deviates and by how much.\n"
              "- End with a short verdict line: PASS, or REDO with the concrete correction the designer must apply.\n";
            break;
        }

        // Component designers always write snippets, on either surface; the other roles never do.
        if (prompt.geometryToolSurface || prompt.role == AgentRole::Component)
        {
            out += kSnippetRules;
        }
        return out;
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
        // Providers need to know where the conversation ends: the live block below is regenerated
        // every turn, so a cache breakpoint must never be placed on it.
        req.conversationMessages = req.messages.size();

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
        sys << defaultSystemPrompt(RolePrompt{m_role, m_geometryToolSurface});
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
        // A scoped run sees only its own component: the rest of the tree is cost without signal.
        bool const scoped = !snapshot.scopeNodeId.empty();
        live << "CURRENT STATE (authoritative, most recent):\n"
             << "- Design phase: " << snapshot.designPhase << "\n";

        if (scoped)
        {
            live << "- Assigned component: " << snapshot.scopeNodeId << "\n";
            if (!snapshot.inheritedContract.empty())
            {
                live << "- Inherited interface contract (hard constraints):\n" << snapshot.inheritedContract;
            }
            if (!snapshot.subtreeSummary.empty())
            {
                live << "- Your component subtree:\n" << snapshot.subtreeSummary;
            }
        }

        if (snapshot.isFollowUpRequest && !scoped)
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
        // Never a suggestion: until the scene contains a level set and a build item, every snippet
        // the model writes is unreachable no matter how correct the math is.
        if (!snapshot.renderabilitySummary.empty())
        {
            live << "- THE MODEL DOES NOT RENDER YET: " << snapshot.renderabilitySummary << "\n";
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
            // On the geometry-only surface the graph only exists to be validated by finish_design.
            if (!m_geometryToolSurface && !scoped && !snapshot.designGraphSummary.empty())
            {
                live << "- Design graph summary:\n" << snapshot.designGraphSummary;
            }
            if (!scoped && !snapshot.activeConfigurationSummary.empty())
            {
                live << "- Active configuration:\n" << snapshot.activeConfigurationSummary;
            }
        }
        if (privacy.allowValidationResults && !snapshot.validationSummary.empty())
        {
            live << "- Deterministic validation results:\n" << snapshot.validationSummary;
        }
        if (!scoped && !snapshot.relevantCatalogueSummary.empty())
        {
            live << "- Relevant catalogue entries:\n" << snapshot.relevantCatalogueSummary;
        }
        if (!m_geometryToolSurface && !scoped && !snapshot.availableSkillsSummary.empty())
        {
            live << "- Available skills (use load_skill to read detailed guidance):\n"
                 << snapshot.availableSkillsSummary;
        }
        if (!m_geometryToolSurface && !scoped && !snapshot.todoSummary.empty())
        {
            live << "- Current planning todos:\n" << snapshot.todoSummary;
        }
        if (scoped && !snapshot.assignedChecklistSummary.empty())
        {
            live << "- Assigned acceptance checklist (all must pass before you report back):\n"
                 << snapshot.assignedChecklistSummary;
        }
        if (!m_geometryToolSurface && !scoped && !snapshot.knowledgeSummary.empty())
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
