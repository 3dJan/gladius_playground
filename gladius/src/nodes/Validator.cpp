
#include "Validator.h"
#include "DerivedNodes.h"
#include "graph/GraphAlgorithms.h"
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

namespace gladius::nodes
{
    std::string getFixSuggestion(IssueType type)
    {
        switch (type)
        {
        case IssueType::MissingConnection:
            return "Connect an output from another node to this input parameter.";
        case IssueType::TypeMismatch:
            return "Ensure the connected output type matches the expected input type.";
        case IssueType::InvalidReference:
            return "Reconnect the parameter to a valid node and port. The referenced node or port "
                   "may have been deleted.";
        case IssueType::CyclicDependency:
            return "Remove one or more connections to break the cycle in the graph.";
        case IssueType::FunctionNotFound:
            return "Ensure the referenced function exists or create a new function with the "
                   "expected name.";
        case IssueType::GraphSyncError:
            return "Review the graph for invalid nodes or broken references, then save or "
                   "reload the model.";
        }
        return "Review and fix this issue.";
    }

    bool Validator::validate(Assembly & assembly, IssueList & issueList)
    {
        ZoneScopedN("ValidateAssembly");
        m_errors.clear();
        issueList.clear();

        for (auto & [name, function] : assembly.getFunctions())
        {
            // Skip validation for managed functions (e.g., auto-generated FunctionFromImage3D)
            if (function->isManaged())
            {
                continue;
            }
            validateModel(*function, assembly, issueList);
        }

        return !issueList.hasErrors();
    }

    bool Validator::validate(Assembly & assembly)
    {
        IssueList discardedIssueList;
        bool const result = validate(assembly, discardedIssueList);
        return result;
    }

    ValidationErrors const & Validator::getErrors() const
    {
        return m_errors;
    }

    void Validator::validateModel(Model & model, Assembly & assembly, IssueList & issueList)
    {
        ZoneScopedN("ValidateModel");
        model.updateGraphAndOrderIfNeeded();
        model.updateTypes();

        model.updateValidityState();

        // Check for cyclic dependencies - this is a model-level issue
        if (graph::isCyclic(model.getGraph()))
        {
            auto modelDisplayName = model.getDisplayName().value_or("unknown");
            auto modelId = model.getResourceId();
            std::string modelInfo = fmt::format("{} (ID: {})", modelDisplayName, modelId);

            auto issue = ValidationIssue{
              .message = fmt::format("Function '{}' contains a cyclic dependency.", modelDisplayName),
              .model = modelInfo,
              .node = "",
              .port = "",
              .parameter = "",
              .type = IssueType::CyclicDependency,
              .severity = IssueSeverity::Error,
              .fixSuggestion = getFixSuggestion(IssueType::CyclicDependency),
              .modelId = modelId,
              .nodeId = {}};
            issueList.add(std::move(issue));

            m_errors.push_back(ValidationError{
              fmt::format("Function '{}' contains a cyclic dependency. Remove one or more "
                          "connections to break the cycle.",
                          modelDisplayName),
              modelInfo,
              "",
              "",
              ""});
            model.setIsValid(false);
        }

        for (auto & [nodeId, node] : model)
        {
            validateNode(*node, model, assembly, issueList);
        }
    }

    void Validator::validateNode(NodeBase & node,
                                 Model & model,
                                 Assembly & assembly,
                                 IssueList & issueList)
    {
      if (auto * functionCall = dynamic_cast<FunctionCall *>(&node); functionCall != nullptr)
      {
        validateNode(*functionCall, model, assembly, issueList);
        return;
      }

        validateNodeImpl(node, model, issueList);
    }

    void Validator::validateNodeImpl(NodeBase & node, Model & model, IssueList & issueList)
    {
        // Skip validation for nodes that are exempt from input validation
        if (node.isExemptFromInputValidation())
        {
            return;
        }

        // Create a descriptive model identifier that includes both name and ID
        auto modelDisplayName = model.getDisplayName().value_or("unknown");
        auto modelId = model.getResourceId();
        std::string modelInfo = fmt::format("{} (ID: {})", modelDisplayName, modelId);

        for (auto & [parameterName, parameter] : node.parameter())
        {
            if (!parameter.getConstSource().has_value() && parameter.isInputSourceRequired())
            {
                auto issue = ValidationIssue{
                  .message = fmt::format(
                    "Node '{}' requires input for parameter '{}' but no connection found.",
                    node.getDisplayName(),
                    parameterName),
                  .model = modelInfo,
                  .node = node.getDisplayName(),
                  .port = "unknown",
                  .parameter = parameterName,
                  .type = IssueType::MissingConnection,
                  .severity = IssueSeverity::Error,
                  .fixSuggestion = getFixSuggestion(IssueType::MissingConnection),
                  .modelId = modelId,
                  .nodeId = node.getId()};
                issueList.add(std::move(issue));

                m_errors.push_back(ValidationError{
                  fmt::format(
                    "Node '{}' requires input for parameter '{}' but no connection found. "
                    "Connect an output from another node to this parameter.",
                    node.getDisplayName(),
                    parameterName),
                  modelInfo,
                  node.getDisplayName(),
                  "unknown",
                  parameterName});
                model.setIsValid(false);
            }
            if (parameter.getConstSource().has_value())
            {
                auto const & source = parameter.getConstSource().value();
                auto referendedPort = model.getPort(source.portId);
                parameter.setValid(true);

                if (!referendedPort)
                {
                    auto issue = ValidationIssue{
                      .message =
                        fmt::format("Parameter '{}' of node '{}' references a non-existing port.",
                                    parameterName,
                                    node.getDisplayName()),
                      .model = modelInfo,
                      .node = node.getDisplayName(),
                      .port = "unknown",
                      .parameter = parameterName,
                      .type = IssueType::InvalidReference,
                      .severity = IssueSeverity::Error,
                      .fixSuggestion = getFixSuggestion(IssueType::InvalidReference),
                      .modelId = modelId,
                      .nodeId = node.getId()};
                    issueList.add(std::move(issue));

                    m_errors.push_back(ValidationError{
                      fmt::format("Parameter '{}' of node '{}' references a non-existing port. "
                                  "The referenced node or port may have been deleted.",
                                  parameterName,
                                  node.getDisplayName()), // message
                      modelInfo,                          // model
                      node.getDisplayName(),              // node
                      "unknown",                          // port
                      parameterName});                    // parameter

                    parameter.setValid(false);
                    model.setIsValid(false);
                    continue;
                }

                if (parameter.getTypeIndex() != referendedPort->getTypeIndex())
                {
                    auto issue = ValidationIssue{
                      .message =
                        fmt::format("Type mismatch: Parameter '{}' of node '{}' expects different "
                                    "type than provided by port '{}'.",
                                    parameterName,
                                    node.getDisplayName(),
                                    referendedPort->getUniqueName()),
                      .model = modelInfo,
                      .node = node.getDisplayName(),
                      .port = referendedPort->getUniqueName(),
                      .parameter = parameterName,
                      .type = IssueType::TypeMismatch,
                      .severity = IssueSeverity::Error,
                      .fixSuggestion = getFixSuggestion(IssueType::TypeMismatch),
                      .modelId = modelId,
                      .nodeId = node.getId()};
                    issueList.add(std::move(issue));

                    m_errors.push_back(ValidationError{
                      fmt::format(
                        "Type mismatch: Parameter '{}' of node '{}' expects different data type "
                        "than provided by connected port '{}'. Check node documentation for "
                        "required types.",
                        parameterName,
                        node.getDisplayName(),
                        referendedPort->getUniqueName()), // message
                      modelInfo,                          // model
                      node.getDisplayName(),              // node
                      referendedPort->getUniqueName(),    // port
                      parameterName});                    // parameter
                    parameter.setValid(false);
                    model.setIsValid(false);
                }
            }
        }
    }

    void Validator::validateNode(FunctionCall & node,
                                 Model & model,
                                 Assembly & assembly,
                                 IssueList & issueList)
    {
        validateNodeImpl(node, model, issueList);
        node.resolveFunctionId();
        auto referencedId = node.getFunctionId();
        auto referencedModel = assembly.findModel(referencedId);

        // Create a descriptive model identifier that includes both name and ID
        auto modelDisplayName = model.getDisplayName().value_or("unknown");
        auto modelId = model.getResourceId();
        std::string modelInfo = fmt::format("{} (ID: {})", modelDisplayName, modelId);

        if (!referencedModel)
        {
            auto issue = ValidationIssue{.message = "Function reference not found",
                                         .model = modelInfo,
                                         .node = node.getDisplayName(),
                                         .port = "unknown",
                                         .parameter = "FunctionId",
                                         .type = IssueType::FunctionNotFound,
                                         .severity = IssueSeverity::Error,
                                         .fixSuggestion = getFixSuggestion(IssueType::FunctionNotFound),
                                         .modelId = modelId,
                                         .nodeId = node.getId()};
            issueList.add(std::move(issue));

            m_errors.push_back(ValidationError{"Function reference not found",
                                               modelInfo,
                                               node.getDisplayName(),
                                               "unknown",
                                               "FunctionId"});
            model.setIsValid(false);
        }
    }
}
