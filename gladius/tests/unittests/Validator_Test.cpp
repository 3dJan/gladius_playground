/**
 * @file Validator_Test.cpp
 * @brief Unit tests for Validator IssueList population (T019)
 */

#include <gtest/gtest.h>
#include "nodes/Assembly.h"
#include "nodes/Validator.h"
#include "nodes/IssueList.h"

#include <algorithm>

using namespace gladius::nodes;

namespace gladius::tests
{
    class Validator_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_issueList = std::make_unique<IssueList>();
        }

        std::unique_ptr<IssueList> m_issueList;
    };

    TEST_F(Validator_Test, GetFixSuggestion_MissingConnection_ReturnsSuggestion)
    {
        auto suggestion = getFixSuggestion(IssueType::MissingConnection);

        EXPECT_FALSE(suggestion.empty());
        EXPECT_TRUE(suggestion.find("connect") != std::string::npos ||
                    suggestion.find("input") != std::string::npos);
    }

    TEST_F(Validator_Test, GetFixSuggestion_TypeMismatch_ReturnsSuggestion)
    {
        auto suggestion = getFixSuggestion(IssueType::TypeMismatch);

        EXPECT_FALSE(suggestion.empty());
        EXPECT_TRUE(suggestion.find("type") != std::string::npos ||
                    suggestion.find("convert") != std::string::npos);
    }

    TEST_F(Validator_Test, GetFixSuggestion_CyclicDependency_ReturnsSuggestion)
    {
        auto suggestion = getFixSuggestion(IssueType::CyclicDependency);

        EXPECT_FALSE(suggestion.empty());
        EXPECT_TRUE(suggestion.find("cycle") != std::string::npos ||
                    suggestion.find("connection") != std::string::npos);
    }

    TEST_F(Validator_Test, GetFixSuggestion_InvalidReference_ReturnsSuggestion)
    {
        auto suggestion = getFixSuggestion(IssueType::InvalidReference);

        EXPECT_FALSE(suggestion.empty());
    }

    TEST_F(Validator_Test, GetFixSuggestion_FunctionNotFound_ReturnsSuggestion)
    {
        auto suggestion = getFixSuggestion(IssueType::FunctionNotFound);

        EXPECT_FALSE(suggestion.empty());
        EXPECT_TRUE(suggestion.find("function") != std::string::npos ||
                    suggestion.find("exist") != std::string::npos);
    }

    TEST_F(Validator_Test, ValidationIssueKey_GeneratesUniqueKey)
    {
        ValidationIssue issue1;
        issue1.message = "Test Error";
        issue1.model = "Model1";
        issue1.node = "Node1";
        issue1.port = "port1";
        issue1.type = IssueType::MissingConnection;

        ValidationIssue issue2;
        issue2.message = "Test Error";
        issue2.model = "Model1";
        issue2.node = "Node2"; // Different node
        issue2.port = "port1";
        issue2.type = IssueType::MissingConnection;

        ValidationIssue issue3 = issue1; // Same as issue1

        // Same issues should have same key
        EXPECT_EQ(issue1.key(), issue3.key());

        // Different issues should have different keys
        EXPECT_NE(issue1.key(), issue2.key());
    }

    TEST_F(Validator_Test, IssueListClearWorks)
    {
        // Pre-populate with issues
        ValidationIssue issue;
        issue.message = "Test Issue";
        issue.type = IssueType::MissingConnection;
        issue.severity = IssueSeverity::Error;
        m_issueList->add(issue);

        EXPECT_EQ(m_issueList->size(), 1u);
        EXPECT_FALSE(m_issueList->empty());

        // Clear
        m_issueList->clear();

        EXPECT_EQ(m_issueList->size(), 0u);
        EXPECT_TRUE(m_issueList->empty());
        EXPECT_FALSE(m_issueList->hasErrors());
    }

    TEST_F(Validator_Test, GetFixSuggestion_AllIssueTypesHaveSuggestions)
    {
        // Verify all issue types have suggestions
        EXPECT_FALSE(getFixSuggestion(IssueType::MissingConnection).empty());
        EXPECT_FALSE(getFixSuggestion(IssueType::TypeMismatch).empty());
        EXPECT_FALSE(getFixSuggestion(IssueType::InvalidReference).empty());
        EXPECT_FALSE(getFixSuggestion(IssueType::CyclicDependency).empty());
        EXPECT_FALSE(getFixSuggestion(IssueType::FunctionNotFound).empty());
    }

    TEST_F(Validator_Test, Validate_FunctionCallReferencesMissingFunction_AddsFunctionNotFoundIssue)
    {
        Assembly assembly;
        constexpr ResourceId callerFunctionId = 10;
        constexpr ResourceId missingFunctionId = 999;

        assembly.addModelIfNotExisting(callerFunctionId);
        auto callerModel = assembly.findModel(callerFunctionId);
        ASSERT_TRUE(callerModel);
        callerModel->createBeginEnd();

        auto * functionCall = callerModel->create<FunctionCall>();
        ASSERT_NE(functionCall, nullptr);
        functionCall->setFunctionId(missingFunctionId);

        Validator validator;

        EXPECT_FALSE(validator.validate(assembly, *m_issueList));
        ASSERT_EQ(m_issueList->size(), 1u);

        auto issues = m_issueList->getAll();
        EXPECT_EQ(issues.front().type, IssueType::FunctionNotFound);
        EXPECT_EQ(issues.front().severity, IssueSeverity::Error);
        EXPECT_NE(issues.front().message.find("Function reference not found"), std::string::npos);
    }

    TEST_F(Validator_Test, Validate_AdditionWithMixedScalarVectorSources_ReportsTypeRuleMismatch)
    {
        Assembly assembly;
        constexpr ResourceId modelId = 11;
        assembly.addModelIfNotExisting(modelId);
        auto model = assembly.findModel(modelId);
        ASSERT_TRUE(model);
        model->createBeginEnd();

        auto * addition = model->create<Addition>();
        auto * scalar = model->create<ConstantScalar>();
        auto * vector = model->create<ConstantVector>();
        ASSERT_NE(addition, nullptr);
        ASSERT_NE(scalar, nullptr);
        ASSERT_NE(vector, nullptr);

        auto * scalarPort = model->getPort(scalar->getOutputs().at(FieldNames::Value).getId());
        auto * vectorPort = model->getPort(vector->getOutputs().at(FieldNames::Vector).getId());
        ASSERT_NE(scalarPort, nullptr);
        ASSERT_NE(vectorPort, nullptr);
        addition->parameter().at(FieldNames::A).setInputFromPort(*scalarPort);
        addition->parameter().at(FieldNames::B).setInputFromPort(*vectorPort);
        model->invalidateGraph();

        Validator validator;
        EXPECT_FALSE(validator.validate(assembly, *m_issueList));

        auto const issues = m_issueList->getAll();
        auto const hasTypeRuleIssue = std::any_of(
          issues.begin(),
          issues.end(),
          [](ValidationIssue const & issue)
          {
              return issue.type == IssueType::TypeMismatch &&
                     issue.message.find("no type rule matching") != std::string::npos;
          });
        EXPECT_TRUE(hasTypeRuleIssue);
    }

} // namespace gladius::tests
