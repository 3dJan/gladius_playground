#pragma once

#include "Assembly.h"
#include "IssueList.h"
#include "Parameter.h"
#include "nodesfwd.h"

namespace gladius::nodes
{
    struct ValidationError
    {
        std::string message;
        std::string model;
        std::string node;
        std::string port;
        std::string parameter;
    };

    using ValidationErrors = std::vector<ValidationError>;

    /**
     * @brief Get a fix suggestion for a given issue type
     *
     * @param type The issue type
     * @return A human-readable suggestion for fixing the issue
     */
    [[nodiscard]] std::string getFixSuggestion(IssueType type);

    class Validator
    {
      public:
        Validator() = default;
        ~Validator() = default;

        /**
         * @brief Validate the assembly and populate the issue list
         *
         * @param assembly The assembly to validate
         * @param issueList The issue list to populate with validation issues
         * @return True if the assembly is valid (no errors), false otherwise
         */
        [[nodiscard]] bool validate(Assembly & assembly, IssueList & issueList);

        /**
         * @brief Validate the assembly (legacy interface)
         *
         * @param assembly The assembly to validate
         * @return True if the assembly is valid, false otherwise
         */
        [[nodiscard]] bool validate(Assembly & assembly);

        [[nodiscard]] ValidationErrors const & getErrors() const;

      private:
        void validateModel(Model & model, Assembly & assembly, IssueList & issueList);
        void validateNode(NodeBase & node, Model & model, Assembly & assembly, IssueList & issueList);
        void validateNodeImpl(NodeBase & node, Model & model, IssueList & issueList);

        void validateNode(FunctionCall & node, Model & model, Assembly & assembly, IssueList & issueList);
        ValidationErrors m_errors;
    };
}