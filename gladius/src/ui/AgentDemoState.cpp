#include "AgentDemoState.h"

#include "../agent/Conversation.h"
#include "../design/DesignGraph.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#include <cstdlib>
#endif

namespace gladius::ui::agentDemo
{
    namespace
    {
        /// @brief Long agent answer, so the collapsed transcript has something to cut
        char const * const kLongAnswer
          = "I read the goal as a clamp with two constraints that fight each other: the jaw has to "
            "open far enough for a 40 mm bar, while the screw keeps enough material behind the "
            "thread to carry the clamping load. My plan is to fix the screw axis first, then let "
            "the jaw angle follow from the opening requirement instead of the other way round. If "
            "that gives a jaw that is too thin at the root, the answer is a longer throat, not a "
            "thicker jaw.";

        /// @brief One example session: a request, a tool call, its result, and the answer that
        ///        followed, then a follow-up question with a viewport capture attached.
        void appendExampleTurns(agent::Conversation & conversation)
        {
            conversation.appendUserText("Design a bench clamp with 40 mm opening and 2 kN "
                                        "clamping force.");
            conversation.appendAssistant(
              "Two things have to be fixed before I can propose geometry: the screw axis and the "
              "jaw travel. I will record both as requirements first.",
              {agent::ToolCall{"call_1", "set_requirements", nlohmann::json::object()}});
            conversation.appendToolResult("call_1",
                                          nlohmann::json{{"requirements",
                                                          {{"id", "req_1"},
                                                           {"id", "req_2"},
                                                           {"id", "req_3"}}},
                                                         {"status", "ok"}});
            conversation.appendAssistant(kLongAnswer);
            conversation.appendUserImage(std::vector<std::byte>(64, std::byte{0x12}), "image/png");
            conversation.appendUserText("The jaw looks too thin at the root. Fix that.");
            conversation.appendAssistant(
              "Agreed. The root is at 4 mm where the throat needs 9 mm; I will lengthen the throat "
              "instead of thickening the jaw, which keeps the opening unchanged.");
        }

        void appendExampleDesign(design::DesignGraph & graph)
        {
            graph.setGoal("Bench clamp, 40 mm opening");
            graph.addRequirement("Opens to 40 mm", "functional");
            graph.addRequirement("Holds 2 kN at the jaw tip", "structural");
            graph.addRequirement("One-handed operation", "usability", false);
        }
    } // namespace

    bool requested()
    {
#ifdef __EMSCRIPTEN__
        return EM_ASM_INT({
            return window.location.search.indexOf("agentDemo=1") >= 0 ? 1 : 0;
        }) != 0;
#else
        char const * const value = std::getenv("GLADIUS_AGENT_DEMO");
        return value != nullptr && *value != '\0' && *value != '0';
#endif
    }

    void populate(design::DesignGraph & graph, agent::Conversation & conversation)
    {
        if (!graph.requirements().empty() || !conversation.empty())
        {
            return;
        }
        appendExampleDesign(graph);
        appendExampleTurns(conversation);
    }
} // namespace gladius::ui::agentDemo
