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

    [[nodiscard]] inline DesignPhase designPhaseFromString(std::string const & str) noexcept
    {
        if (str == "DecomposeFunctions")
            return DesignPhase::DecomposeFunctions;
        if (str == "GenerateSolutionPrinciples")
            return DesignPhase::GenerateSolutionPrinciples;
        if (str == "BuildSystemConcepts")
            return DesignPhase::BuildSystemConcepts;
        if (str == "EvaluateConcepts")
            return DesignPhase::EvaluateConcepts;
        if (str == "InstantiateEnvelopes")
            return DesignPhase::InstantiateEnvelopes;
        if (str == "DevelopComponents")
            return DesignPhase::DevelopComponents;
        if (str == "IntegrateSystem")
            return DesignPhase::IntegrateSystem;
        if (str == "Validate")
            return DesignPhase::Validate;
        if (str == "Refine")
            return DesignPhase::Refine;
        if (str == "Complete")
            return DesignPhase::Complete;
        return DesignPhase::ClarifyRequirements;
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

    [[nodiscard]] inline FidelityLevel fidelityLevelFromString(std::string const & str) noexcept
    {
        if (str == "envelope")
            return FidelityLevel::Envelope;
        if (str == "structural")
            return FidelityLevel::Structural;
        if (str == "detailed")
            return FidelityLevel::Detailed;
        return FidelityLevel::Symbolic;
    }

    enum class DesignStatus
    {
        Proposed,
        Active,
        Frozen,
        Rejected
    };

    [[nodiscard]] inline char const * toString(DesignStatus status) noexcept
    {
        switch (status)
        {
        case DesignStatus::Proposed:
            return "proposed";
        case DesignStatus::Active:
            return "active";
        case DesignStatus::Frozen:
            return "frozen";
        case DesignStatus::Rejected:
            return "rejected";
        }
        return "proposed";
    }

    [[nodiscard]] inline DesignStatus designStatusFromString(std::string const & str) noexcept
    {
        if (str == "active")
            return DesignStatus::Active;
        if (str == "frozen")
            return DesignStatus::Frozen;
        if (str == "rejected")
            return DesignStatus::Rejected;
        return DesignStatus::Proposed;
    }

    enum class DesignNodeType
    {
        System,
        FunctionGroup,
        Function,
        Component,
        Feature
    };

    [[nodiscard]] inline char const * toString(DesignNodeType type) noexcept
    {
        switch (type)
        {
        case DesignNodeType::System:
            return "system";
        case DesignNodeType::FunctionGroup:
            return "function_group";
        case DesignNodeType::Function:
            return "function";
        case DesignNodeType::Component:
            return "component";
        case DesignNodeType::Feature:
            return "feature";
        }
        return "function";
    }

    [[nodiscard]] inline DesignNodeType designNodeTypeFromString(std::string const & str) noexcept
    {
        if (str == "system")
            return DesignNodeType::System;
        if (str == "function_group")
            return DesignNodeType::FunctionGroup;
        if (str == "component")
            return DesignNodeType::Component;
        if (str == "feature")
            return DesignNodeType::Feature;
        return DesignNodeType::Function;
    }

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
        std::string functionName;
        std::vector<std::byte> previewPng;
        std::string userFeedback;
    };

    /// @brief Generated or attached visual reference image stored as a design artifact
    struct ReferenceImage
    {
        std::string id;
        std::string prompt;
        std::string caption;
        std::string linkedNodeId;
        std::string linkedVariantId;
        std::string mimeType = "image/png";
        std::vector<std::byte> pngBytes;
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
        std::string userFeedback;
    };
} // namespace gladius::design
