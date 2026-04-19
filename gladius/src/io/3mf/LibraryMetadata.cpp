#include "LibraryMetadata.h"

#include "ResourceDependencyGraph.h"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
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

            unsigned long val = 0;
            auto const [ptr, ec] =
              std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), val);
            if (ec == std::errc{})
            {
                ids.push_back(static_cast<Lib3MF_uint32>(val));
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

        // Iterate by index instead of GetMetaDataByKey, which can throw
        // "Referenced resource must not be nullptr" on deserialized models.
        std::string functionsValue;
        std::string descriptionValue;
        std::string tagsValue;

        try
        {
            auto const count = metaDataGroup->GetMetaDataCount();
            for (Lib3MF_uint32 i = 0; i < count; ++i)
            {
                auto meta = metaDataGroup->GetMetaData(i);
                if (!meta || meta->GetNameSpace() != LIBRARY_METADATA_NAMESPACE)
                {
                    continue;
                }
                if (meta->GetName() == LIBRARY_FUNCTIONS_KEY)
                {
                    functionsValue = meta->GetValue();
                }
                else if (meta->GetName() == LIBRARY_DESCRIPTION_KEY)
                {
                    descriptionValue = meta->GetValue();
                }
                else if (meta->GetName() == LIBRARY_TAGS_KEY)
                {
                    tagsValue = meta->GetValue();
                }
            }
        }
        catch (...)
        {
            return std::nullopt;
        }

        if (functionsValue.empty())
        {
            return std::nullopt;
        }

        return LibraryMetadata{
            std::move(functionsValue), std::move(descriptionValue), std::move(tagsValue)};
    }

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

        // Try to update existing entries first, add new ones only if not found.
        // We iterate manually instead of using GetMetaDataByKey because that
        // method can crash on models deserialized from certain buffers.
        auto setOrAdd = [&](std::string const & name, std::string const & value)
        {
            auto const count = metaDataGroup->GetMetaDataCount();
            for (Lib3MF_uint32 i = 0; i < count; ++i)
            {
                auto meta = metaDataGroup->GetMetaData(i);
                if (meta && meta->GetNameSpace() == LIBRARY_METADATA_NAMESPACE &&
                    meta->GetName() == name)
                {
                    meta->SetValue(value);
                    return;
                }
            }
            metaDataGroup->AddMetaData(
              LIBRARY_METADATA_NAMESPACE, name, value, "xs:string", true);
        };

        setOrAdd(LIBRARY_FUNCTIONS_KEY, metadata.libraryFunctions);
        setOrAdd(LIBRARY_DESCRIPTION_KEY, metadata.libraryDescription);
        setOrAdd(LIBRARY_TAGS_KEY, metadata.libraryTags);
    }

    void removeLibraryMetadata(Lib3MF::PModel model)
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

        // Remove in reverse order to avoid index invalidation.
        try
        {
            auto count = metaDataGroup->GetMetaDataCount();
            for (Lib3MF_uint32 i = count; i > 0; --i)
            {
                auto meta = metaDataGroup->GetMetaData(i - 1);
                if (meta && meta->GetNameSpace() == LIBRARY_METADATA_NAMESPACE)
                {
                    metaDataGroup->RemoveMetaDataByIndex(i - 1);
                }
            }
        }
        catch (...)
        {
            // Best-effort cleanup — ignore errors.
        }
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

        return computeSelectiveImportClosure(depGraph, taggedModelResourceIds);
    }

    std::optional<std::unordered_set<Lib3MF_uint32>>
    computeSelectiveImportClosure(ResourceDependencyGraph const & depGraph,
                                  std::vector<Lib3MF_uint32> const & taggedModelResourceIds)
    {
        if (taggedModelResourceIds.empty())
        {
            return std::nullopt;
        }

        std::unordered_set<Lib3MF_uint32> closure;

        for (auto const taggedModelId : taggedModelResourceIds)
        {
            auto resource = depGraph.getResourceById(taggedModelId);
            if (!resource)
            {
                return std::nullopt;
            }

            closure.insert(resource->GetModelResourceID());

            auto deps = depGraph.getAllRequiredResources(resource);
            for (auto const & dep : deps)
            {
                closure.insert(dep->GetModelResourceID());
            }
        }

        return closure;
    }

    std::optional<Lib3MF::PModel>
    pruneSourceForImport(std::filesystem::path const & filePath,
                         events::SharedLogger logger)
    {
        try
        {
            // 1. Read library file into a standalone model.
            auto wrapper = Lib3MF::CWrapper::loadLibrary();
            auto sourceModel = wrapper->CreateModel();
            auto reader = sourceModel->QueryReader("3mf");
            reader->SetStrictModeActive(false);
            reader->ReadFromFile(filePath.string());

            // 2. Read metadata — if missing, fall back to full merge.
            auto metadata = readLibraryMetadata(sourceModel);
            if (!metadata.has_value())
            {
                return std::nullopt;
            }

            auto taggedIds = parseResourceIds(metadata->libraryFunctions);
            if (taggedIds.empty())
            {
                return std::nullopt;
            }

            // 3. Compute transitive dependency closure.
            ResourceDependencyGraph depGraph(sourceModel, logger);
            depGraph.buildGraph();

            auto closure = computeSelectiveImportClosure(depGraph, taggedIds);
            if (!closure.has_value())
            {
                return std::nullopt;
            }

            // 4. Remove all build items — they reference the example/main function
            //    and its mesh chain which are only needed for standalone viewing,
            //    not for merging the library function into another document.
            {
                std::vector<Lib3MF::PBuildItem> buildItemsToRemove;
                auto buildItemIter = sourceModel->GetBuildItems();
                while (buildItemIter->MoveNext())
                {
                    buildItemsToRemove.push_back(buildItemIter->GetCurrent());
                }
                for (auto const & item : buildItemsToRemove)
                {
                    sourceModel->RemoveBuildItem(item.get());
                }
            }

            // 5. Remove all resources outside the closure.
            std::vector<Lib3MF::PResource> resourcesToRemove;
            auto resIter = sourceModel->GetResources();
            while (resIter->MoveNext())
            {
                auto res = resIter->GetCurrent();
                if (res && closure->count(res->GetModelResourceID()) == 0)
                {
                    resourcesToRemove.push_back(res);
                }
            }

            for (auto const & res : resourcesToRemove)
            {
                sourceModel->RemoveResource(res.get());
            }

            if (logger)
            {
                logger->addEvent(
                  {fmt::format("Selective import: kept {} resources, removed {}",
                               closure->size(),
                               resourcesToRemove.size()),
                   events::Severity::Info});
            }

            // 7. Round-trip through buffer to get a clean model without dangling
            //    ResourceIdNode references from removed resources.
            auto writer = sourceModel->QueryWriter("3mf");
            std::vector<Lib3MF_uint8> buffer;
            writer->WriteToBuffer(buffer);

            auto cleanModel = wrapper->CreateModel();
            auto cleanReader = cleanModel->QueryReader("3mf");
            cleanReader->SetStrictModeActive(false);
            cleanReader->ReadFromBuffer(buffer);

            return cleanModel;
        }
        catch (std::exception const & e)
        {
            if (logger)
            {
                logger->addEvent(
                  {fmt::format("Selective import pruning failed for '{}': {}. "
                               "Falling back to full merge.",
                               filePath.string(),
                               e.what()),
                   events::Severity::Warning});
            }
            return std::nullopt;
        }
    }

} // namespace gladius::io
