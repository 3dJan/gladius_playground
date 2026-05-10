#include "IssueList.h"

#include <algorithm>
#include <fmt/format.h>

namespace gladius::nodes
{
    std::string ValidationIssue::key() const
    {
        return fmt::format("{}:{}:{}:{}:{}", model, node, port, parameter, static_cast<int>(type));
    }

    void IssueList::clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_issues.clear();
    }

    void IssueList::add(ValidationIssue issue)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_issues.push_back(std::move(issue));
    }

    std::vector<ValidationIssue> IssueList::getAll() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_issues;
    }

    std::vector<ValidationIssue> IssueList::getForModel(ResourceId modelId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<ValidationIssue> result;
        result.reserve(m_issues.size());
        
        std::copy_if(m_issues.begin(),
                     m_issues.end(),
                     std::back_inserter(result),
                     [modelId](ValidationIssue const& issue) { return issue.modelId == modelId; });
        
        return result;
    }

    bool IssueList::hasErrors() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::any_of(m_issues.begin(), m_issues.end(), [](ValidationIssue const& issue) {
            return issue.severity == IssueSeverity::Error;
        });
    }

    size_t IssueList::errorCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<size_t>(std::count_if(m_issues.begin(), m_issues.end(), [](ValidationIssue const& issue) {
            return issue.severity == IssueSeverity::Error;
        }));
    }

    size_t IssueList::warningCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<size_t>(std::count_if(m_issues.begin(), m_issues.end(), [](ValidationIssue const& issue) {
            return issue.severity == IssueSeverity::Warning;
        }));
    }

    bool IssueList::empty() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_issues.empty();
    }

    size_t IssueList::size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_issues.size();
    }
} // namespace gladius::nodes
