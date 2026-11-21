#pragma once

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace gladius::design
{
    /// @brief Explicit phases of the systematic engineering-design process
    enum class DesignPhase
    {
        ClarifyRequirements,
        DecomposeFunctions,
        GenerateSolutionPrinciples,
        BuildSystemConcepts,
        EvaluateConcepts,
        InstantiateEnvelopes,
        DevelopComponents,
        IntegrateSystem,
        Validate,
        Refine,
        Complete
    };

    [[nodiscard]] inline char const * toString(DesignPhase phase) noexcept
    {
        switch (phase)
        {
        case DesignPhase::ClarifyRequirements:
            return "ClarifyRequirements";
        case DesignPhase::DecomposeFunctions:
            return "DecomposeFunctions";
        case DesignPhase::GenerateSolutionPrinciples:
            return "GenerateSolutionPrinciples";
        case DesignPhase::BuildSystemConcepts:
            return "BuildSystemConcepts";
        case DesignPhase::EvaluateConcepts:
            return "EvaluateConcepts";
        case DesignPhase::InstantiateEnvelopes:
            return "InstantiateEnvelopes";
        case DesignPhase::DevelopComponents:
            return "DevelopComponents";
        case DesignPhase::IntegrateSystem:
            return "IntegrateSystem";
        case DesignPhase::Validate:
            return "Validate";
        case DesignPhase::Refine:
            return "Refine";
        case DesignPhase::Complete:
            return "Complete";
        }
        return "ClarifyRequirements";
    }

    enum class FidelityLevel
    {
        Symbolic,
        Envelope,
        Structural,
        Detailed
    };

    [[nodiscard]] inline char const * toString(FidelityLevel level) noexcept
    {
        switch (level)
        {
        case FidelityLevel::Symbolic:
            return "symbolic";
        case FidelityLevel::Envelope:
            return "envelope";
        case FidelityLevel::Structural:
            return "structural";
        case FidelityLevel::Detailed:
            return "detailed";
        }
        return "symbolic";
    }

    enum class DesignStatus
    {
        Proposed,
        Active,
        Frozen,
        Rejected
    };

    enum class DesignNodeType
    {
        System,
        FunctionGroup,
        Function,
        Component,
        Feature
    };

    struct Requirement
    {
        std::string id;
        std::string description;
        std::string category = "functional";
        bool mandatory = true;
    };

    struct Constraint
    {
        std::string id;
        std::string expression;
        std::string rationale;
    };

    struct Interface
    {
        std::string id;
        std::string name;
        std::string kind;
        nlohmann::json specification = nlohmann::json::object();
    };

    /// @brief Separates calculated engineering facts from heuristic and subjective assessments
    struct Evaluation
    {
        std::vector<std::string> deterministicPassed;
        std::vector<std::string> deterministicWarnings;
        std::vector<std::string> heuristicNotes;
        std::vector<std::string> subjectiveNotes;
        double pruningScore = 0.0;
    };

    struct DesignVariant
    {
        std::string id;
        std::string nodeId;
        std::string name;
        std::string rationale;
        FidelityLevel fidelity = FidelityLevel::Symbolic;
        DesignStatus status = DesignStatus::Proposed;
        nlohmann::json parameters = nlohmann::json::object();
        std::vector<Constraint> constraints;
        std::vector<Interface> interfaces;
        Evaluation evaluation;
        uint32_t modelResourceId = 0;
        std::vector<std::byte> previewPng;
    };

    struct DesignNode
    {
        std::string id;
        DesignNodeType type = DesignNodeType::Function;
        std::string name;
        std::vector<std::string> addressedRequirements;
        std::vector<std::string> children;
        std::vector<std::string> variantIds;
        DesignStatus status = DesignStatus::Proposed;
    };

    struct Configuration
    {
        std::string id;
        std::string name;
        std::unordered_map<std::string, std::string> selectedVariants; // nodeId -> variantId
        Evaluation evaluation;
        bool active = false;
        std::vector<std::byte> previewPng;
    };
} // namespace gladius::design
