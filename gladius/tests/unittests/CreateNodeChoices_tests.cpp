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

        ui::CreateNodeChoiceId makeDifferenceChoice(std::string inputName)
        {
            ui::CreateNodeChoiceId choice;
            choice.kind = ui::CreateNodeChoiceKind::LibraryFunction;
            choice.inputName = std::move(inputName);
            choice.semantic = ui::CreateNodeChoiceSemantic::Difference;
            choice.libraryAsset = "csg/subtraction.3mf";
            choice.functionName = "subtraction";
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

    TEST(CreateNodeChoiceHistory, SemanticChoicesRemainDistinctFromGenericOperators)
    {
        ui::CreateNodeChoiceHistory history;
        auto const genericMin = makeNodeChoice("Min", "A");
        auto unionAlias = makeNodeChoice("Min", "A");
        unionAlias.semantic = ui::CreateNodeChoiceSemantic::Union;
        auto const differenceA = makeDifferenceChoice("shapeA");
        auto const differenceB = makeDifferenceChoice("shapeB");

        history.record(nodes::ParameterTypeIndex::Float, genericMin);
        history.record(nodes::ParameterTypeIndex::Float, unionAlias);
        history.record(nodes::ParameterTypeIndex::Float, differenceA);
        history.record(nodes::ParameterTypeIndex::Float, differenceB);

        EXPECT_EQ(history.getRecent(nodes::ParameterTypeIndex::Float),
                  (std::vector<ui::CreateNodeChoiceId>{differenceB,
                                                      differenceA,
                                                      unionAlias,
                                                      genericMin}));
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

    TEST(CreateNodeChoices, CollectCompatibleFloatChoices_IncludesBooleanAliasesAndOrderedDifference)
    {
        nodes::Model currentModel;
        nodes::NodeTypes nodeTypes;
        nodes::Assembly assembly;
        nodes::Port sourcePort;
        sourcePort.setTypeIndex(nodes::ParameterTypeIndex::Float);

                auto const choices = ui::collectCompatibleCreateNodeChoices(
                    currentModel, sourcePort, assembly, nodeTypes);

                auto const unionAlias = std::find_if(
                    choices.begin(), choices.end(), [](ui::CreateNodeChoice const & choice)
                    { return choice.id.semantic == ui::CreateNodeChoiceSemantic::Union; });
                auto const intersectionAlias = std::find_if(
                    choices.begin(), choices.end(), [](ui::CreateNodeChoice const & choice)
                    { return choice.id.semantic == ui::CreateNodeChoiceSemantic::Intersection; });
                ASSERT_NE(unionAlias, choices.end());
                ASSERT_NE(intersectionAlias, choices.end());
                EXPECT_EQ(unionAlias->id.nodeTypeName, "Min");
                EXPECT_EQ(unionAlias->id.inputName, "A");
                EXPECT_EQ(unionAlias->category, nodes::Category::BoolOperation);
                EXPECT_EQ(unionAlias->label, "Union");
                EXPECT_EQ(intersectionAlias->id.nodeTypeName, "Max");
                EXPECT_EQ(intersectionAlias->id.inputName, "A");
                EXPECT_EQ(intersectionAlias->category, nodes::Category::BoolOperation);
                EXPECT_EQ(intersectionAlias->label, "Intersection");

                auto const genericMinA = std::find_if(
                    choices.begin(), choices.end(), [](ui::CreateNodeChoice const & choice)
                    {
                            return choice.id.kind == ui::CreateNodeChoiceKind::NodePrototype &&
                                         choice.id.semantic == ui::CreateNodeChoiceSemantic::None &&
                                         choice.id.nodeTypeName == "Min" && choice.id.inputName == "A";
                    });
                ASSERT_NE(genericMinA, choices.end());
                EXPECT_EQ(genericMinA->category, nodes::Category::Math);

                auto const differenceA = std::find_if(
                    choices.begin(), choices.end(), [](ui::CreateNodeChoice const & choice)
                    { return choice.id == makeDifferenceChoice("shapeA"); });
                auto const differenceB = std::find_if(
                    choices.begin(), choices.end(), [](ui::CreateNodeChoice const & choice)
                    { return choice.id == makeDifferenceChoice("shapeB"); });
                ASSERT_NE(differenceA, choices.end());
                ASSERT_NE(differenceB, choices.end());
                EXPECT_EQ(differenceA->category, nodes::Category::BoolOperation);
                EXPECT_EQ(differenceA->label, "Difference (selected - other)");
            EXPECT_EQ(differenceB->label, "Difference (other - selected)");
        }

    TEST(CreateNodeChoices, CollectCompatibleChoices_NonScalarSourcesHaveNoSemanticBooleanChoices)
    {
        nodes::Model currentModel;
        nodes::NodeTypes nodeTypes;
        nodes::Assembly assembly;

        for (auto const sourceType : {nodes::ParameterTypeIndex::Float3,
                                      nodes::ParameterTypeIndex::Matrix4})
        {
            nodes::Port sourcePort;
            sourcePort.setTypeIndex(sourceType);
            auto const choices = ui::collectCompatibleCreateNodeChoices(
              currentModel, sourcePort, assembly, nodeTypes);

            EXPECT_EQ(std::count_if(choices.begin(),
                                    choices.end(),
                                    [](ui::CreateNodeChoice const & choice)
                                    {
                                        return choice.id.semantic !=
                                               ui::CreateNodeChoiceSemantic::None;
                                    }),
                      0);
        }
    }

        TEST(CreateNodeChoices, CollectCompatibleChoices_HidesImportedDifferenceFromGenericFunctions)
        {
                nodes::Model currentModel;
                nodes::NodeTypes nodeTypes;
                nodes::Assembly assembly;
                nodes::Port sourcePort;
                sourcePort.setTypeIndex(nodes::ParameterTypeIndex::Float);

                auto difference = std::make_shared<nodes::Model>();
                difference->createBeginEnd();
                difference->addArgument("shapeA", nodes::VariantParameter(float{0.0f}));
                difference->addArgument("shapeB", nodes::VariantParameter(float{0.0f}));
                difference->addFunctionOutput("shape", nodes::VariantParameter(float{0.0f}));
                auto * negativeOne = difference->create<nodes::ConstantScalar>();
                negativeOne->parameter().at(nodes::FieldNames::Value).setValue(-1.0f);
                auto * negation = difference->create<nodes::Multiplication>();
                auto * maximum = difference->create<nodes::Max>();
                ASSERT_TRUE(difference->addLink(
                    negativeOne->getValueOutputPort().getId(), negation->getInputA().getId()));
                ASSERT_TRUE(difference->addLink(
                    difference->getInputs().at("shapeB").getId(), negation->getInputB().getId()));
                ASSERT_TRUE(difference->addLink(
                    difference->getInputs().at("shapeA").getId(), maximum->getInputA().getId()));
                ASSERT_TRUE(difference->addLink(
                    negation->getResultOutputPort().getId(), maximum->getInputB().getId()));
                ASSERT_TRUE(difference->addLink(
                    maximum->getResultOutputPort().getId(), difference->getOutputs().at("shape").getId()));
                difference->setResourceId(45);
                difference->setDisplayName("subtraction");
                assembly.getFunctions().emplace(45, difference);

                EXPECT_TRUE(ui::isSdfDifferenceLibraryFunction(*difference));
                auto const choices = ui::collectCompatibleCreateNodeChoices(
                    currentModel, sourcePort, assembly, nodeTypes);

                EXPECT_EQ(std::count_if(choices.begin(),
                                                                choices.end(),
                                                                [](ui::CreateNodeChoice const & choice)
                                                                { return choice.id.kind == ui::CreateNodeChoiceKind::Function; }),
                                    0);
                EXPECT_EQ(std::count_if(choices.begin(),
                                                                choices.end(),
                                                                [](ui::CreateNodeChoice const & choice)
                                                                { return choice.id.kind == ui::CreateNodeChoiceKind::LibraryFunction; }),
                                    2);
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