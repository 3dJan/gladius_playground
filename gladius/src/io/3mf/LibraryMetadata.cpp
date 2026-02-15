#include "LibraryMetadata.h"

#include "ResourceDependencyGraph.h"

#include <algorithm>
#include <sstream>

namespace gladius::io
{
    std::vector<Lib3MF_uint32> parseResourceIds(std::string const & value)
    {
        std::vector<Lib3MF_uint32> ids;
        if (value.empty())
        {
            return ids;
        }

        std::istringstream stream(value);
        std::string segment;
        while (std::getline(stream, segment, ';'))
        {
            // Trim whitespace
            auto const start = segment.find_first_not_of(" \t");
            if (start == std::string::npos)
            {
                continue; // skip empty/whitespace-only segments
            }
            auto const end = segment.find_last_not_of(" \t");
            auto const trimmed = segment.substr(start, end - start + 1);

            try
            {
                auto const id = static_cast<Lib3MF_uint32>(std::stoul(trimmed));
                ids.push_back(id);
            }
            catch (std::exception const &)
            {
                // Skip non-numeric segments
            }
        }

        return ids;
    }

    std::string serializeResourceIds(std::vector<Lib3MF_uint32> const & ids)
    {
        std::string result;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i > 0)
            {
                result += ';';
            }
            result += std::to_string(ids[i]);
        }
        return result;
    }

    std::optional<LibraryMetadata> readLibraryMetadata(Lib3MF::PModel model)
    {
        if (!model)
        {
            return std::nullopt;
        }

        auto metaDataGroup = model->GetMetaDataGroup();
        if (!metaDataGroup)
        {
            return std::nullopt;
        }

        // library-functions is required for this to be a library file
        std::string functionsValue;
        try
        {
            auto meta =
              metaDataGroup->GetMetaDataByKey(LIBRARY_METADATA_NAMESPACE, LIBRARY_FUNCTIONS_KEY);
            functionsValue = meta->GetValue();
        }
        catch (...)
        {
            return std::nullopt;
        }

        if (functionsValue.empty())
        {
            return std::nullopt;
        }

        // library-description is optional
        std::string descriptionValue;
        try
        {
            auto meta =
              metaDataGroup->GetMetaDataByKey(LIBRARY_METADATA_NAMESPACE, LIBRARY_DESCRIPTION_KEY);
            descriptionValue = meta->GetValue();
        }
        catch (...)
        {
            // No description — that's fine
        }

        return LibraryMetadata{std::move(functionsValue), std::move(descriptionValue)};
    }

    namespace
    {
        /// Sets a metadata entry, creating it if it doesn't exist or updating
        /// the value if it already does. This avoids "Duplicate Model Metadata"
        /// errors from lib3mf's AddMetaData when the key is already present.
        void setMetaDataValue(Lib3MF::PMetaDataGroup const & metaDataGroup,
                              std::string const & nameSpace,
                              std::string const & name,
                              std::string const & value)
        {
            try
            {
                auto existing = metaDataGroup->GetMetaDataByKey(nameSpace, name);
                existing->SetValue(value);
            }
            catch (...)
            {
                metaDataGroup->AddMetaData(nameSpace, name, value, "xs:string", true);
            }
        }
    } // namespace

    void writeLibraryMetadata(Lib3MF::PModel model, LibraryMetadata const & metadata)
    {
        if (!model)
        {
            return;
        }

        auto metaDataGroup = model->GetMetaDataGroup();
        if (!metaDataGroup)
        {
            return;
        }

        setMetaDataValue(
          metaDataGroup, LIBRARY_METADATA_NAMESPACE, LIBRARY_FUNCTIONS_KEY, metadata.libraryFunctions);
        setMetaDataValue(metaDataGroup,
                         LIBRARY_METADATA_NAMESPACE,
                         LIBRARY_DESCRIPTION_KEY,
                         metadata.libraryDescription);
    }

    std::optional<std::unordered_set<Lib3MF_uint32>>
    computeSelectiveImportClosure(Lib3MF::PModel sourceModel,
                                  std::vector<Lib3MF_uint32> const & taggedModelResourceIds,
                                  events::SharedLogger logger)
    {
        if (!sourceModel || taggedModelResourceIds.empty())
        {
            return std::nullopt;
        }

        ResourceDependencyGraph depGraph(sourceModel, logger);
        depGraph.buildGraph();

        std::unordered_set<Lib3MF_uint32> closure;

        for (auto const taggedModelId : taggedModelResourceIds)
        {
            // Find the resource with this model resource ID
            auto resource = depGraph.getResourceById(taggedModelId);
            if (!resource)
            {
                // Tagged ID does not exist — fall back to full merge
                return std::nullopt;
            }

            closure.insert(resource->GetModelResourceID());

            // Add transitive dependencies
            auto deps = depGraph.getAllRequiredResources(resource);
            for (auto const & dep : deps)
            {
                closure.insert(dep->GetModelResourceID());
            }
        }

        return closure;
    }

    bool pruneModelForSelectiveImport(
      Lib3MF::PModel model,
      std::unordered_set<Lib3MF_uint32> const & closureModelResourceIds)
    {
        if (!model)
        {
            return false;
        }

        try
        {
            // Remove all build items
            {
                std::vector<Lib3MF::PBuildItem> buildItemsToRemove;
                auto buildItemIter = model->GetBuildItems();
                while (buildItemIter->MoveNext())
                {
                    buildItemsToRemove.push_back(buildItemIter->GetCurrent());
                }
                for (auto const & item : buildItemsToRemove)
                {
                    model->RemoveBuildItem(item.get());
                }
            }

            // Remove functions not in the closure
            {
                std::vector<Lib3MF::PResource> toRemove;
                auto resourceIter = model->GetResources();
                while (resourceIter->MoveNext())
                {
                    auto res = resourceIter->GetCurrent();
                    bool const isImplicitFunc =
                      std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(res) != nullptr;
                    bool const isFuncFromImage3d =
                      std::dynamic_pointer_cast<Lib3MF::CFunctionFromImage3D>(res) != nullptr;

                    if (isImplicitFunc || isFuncFromImage3d)
                    {
                        if (closureModelResourceIds.count(res->GetModelResourceID()) == 0)
                        {
                            toRemove.push_back(res);
                        }
                    }
                }
                for (auto const & res : toRemove)
                {
                    model->RemoveResource(res.get());
                }
            }

            return true;
        }
        catch (...)
        {
            return false;
        }
    }

} // namespace gladius::io
