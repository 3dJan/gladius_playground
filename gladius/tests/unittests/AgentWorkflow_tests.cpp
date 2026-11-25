/**
 * @file AgentWorkflow_tests.cpp
 * @brief Unit tests for the workflow rail derivation and its presentation-only animation.
 * @details These are the only pure-logic parts of the new Design Agent panel: everything else
 *          needs an ImGui frame. The tests focus on the honesty guarantees - phase state must come
 *          from authoritative design state only, and Attention must only appear for real signals.
 */

#include <gtest/gtest.h>

#include "ui/AgentWorkflow.h"
#include "ui/AgentWorkflowAnimation.h"

#include "design/DesignGraph.h"
#include "design/TodoStore.h"

namespace gladius::ui::tests
{
    namespace
    {
        std::size_t indexOfState(std::array<PhaseState, kWorkflowPhaseCount> const & states,
                                 PhaseState state)
        {
            std::size_t found = kWorkflowPhaseCount;
            for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
            {
                if (states[i] == state)
                {
                    found = i;
                }
            }
            return found;
        }
    } // namespace

    TEST(UserPhaseOf, EngineeringPhases_MapOntoDisplayedRail)
    {
        EXPECT_EQ(userPhaseOf(design::DesignPhase::ClarifyRequirements), WorkflowPhase::Understand);
        EXPECT_EQ(userPhaseOf(design::DesignPhase::DecomposeFunctions), WorkflowPhase::Decompose);
        EXPECT_EQ(userPhaseOf(design::DesignPhase::GenerateSolutionPrinciples),
                  WorkflowPhase::Explore);
        EXPECT_EQ(userPhaseOf(design::DesignPhase::BuildSystemConcepts), WorkflowPhase::Explore);
        EXPECT_EQ(userPhaseOf(design::DesignPhase::EvaluateConcepts), WorkflowPhase::Explore);
        EXPECT_EQ(userPhaseOf(design::DesignPhase::InstantiateEnvelopes), WorkflowPhase::Develop);
        EXPECT_EQ(userPhaseOf(design::DesignPhase::DevelopComponents), WorkflowPhase::Develop);
        EXPECT_EQ(userPhaseOf(design::DesignPhase::IntegrateSystem), WorkflowPhase::Develop);
        EXPECT_EQ(userPhaseOf(design::DesignPhase::Refine), WorkflowPhase::Develop);
        EXPECT_EQ(userPhaseOf(design::DesignPhase::Validate), WorkflowPhase::Validate);
        EXPECT_EQ(userPhaseOf(design::DesignPhase::Complete), WorkflowPhase::Validate);
    }

    TEST(PhaseLabels, FivePhases_HaveLabelsAndBoundedLookup)
    {
        EXPECT_STREQ(phaseLabel(WorkflowPhase::Understand), "Understand");
        EXPECT_STREQ(phaseLabel(WorkflowPhase::Decompose), "Decompose");
        EXPECT_STREQ(phaseLabel(WorkflowPhase::Explore), "Explore");
        EXPECT_STREQ(phaseLabel(WorkflowPhase::Develop), "Develop");
        EXPECT_STREQ(phaseLabel(WorkflowPhase::Validate), "Validate");
        EXPECT_EQ(phaseAt(99), WorkflowPhase::Validate);
        EXPECT_EQ(indexOf(WorkflowPhase::Explore), std::size_t{2});
    }

    TEST(DerivePhaseStates, FreshDesign_OnlyUnderstandIsActive)
    {
        design::DesignGraph graph;
        auto const states = derivePhaseStates(graph, WorkflowSignals{});

        EXPECT_EQ(states[indexOf(WorkflowPhase::Understand)], PhaseState::Active);
        for (std::size_t i = 1; i < kWorkflowPhaseCount; ++i)
        {
            EXPECT_EQ(states[i], PhaseState::Pending);
        }
    }

    TEST(DerivePhaseStates, MidDesign_EarlierPhasesCountAsCompleted)
    {
        design::DesignGraph graph;
        graph.setPhase(design::DesignPhase::DevelopComponents);
        auto const states = derivePhaseStates(graph, WorkflowSignals{});

        EXPECT_EQ(indexOfState(states, PhaseState::Active), indexOf(WorkflowPhase::Develop));
        for (std::size_t i = 0; i < indexOf(WorkflowPhase::Develop); ++i)
        {
            EXPECT_EQ(states[i], PhaseState::Completed);
        }
        EXPECT_EQ(states[indexOf(WorkflowPhase::Validate)], PhaseState::Pending);
    }

    TEST(DerivePhaseStates, CompletedDesign_EveryPhaseCompleted)
    {
        design::DesignGraph graph;
        graph.setPhase(design::DesignPhase::Complete);
        auto const states = derivePhaseStates(graph, WorkflowSignals{});

        EXPECT_TRUE(isDesignComplete(graph));
        for (auto const state : states)
        {
            EXPECT_EQ(state, PhaseState::Completed);
        }
    }

    TEST(DerivePhaseStates, FollowUpRequest_ValidateFallsBackToDevelop)
    {
        design::DesignGraph graph;
        graph.setPhase(design::DesignPhase::Complete);
        EXPECT_TRUE(graph.reopenForFollowUpRequest("make the base wider"));

        auto const states = derivePhaseStates(graph, WorkflowSignals{});
        EXPECT_EQ(indexOfState(states, PhaseState::Active), indexOf(WorkflowPhase::Develop));
        EXPECT_EQ(states[indexOf(WorkflowPhase::Validate)], PhaseState::Pending);
    }

    TEST(DerivePhaseStates, FailedRun_ActivePhaseShowsAttention)
    {
        design::DesignGraph graph;
        graph.setPhase(design::DesignPhase::DevelopComponents);
        WorkflowSignals signals;
        signals.agentFailed = true;

        auto const states = derivePhaseStates(graph, signals);
        EXPECT_EQ(states[indexOf(WorkflowPhase::Develop)], PhaseState::Attention);
        EXPECT_EQ(states[indexOf(WorkflowPhase::Understand)], PhaseState::Completed);
    }

    TEST(DerivePhaseStates, BlockedPlanning_DevelopShowsAttention)
    {
        design::DesignGraph graph;
        graph.setPhase(design::DesignPhase::GenerateSolutionPrinciples);
        design::TodoStore todos;
        std::string const id = todos.addItem("Build the shade");
        EXPECT_FALSE(hasBlockedPlanning(todos));
        todos.updateItem(id, design::TodoStatus::Blocked);

        WorkflowSignals signals;
        signals.planningBlocked = hasBlockedPlanning(todos);
        auto const states = derivePhaseStates(graph, signals);

        EXPECT_TRUE(signals.planningBlocked);
        EXPECT_EQ(states[indexOf(WorkflowPhase::Develop)], PhaseState::Attention);
        EXPECT_EQ(states[indexOf(WorkflowPhase::Explore)], PhaseState::Active);
    }

    TEST(DerivePhaseStates, DeterministicWarnings_ValidateShowsAttention)
    {
        design::DesignGraph graph;
        graph.setPhase(design::DesignPhase::DevelopComponents);

        nlohmann::json state = nlohmann::json::parse(R"({
            "phase": "DevelopComponents",
            "configurations": [
                { "id": "cfg_1", "name": "A", "active": true,
                  "evaluation": { "deterministicWarnings": [ "Wall below 0.8 mm" ] } }
            ]
        })");
        ASSERT_TRUE(graph.fromJson(state));

        WorkflowSignals signals;
        std::size_t passed = 0;
        countDeterministicResults(graph, passed, signals.deterministicWarnings);
        EXPECT_EQ(passed, std::size_t{0});
        EXPECT_EQ(signals.deterministicWarnings, std::size_t{1});

        auto const states = derivePhaseStates(graph, signals);
        EXPECT_EQ(states[indexOf(WorkflowPhase::Validate)], PhaseState::Attention);
    }

    TEST(CountDeterministicResults, NoActiveConfiguration_ReportsNothing)
    {
        design::DesignGraph graph;
        graph.createConfiguration("A", {});
        std::size_t passed = 7;
        std::size_t warnings = 7;
        countDeterministicResults(graph, passed, warnings);
        EXPECT_EQ(passed, std::size_t{0});
        EXPECT_EQ(warnings, std::size_t{0});
    }

    TEST(CountDeterministicResults, ActiveConfiguration_CountsMeasuredEntriesOnly)
    {
        design::DesignGraph graph;
        std::string const cfgId = graph.createConfiguration("A", {});
        graph.activateConfiguration(cfgId);

        nlohmann::json state = graph.toJson();
        state["configurations"][0]["evaluation"]["deterministicPassed"] =
          nlohmann::json::array({"watertight", "wall thickness ok"});
        state["configurations"][0]["evaluation"]["deterministicWarnings"] =
          nlohmann::json::array({"overhang"});
        state["configurations"][0]["evaluation"]["heuristicNotes"] =
          nlohmann::json::array({"looks sturdy"});
        ASSERT_TRUE(graph.fromJson(state));

        std::size_t passed = 0;
        std::size_t warnings = 0;
        countDeterministicResults(graph, passed, warnings);
        EXPECT_EQ(passed, std::size_t{2});
        EXPECT_EQ(warnings, std::size_t{1});
    }

    TEST(AnimationState, RapidPhaseAdvance_OnlyTheNewestTransitionAnimates)
    {
        AgentWorkflowAnimationState animation;
        std::array<PhaseState, kWorkflowPhaseCount> states{};
        states.fill(PhaseState::Pending);

        animation.update(0, states, 0.0f);
        states[0] = PhaseState::Completed;
        states[2] = PhaseState::Active;
        animation.update(2, states, 0.01f);
        states[1] = PhaseState::Completed;
        states[2] = PhaseState::Pending;
        states[3] = PhaseState::Active;
        animation.update(3, states, 0.02f);

        ASSERT_TRUE(animation.lineTransitionActive());
        EXPECT_EQ(animation.lineTransitionFrom(), std::size_t{2});
        EXPECT_NEAR(animation.lineTransitionProgress(0.02f), 0.0f, 1e-5f);
        EXPECT_NEAR(animation.lineTransitionProgress(1.0f), 1.0f, 1e-5f);
    }

    TEST(AnimationState, BackwardPhaseChange_SkipsTheLineTransition)
    {
        AgentWorkflowAnimationState animation;
        std::array<PhaseState, kWorkflowPhaseCount> states{};
        states.fill(PhaseState::Pending);
        animation.update(3, states, 0.0f);

        animation.update(1, states, 0.5f);
        EXPECT_FALSE(animation.lineTransitionActive());
        EXPECT_NEAR(animation.lineTransitionProgress(0.5f), 1.0f, 1e-5f);
    }

    TEST(AnimationState, CompletionPulse_RisesOnceThenSettles)
    {
        AgentWorkflowAnimationState animation;
        std::array<PhaseState, kWorkflowPhaseCount> states{};
        states.fill(PhaseState::Pending);
        animation.update(0, states, 0.0f);

        states[0] = PhaseState::Completed;
        states[1] = PhaseState::Active;
        animation.update(1, states, 1.0f);

        float const mid = animation.completionPulse(0, 1.0f + 0.5f
                                                        * AgentWorkflowAnimationState::
                                                            kCompletionPulseSeconds);
        EXPECT_NEAR(mid, 1.0f, 1e-5f);
        EXPECT_NEAR(animation.completionPulse(0, 5.0f), 0.0f, 1e-5f);
        EXPECT_NEAR(animation.completionPulse(4, 1.1f), 0.0f, 1e-5f);
    }

    TEST(AnimationState, ArtifactArrival_FadesInAndKnownRowsStayOpaque)
    {
        AgentWorkflowAnimationState animation;
        EXPECT_NEAR(animation.artifactArrival("req_1", 3.0f), 1.0f, 1e-5f);

        animation.noteArtifact("req_1", 5.0f);
        EXPECT_NEAR(animation.artifactArrival("req_1", 5.0f), 0.0f, 1e-5f);
        EXPECT_NEAR(animation.artifactArrival(
                      "req_1",
                      5.0f + 0.5f * AgentWorkflowAnimationState::kArtifactArrivalSeconds),
                    0.5f,
                    1e-5f);
        EXPECT_NEAR(animation.artifactArrival("req_1", 30.0f), 1.0f, 1e-5f);
    }

    TEST(AnimationState, Reset_ForgetsTimingsForANewRun)
    {
        AgentWorkflowAnimationState animation;
        std::array<PhaseState, kWorkflowPhaseCount> states{};
        states.fill(PhaseState::Completed);
        animation.update(4, states, 10.0f);
        animation.noteArtifact("cfg_1", 10.0f);

        animation.reset();
        EXPECT_FALSE(animation.lineTransitionActive());
        EXPECT_NEAR(animation.completionPulse(4, 10.0f), 0.0f, 1e-5f);
        EXPECT_NEAR(animation.artifactArrival("cfg_1", 10.0f), 1.0f, 1e-5f);

        states.fill(PhaseState::Pending);
        states[0] = PhaseState::Active;
        animation.update(0, states, 11.0f);
        EXPECT_FALSE(animation.lineTransitionActive());
    }
} // namespace gladius::ui::tests
