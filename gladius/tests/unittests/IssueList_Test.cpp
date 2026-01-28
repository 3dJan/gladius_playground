#include <gtest/gtest.h>

#include "nodes/IssueList.h"

namespace gladius::nodes::tests
{
    class IssueList_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_issueList = std::make_unique<IssueList>();
        }

        ValidationIssue createErrorIssue(std::string const& message, ResourceId modelId = 1, NodeId nodeId = 100)
        {
            ValidationIssue issue;
            issue.message = message;
            issue.model = "TestModel";
            issue.node = "TestNode";
            issue.port = "input";
            issue.parameter = "value";
            issue.type = IssueType::MissingConnection;
            issue.severity = IssueSeverity::Error;
            issue.fixSuggestion = "Connect an output to this parameter";
            issue.modelId = modelId;
            issue.nodeId = nodeId;
            return issue;
        }

        ValidationIssue createWarningIssue(std::string const& message, ResourceId modelId = 1)
        {
            ValidationIssue issue = createErrorIssue(message, modelId);
            issue.severity = IssueSeverity::Warning;
            return issue;
        }

        std::unique_ptr<IssueList> m_issueList;
    };

    TEST_F(IssueList_Test, NewIssueList_IsEmpty)
    {
        EXPECT_TRUE(m_issueList->empty());
        EXPECT_EQ(m_issueList->size(), 0u);
        EXPECT_FALSE(m_issueList->hasErrors());
        EXPECT_EQ(m_issueList->errorCount(), 0u);
        EXPECT_EQ(m_issueList->warningCount(), 0u);
    }

    TEST_F(IssueList_Test, AddIssue_IncreasesSize)
    {
        m_issueList->add(createErrorIssue("Test error"));

        EXPECT_FALSE(m_issueList->empty());
        EXPECT_EQ(m_issueList->size(), 1u);
    }

    TEST_F(IssueList_Test, AddMultipleIssues_TracksAll)
    {
        m_issueList->add(createErrorIssue("Error 1"));
        m_issueList->add(createErrorIssue("Error 2"));
        m_issueList->add(createWarningIssue("Warning 1"));

        EXPECT_EQ(m_issueList->size(), 3u);
    }

    TEST_F(IssueList_Test, Clear_RemovesAllIssues)
    {
        m_issueList->add(createErrorIssue("Error 1"));
        m_issueList->add(createErrorIssue("Error 2"));

        m_issueList->clear();

        EXPECT_TRUE(m_issueList->empty());
        EXPECT_EQ(m_issueList->size(), 0u);
    }

    TEST_F(IssueList_Test, HasErrors_ReturnsTrueWhenErrorExists)
    {
        m_issueList->add(createWarningIssue("Warning"));
        EXPECT_FALSE(m_issueList->hasErrors());

        m_issueList->add(createErrorIssue("Error"));
        EXPECT_TRUE(m_issueList->hasErrors());
    }

    TEST_F(IssueList_Test, HasErrors_ReturnsFalseForWarningsOnly)
    {
        m_issueList->add(createWarningIssue("Warning 1"));
        m_issueList->add(createWarningIssue("Warning 2"));

        EXPECT_FALSE(m_issueList->hasErrors());
    }

    TEST_F(IssueList_Test, ErrorCount_CountsOnlyErrors)
    {
        m_issueList->add(createErrorIssue("Error 1"));
        m_issueList->add(createWarningIssue("Warning 1"));
        m_issueList->add(createErrorIssue("Error 2"));
        m_issueList->add(createWarningIssue("Warning 2"));

        EXPECT_EQ(m_issueList->errorCount(), 2u);
    }

    TEST_F(IssueList_Test, WarningCount_CountsOnlyWarnings)
    {
        m_issueList->add(createErrorIssue("Error 1"));
        m_issueList->add(createWarningIssue("Warning 1"));
        m_issueList->add(createErrorIssue("Error 2"));
        m_issueList->add(createWarningIssue("Warning 2"));

        EXPECT_EQ(m_issueList->warningCount(), 2u);
    }

    TEST_F(IssueList_Test, GetAll_ReturnsAllIssues)
    {
        m_issueList->add(createErrorIssue("Error 1"));
        m_issueList->add(createErrorIssue("Error 2"));

        auto issues = m_issueList->getAll();

        EXPECT_EQ(issues.size(), 2u);
        EXPECT_EQ(issues[0].message, "Error 1");
        EXPECT_EQ(issues[1].message, "Error 2");
    }

    TEST_F(IssueList_Test, GetAll_ReturnsCopy)
    {
        m_issueList->add(createErrorIssue("Error 1"));

        auto issues = m_issueList->getAll();
        issues.clear();

        EXPECT_EQ(m_issueList->size(), 1u);
    }

    TEST_F(IssueList_Test, GetForModel_FiltersCorrectly)
    {
        m_issueList->add(createErrorIssue("Model1 Error", 1));
        m_issueList->add(createErrorIssue("Model2 Error", 2));
        m_issueList->add(createErrorIssue("Model1 Error 2", 1));

        auto model1Issues = m_issueList->getForModel(1);
        auto model2Issues = m_issueList->getForModel(2);
        auto model3Issues = m_issueList->getForModel(3);

        EXPECT_EQ(model1Issues.size(), 2u);
        EXPECT_EQ(model2Issues.size(), 1u);
        EXPECT_EQ(model3Issues.size(), 0u);
    }

    TEST_F(IssueList_Test, ValidationIssue_KeyIsUnique)
    {
        ValidationIssue issue1;
        issue1.model = "Model1";
        issue1.node = "Node1";
        issue1.port = "port1";
        issue1.parameter = "param1";
        issue1.type = IssueType::MissingConnection;

        ValidationIssue issue2 = issue1;
        issue2.node = "Node2";

        EXPECT_NE(issue1.key(), issue2.key());
    }

    TEST_F(IssueList_Test, ValidationIssue_SameFieldsSameKey)
    {
        ValidationIssue issue1;
        issue1.model = "Model1";
        issue1.node = "Node1";
        issue1.port = "port1";
        issue1.parameter = "param1";
        issue1.type = IssueType::MissingConnection;

        ValidationIssue issue2 = issue1;

        EXPECT_EQ(issue1.key(), issue2.key());
    }

    TEST_F(IssueList_Test, IssueType_AllValuesDistinguishable)
    {
        ValidationIssue base;
        base.model = "M";
        base.node = "N";
        base.port = "P";
        base.parameter = "p";

        base.type = IssueType::MissingConnection;
        auto key1 = base.key();

        base.type = IssueType::TypeMismatch;
        auto key2 = base.key();

        base.type = IssueType::InvalidReference;
        auto key3 = base.key();

        base.type = IssueType::CyclicDependency;
        auto key4 = base.key();

        base.type = IssueType::FunctionNotFound;
        auto key5 = base.key();

        EXPECT_NE(key1, key2);
        EXPECT_NE(key2, key3);
        EXPECT_NE(key3, key4);
        EXPECT_NE(key4, key5);
    }
} // namespace gladius::nodes::tests
