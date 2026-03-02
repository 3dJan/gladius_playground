#pragma once

#include "Model.h"
#include "nodesfwd.h"
#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace gladius::nodes
{
    using Models = std::map<ResourceId, SharedModel>; // The models should be sorted for convience
    using ModelNames = std::vector<std::string>;

    using AssemblyException = std::runtime_error;

    using OptionalFAllbackvalue = std::optional<double>;

    class ModelDoesNotExist : public AssemblyException
    {
      public:
        explicit ModelDoesNotExist(NodeName const & missingModelName)
            : AssemblyException("The Assembly does not contain a model with the name " +
                                missingModelName)
        {
        }
    };
    class ModelDoesAlreadyExist : public AssemblyException
    {
      public:
        explicit ModelDoesAlreadyExist(NodeName const & nameOfTheModelToAdd)
            : AssemblyException("The Assembly does already contain a model with the name " +
                                nameOfTheModelToAdd)
        {
        }
    };

    /// Result of findImportedFunction().
    struct FunctionMatch
    {
        ResourceId id{0};
        SharedModel model;
    };

    class Assembly
    {
      public:
        Assembly();

        // copy ctor
        Assembly(Assembly const & other);

        void visitNodes(Visitor & visitor);

        void visitAssemblyNodes(Visitor & visitor);

        [[nodiscard]] auto getFunctions() -> Models &;
        [[nodiscard]] auto getFunctions() const -> Models const &;

        auto assemblyModel() -> SharedModel &;

        auto addModelIfNotExisting(ResourceId id) -> bool;

        auto getModelId(std::string const & name) -> int;

        void deleteModel(ResourceId id);

        bool equals(Assembly const & other);

        void setFilename(std::filesystem::path fileName)
        {
            m_fileName = fileName;
        }

        [[nodiscard]] std::filesystem::path getFilename() const
        {
            return m_fileName;
        }

        [[nodiscard]] bool isValid();

        void setAssemblyModelId(ResourceId id)
        {
            m_assemblyModelId = id;
        }

        [[nodiscard]] ResourceId getAssemblyModelId() const
        {
            return m_assemblyModelId;
        }

        SharedModel findModel(ResourceId id);

        /// @brief Find the best matching function after merging a library file.
        /// Prefers newly imported functions whose display name matches the target.
        /// @param targetName Display name to match (empty = pick first new function).
        /// @param existingIds Function IDs present before the merge.
        /// @param excludeModel Model to exclude from matching (e.g. the current model).
        [[nodiscard]] FunctionMatch findImportedFunction(
            std::string const & targetName,
            std::set<ResourceId> const & existingIds,
            SharedModel const & excludeModel) const;

        void updateInputsAndOutputs();

        void setFallbackValueLevelSet(OptionalFAllbackvalue value)
        {
            m_FallbackValueLevelSet = value;
        }

        [[nodiscard]] OptionalFAllbackvalue getFallbackValueLevelSet() const
        {
            return m_FallbackValueLevelSet;
        }

      private:
        Models m_subModels;
        ResourceId m_assemblyModelId{};
        std::filesystem::path m_fileName{};

        OptionalFAllbackvalue m_FallbackValueLevelSet{};
    };
} // namespace gladius::nodes
