/**
 * @file Validator_Test.cpp
 * @brief Unit tests for Validator IssueList population (T019)
 */

#include <gtest/gtest.h>
#include "nodes/Validator.h"
#include "nodes/IssueList.h"

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

} // namespace gladius::tests
