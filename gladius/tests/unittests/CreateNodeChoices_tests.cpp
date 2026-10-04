#include "ui/CreateNodeChoices.h"

#include <gtest/gtest.h>
#include <nodes/DerivedNodes.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace gladius_tests
{
    namespace nodes = gladius::nodes;
    namespace ui = gladius::ui;

    namespace
    {
        ui::CreateNodeChoiceId makeNodeChoice(std::string nodeTypeName, std::string inputName)
        {
            ui::CreateNodeChoiceId choice;
            choice.kind = ui::CreateNodeChoiceKind::NodePrototype;
            choice.nodeTypeName = std::move(nodeTypeName);
            choice.inputName = std::move(inputName);
            return choice;
        }

        ui::CreateNodeChoiceId makeFunctionChoice(nodes::ResourceId functionId,
                                                  std::string inputName)
        {
            ui::CreateNodeChoiceId choice;
            choice.kind = ui::CreateNodeChoiceKind::Function;
            choice.functionId = functionId;
            choice.inputName = std::move(inputName);
            return choice;
        }
    } // namespace

    TEST(CreateNodeChoiceHistory, WhenRecordingDifferentSourceTypes_KeepsSeparateHistories)
    {
        ui::CreateNodeChoiceHistory history;
        auto const scalarChoice = makeNodeChoice("Addition", "a");
        auto const vectorChoice = makeNodeChoice("Addition", "b");

        history.record(nodes::ParameterTypeIndex::Float, scalarChoice);
        history.record(nodes::ParameterTypeIndex::Float3, vectorChoice);

        EXPECT_EQ(history.getRecent(nodes::ParameterTypeIndex::Float),
                  (std::vector<ui::CreateNodeChoiceId>{scalarChoice}));
        EXPECT_EQ(history.getRecent(nodes::ParameterTypeIndex::Float3),
                  (std::vector<ui::CreateNodeChoiceId>{vectorChoice}));
    }

    TEST(CreateNodeChoiceHistory, WhenRecordingDuplicate_MovesChoiceToFront)
    {
        ui::CreateNodeChoiceHistory history;
        auto const firstChoice = makeNodeChoice("Addition", "a");
        auto const secondChoice = makeNodeChoice("Multiplication", "b");

        history.record(nodes::ParameterTypeIndex::Float, firstChoice);
        history.record(nodes::ParameterTypeIndex::Float, secondChoice);
        history.record(nodes::ParameterTypeIndex::Float, firstChoice);

        EXPECT_EQ(history.getRecent(nodes::ParameterTypeIndex::Float),
                  (std::vector<ui::CreateNodeChoiceId>{firstChoice, secondChoice}));
    }

    TEST(CreateNodeChoiceHistory, WhenMoreThanFiveChoicesRecorded_KeepsFiveMostRecent)
    {
        ui::CreateNodeChoiceHistory history;
        std::vector<ui::CreateNodeChoiceId> choices;
        for (int index = 0; index < 7; ++index)
        {
            choices.push_back(makeNodeChoice("Node" + std::to_string(index), "input"));
            history.record(nodes::ParameterTypeIndex::Float, choices.back());
        }

        EXPECT_EQ(history.getRecent(nodes::ParameterTypeIndex::Float),
                  (std::vector<ui::CreateNodeChoiceId>{choices[6],
                                                      choices[5],
                                                      choices[4],
                                                      choices[3],
                                                      choices[2]}));
    }

    TEST(CreateNodeChoiceHistory, WhenCleared_RemovesAllSourceTypeHistories)
    {
        ui::CreateNodeChoiceHistory history;
        history.record(nodes::ParameterTypeIndex::Float, makeNodeChoice("Addition", "a"));
        history.record(nodes::ParameterTypeIndex::Float3, makeNodeChoice("Addition", "b"));

        history.clear();

        EXPECT_TRUE(history.getRecent(nodes::ParameterTypeIndex::Float).empty());
        EXPECT_TRUE(history.getRecent(nodes::ParameterTypeIndex::Float3).empty());
    }

    TEST(CreateNodeChoices, CollectCompatibleChoices_ListsInputsAndFunctionArguments)
    {
        nodes::Model currentModel;
        nodes::NodeTypes nodeTypes;
        nodes::Assembly assembly;
        nodes::Port sourcePort;
        sourcePort.setTypeIndex(nodes::ParameterTypeIndex::Float);

        auto function = std::make_shared<nodes::Model>();
        function->createBeginEnd();
        function->setResourceId(42);
        function->setDisplayName("Scalar Function");
        function->getBeginNode()->addOutputPort("distance", nodes::ParameterTypeIndex::Float);
        function->getBeginNode()->addOutputPort("position", nodes::ParameterTypeIndex::Float3);
        assembly.getFunctions().emplace(42, function);

        auto const choices = ui::collectCompatibleCreateNodeChoices(
          currentModel, sourcePort, assembly, nodeTypes);

        auto const hasAdditionInput = std::any_of(
          choices.begin(),
          choices.end(),
          [](ui::CreateNodeChoice const & choice)
          {
              return choice.id.kind == ui::CreateNodeChoiceKind::NodePrototype &&
                     choice.id.nodeTypeName == "Addition" && !choice.id.inputName.empty();
          });
        auto const functionChoice = std::find_if(
          choices.begin(),
          choices.end(),
          [](ui::CreateNodeChoice const & choice)
          {
              return choice.id == makeFunctionChoice(42, "distance");
          });

        EXPECT_TRUE(hasAdditionInput);
        ASSERT_NE(functionChoice, choices.end());
        EXPECT_EQ(functionChoice->label, "Scalar Function :: distance");
        EXPECT_EQ(std::count_if(choices.begin(),
                                choices.end(),
                                [](ui::CreateNodeChoice const & choice)
                                { return choice.id.kind == ui::CreateNodeChoiceKind::Function; }),
                  1);
        EXPECT_EQ(function->getBeginNode()->outputs().size(), 2);
    }

    TEST(CreateNodeChoices, CollectCompatibleChoices_SkipsFunctionWithoutEndNode)
    {
        nodes::Model currentModel;
        nodes::NodeTypes nodeTypes;
        nodes::Assembly assembly;
        nodes::Port sourcePort;
        sourcePort.setTypeIndex(nodes::ParameterTypeIndex::Float);

        auto function = std::make_shared<nodes::Model>();
        nodes::Begin beginPrototype;
        auto * beginNode = function->create(beginPrototype);
        beginNode->addOutputPort("distance", nodes::ParameterTypeIndex::Float);
        function->setResourceId(43);
        function->setDisplayName("Incomplete Function");
        assembly.getFunctions().emplace(43, function);

        auto const choices = ui::collectCompatibleCreateNodeChoices(
          currentModel, sourcePort, assembly, nodeTypes);

        EXPECT_EQ(std::count_if(choices.begin(),
                                choices.end(),
                                [](ui::CreateNodeChoice const & choice)
                                { return choice.id.kind == ui::CreateNodeChoiceKind::Function; }),
                  0);
        EXPECT_EQ(function->getEndNode(), nullptr);
    }

    TEST(CreateNodeChoices, CollectCompatibleChoices_DisambiguatesFunctionsWithSameName)
    {
        nodes::Model currentModel;
        nodes::NodeTypes nodeTypes;
        nodes::Assembly assembly;
        nodes::Port sourcePort;
        sourcePort.setTypeIndex(nodes::ParameterTypeIndex::Float);

        for (auto const functionId : {42, 44})
        {
            auto function = std::make_shared<nodes::Model>();
            function->createBeginEnd();
            function->setResourceId(functionId);
            function->setDisplayName("Duplicate Function");
            function->getBeginNode()->addOutputPort("distance", nodes::ParameterTypeIndex::Float);
            assembly.getFunctions().emplace(functionId, std::move(function));
        }

        auto const choices = ui::collectCompatibleCreateNodeChoices(
          currentModel, sourcePort, assembly, nodeTypes);
        std::vector<ui::CreateNodeChoice const *> functionChoices;
        for (auto const & choice : choices)
        {
            if (choice.id.kind == ui::CreateNodeChoiceKind::Function)
            {
                functionChoices.push_back(&choice);
            }
        }

        ASSERT_EQ(functionChoices.size(), 2);
        EXPECT_NE(functionChoices[0]->label, functionChoices[1]->label);
        EXPECT_NE(functionChoices[0]->label.find("42"), std::string::npos);
        EXPECT_NE(functionChoices[1]->label.find("44"), std::string::npos);
    }

    TEST(CreateNodeChoices, CollectCompatibleChoices_SkipsUnconfiguredFunctionCallPrototype)
    {
        nodes::Model currentModel;
        nodes::NodeTypes nodeTypes;
        nodes::Assembly assembly;
        nodes::Port sourcePort;
        sourcePort.setTypeIndex(nodes::ParameterTypeIndex::ResourceId);

        auto const choices = ui::collectCompatibleCreateNodeChoices(
          currentModel, sourcePort, assembly, nodeTypes);

        EXPECT_EQ(std::count_if(choices.begin(),
                                choices.end(),
                                [](ui::CreateNodeChoice const & choice)
                                { return choice.id.nodeTypeName == "FunctionCall"; }),
                  0);
    }
} // namespace gladius_tests