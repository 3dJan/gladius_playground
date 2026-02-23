/**
 * @file LibraryTool.cpp
 * @brief Implementation of MCP library tool operations
 */

#include "LibraryTool.h"

#include "../../Application.h"
#include "../../Document.h"
#include "../../ExpressionParser.h"
#include "../../ExpressionToGraphConverter.h"
#include "../../FileSystemUtils.h"
#include "../../FunctionArgument.h"
#include "../../compute/ComputeCore.h"
#include "../../io/3mf/LibraryMetadata.h"
#include "../../io/3mf/ResourceDependencyGraph.h"
#include "../../io/3mf/Writer3mf.h"
#include "../../nodes/Model.h"

#include <algorithm>
#include <filesystem>
#include <fmt/format.h>
#include <lib3mf_implicit.hpp>
#include <unordered_set>

namespace gladius::mcp::tools
{
    namespace fs = std::filesystem;

    namespace
    {
        /// @brief Open a 3MF file into a standalone lib3mf model (does not affect the app).
        Lib3MF::PModel openStandaloneModel(fs::path const & path)
        {
            auto wrapper = Lib3MF::CWrapper::loadLibrary();
            auto model = wrapper->CreateModel();
            auto reader = model->QueryReader("3mf");
            reader->ReadFromFile(path.string());
            return model;
        }

        /// @brief Convert a lib3mf port type to a string.
        std::string portTypeToString(Lib3MF::eImplicitPortType type)
        {
            switch (type)
            {
            case Lib3MF::eImplicitPortType::Scalar:
                return "float";
            case Lib3MF::eImplicitPortType::Vector:
                return "vec3";
            default:
                return "unknown";
            }
        }

        /// @brief Collect ports from an iterator into a JSON array.
        nlohmann::json collectPorts(Lib3MF::PImplicitPortIterator const & portIter)
        {
            auto ports = nlohmann::json::array();
            while (portIter->MoveNext())
            {
                auto port = portIter->GetCurrent();
                ports.push_back({{"name", port->GetDisplayName()},
                                 {"type", portTypeToString(port->GetType())}});
            }
            return ports;
        }

        /// @brief Extract function info from a model for JSON response.
        nlohmann::json extractFunctionInfo(Lib3MF::PModel const & model,
                                           std::vector<Lib3MF_uint32> const & taggedIds)
        {
            auto functions = nlohmann::json::array();
            auto funcIter = model->GetFunctions();
            while (funcIter->MoveNext())
            {
                auto func = funcIter->GetCurrentFunction();
                auto const modelResId = func->GetModelResourceID();
                bool const isTagged = std::find(taggedIds.begin(), taggedIds.end(), modelResId)
                                      != taggedIds.end();

                functions.push_back({{"resource_id", modelResId},
                                     {"name", func->GetDisplayName()},
                                     {"type", "ImplicitFunction"},
                                     {"is_tagged", isTagged},
                                     {"inputs", collectPorts(func->GetInputs())},
                                     {"outputs", collectPorts(func->GetOutputs())}});
            }
            return functions;
        }

        /// @brief Read a single library entry's summary for listing.
        nlohmann::json readEntrySummary(fs::path const & filePath, bool isShipped)
        {
            auto const name = filePath.stem().string();
            nlohmann::json entry = {
              {"name", name},
              {"is_shipped", isShipped},
              {"has_metadata", false},
              {"description", ""},
              {"tagged_function_ids", nlohmann::json::array()}
            };

            try
            {
                auto model = openStandaloneModel(filePath);
                auto metadata = io::readLibraryMetadata(model);
                if (metadata.has_value())
                {
                    entry["has_metadata"] = true;
                    entry["description"] = metadata->libraryDescription;
                    auto ids = io::parseResourceIds(metadata->libraryFunctions);
                    entry["tagged_function_ids"] = ids;
                }
            }
            catch (...)
            {
                // File unreadable — return entry with minimal info
            }

            return entry;
        }

        /// @brief Prune an exported library 3MF file to only contain the tagged function,
        /// its transitive dependencies, and any example build item chain that references it.
        ///
        /// Loads the written file into an isolated model, removes build items that don't
        /// transitively depend on the tagged function, then removes all resources that
        /// become unreachable. Finally rewrites the cleaned file.
        ///
        /// @param filePath  Path to the already-written 3MF file.
        /// @param taggedFunctionId  Model resource ID of the primary library function.
        /// @param logger  Shared logger (may be nullptr).
        /// @return Number of resources removed, or 0 if nothing was pruned.
        std::size_t pruneExportedLibraryFile(
          fs::path const & filePath,
          Lib3MF_uint32 taggedFunctionId,
          std::vector<unsigned char> const & thumbnailPng,
          events::SharedLogger logger)
        {
            try
            {
                auto model = openStandaloneModel(filePath);

                // Build a dependency graph for the isolated model
                io::ResourceDependencyGraph depGraph(model, logger);
                depGraph.buildGraph();

                // Compute the set of resources transitively required by the tagged function
                // Use UniqueResourceIDs consistently since the graph and build items use them
                std::unordered_set<Lib3MF_uint32> requiredByTaggedFunc;
                auto taggedResource = depGraph.getResourceById(taggedFunctionId);
                if (taggedResource)
                {
                    requiredByTaggedFunc.insert(taggedResource->GetResourceID());
                    auto deps = depGraph.getAllRequiredResources(taggedResource);
                    for (auto const & dep : deps)
                    {
                        requiredByTaggedFunc.insert(dep->GetResourceID());
                    }
                }

                // Identify build items whose object (levelset) transitively depends on
                // the tagged function. Keep those; remove the rest.
                {
                    std::vector<Lib3MF::PBuildItem> toRemove;
                    auto buildItemIter = model->GetBuildItems();
                    while (buildItemIter->MoveNext())
                    {
                        auto buildItem = buildItemIter->GetCurrent();
                        if (!buildItem)
                            continue;

                        auto objectId = buildItem->GetObjectResourceID();
                        bool referencesTagged = requiredByTaggedFunc.count(objectId) > 0;

                        if (!referencesTagged)
                        {
                            // Check if the build item's transitive deps overlap with the tagged set
                            auto objResource = model->GetResourceByID(objectId);
                            if (objResource)
                            {
                                auto objDeps = depGraph.getAllRequiredResources(objResource);
                                for (auto const & dep : objDeps)
                                {
                                    if (requiredByTaggedFunc.count(dep->GetResourceID()) > 0)
                                    {
                                        referencesTagged = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (!referencesTagged)
                        {
                            toRemove.push_back(buildItem);
                        }
                    }
                    for (auto const & item : toRemove)
                    {
                        model->RemoveBuildItem(item.get());
                    }
                }

                // Expand the required set to include resources needed by kept build items.
                // The tagged function's deps don't include the levelset/mesh objects that
                // *depend on* the function (reverse direction), so we must add them here.
                {
                    auto keptBuildItems = model->GetBuildItems();
                    while (keptBuildItems->MoveNext())
                    {
                        auto bi = keptBuildItems->GetCurrent();
                        if (!bi)
                            continue;
                        auto objId = bi->GetObjectResourceID();
                        // objId is a UniqueResourceID, so use GetResourceByID directly
                        auto objRes = model->GetResourceByID(objId);
                        if (objRes)
                        {
                            requiredByTaggedFunc.insert(objRes->GetResourceID());
                            for (auto const & dep : depGraph.getAllRequiredResources(objRes))
                            {
                                requiredByTaggedFunc.insert(dep->GetResourceID());
                            }
                        }
                    }
                }

                // Remove every resource that is not required by the tagged function, its
                // transitive dependencies, or the kept build items' objects.
                // Guard: if requiredByTaggedFunc is empty the tagged function was not found in
                // the model — in that case skip pruning to avoid deleting everything.
                std::vector<Lib3MF::PResource> toRemove;
                if (!requiredByTaggedFunc.empty())
                {
                    auto resIter = model->GetResources();
                    while (resIter->MoveNext())
                    {
                        auto res = resIter->GetCurrent();
                        if (res && requiredByTaggedFunc.count(res->GetResourceID()) == 0)
                        {
                            toRemove.push_back(res);
                        }
                    }
                }

                for (auto const & res : toRemove)
                {
                    model->RemoveResource(res.get());
                }

                // Embed the thumbnail into the (pruned) model if available.
                if (!thumbnailPng.empty())
                {
                    if (model->HasPackageThumbnailAttachment())
                    {
                        model->RemovePackageThumbnailAttachment();
                    }
                    auto thumb = model->CreatePackageThumbnailAttachment();
                    thumb->ReadFromBuffer(thumbnailPng);
                }

                bool const needsRewrite = !toRemove.empty() || !thumbnailPng.empty();
                if (needsRewrite)
                {
                    auto writer = model->QueryWriter("3mf");
                    writer->WriteToFile(filePath.string());
                }

                return toRemove.size();
            }
            catch (std::exception const & e)
            {
                if (logger)
                {
                    logger->addEvent(
                      {fmt::format("Warning: Export pruning failed for '{}': {}. "
                                   "The library entry may contain unrelated resources.",
                                   filePath.string(), e.what()),
                       events::Severity::Warning});
                }
                return 0; // Non-fatal — the file was already written with full content
            }
        }

        /// @brief Replace standalone x, y, z variables with pos.x, pos.y, pos.z.
        std::string transformVariablesToComponentAccess(std::string const & expression)
        {
            std::string result = expression;
            for (auto const & var : {"x", "y", "z"})
            {
                std::string const replacement = std::string("pos.") + var;
                size_t pos = 0;
                while ((pos = result.find(var, pos)) != std::string::npos)
                {
                    bool const isStandalone =
                      (pos == 0 || !std::isalnum(static_cast<unsigned char>(result[pos - 1]))) &&
                      (pos + 1 >= result.length()
                       || !std::isalnum(static_cast<unsigned char>(result[pos + 1])));
                    if (isStandalone)
                    {
                        result.replace(pos, 1, replacement);
                        pos += replacement.length();
                    }
                    else
                    {
                        ++pos;
                    }
                }
            }
            return result;
        }
    } // namespace

    LibraryTool::LibraryTool(Application * app)
        : MCPToolBase(app)
    {
    }

    nlohmann::json LibraryTool::listLibrary(std::string const & category) const
    {
        // If a category filter is specified, validate it exists
        if (!category.empty())
        {
            auto categories = getAvailableCategories();
            if (std::find(categories.begin(), categories.end(), category) == categories.end())
            {
                return createToolError(
                  fmt::format("Category '{}' not found", category),
                  {{"category", categories.empty() ? "" : categories.front()}},
                  {{"available_categories", categories}});
            }
        }

        auto const userRoot = getUserLibraryDir();
        auto const shippedRoot = getShippedLibraryDir();

        // Collect categories to scan
        auto categoryNames = category.empty()
                               ? getAvailableCategories()
                               : std::vector<std::string>{category};

        auto categoriesJson = nlohmann::json::array();
        for (auto const & catName : categoryNames)
        {
            auto entries = nlohmann::json::array();
            std::vector<std::string> seenEntries;

            // Scan user library first (takes precedence)
            auto const userCatPath = userRoot / catName;
            if (fs::exists(userCatPath) && fs::is_directory(userCatPath))
            {
                for (auto const & fileEntry : fs::directory_iterator(userCatPath))
                {
                    if (fileEntry.is_regular_file() && fileEntry.path().extension() == ".3mf")
                    {
                        auto const entryName = fileEntry.path().stem().string();
                        seenEntries.push_back(entryName);
                        entries.push_back(readEntrySummary(fileEntry.path(), false));
                    }
                }
            }

            // Scan shipped library (skip entries already seen in user library)
            auto const shippedCatPath = shippedRoot / catName;
            if (fs::exists(shippedCatPath) && fs::is_directory(shippedCatPath))
            {
                for (auto const & fileEntry : fs::directory_iterator(shippedCatPath))
                {
                    if (fileEntry.is_regular_file() && fileEntry.path().extension() == ".3mf")
                    {
                        auto const entryName = fileEntry.path().stem().string();
                        if (std::find(seenEntries.begin(), seenEntries.end(), entryName)
                            == seenEntries.end())
                        {
                            entries.push_back(readEntrySummary(fileEntry.path(), true));
                        }
                    }
                }
            }

            // Sort entries by name
            std::sort(entries.begin(), entries.end(),
                      [](auto const & a, auto const & b)
                      { return a["name"] < b["name"]; });

            // Determine if this category is only in shipped library
            bool const catInShipped = fs::exists(shippedCatPath) && fs::is_directory(shippedCatPath);
            bool const catInUser = fs::exists(userCatPath) && fs::is_directory(userCatPath);
            bool const isShippedOnly = catInShipped && !catInUser;

            categoriesJson.push_back({
              {"name", catName},
              {"is_shipped", isShippedOnly},
              {"entries", entries}
            });
        }

        return {{"success", true},
                {"library_root", userRoot.string()},
                {"categories", categoriesJson}};
    }

    nlohmann::json LibraryTool::getLibraryEntryInfo(std::string const & category,
                                                     std::string const & name) const
    {
        bool isShipped = false;
        auto const entryPath = resolveEntryPath(category, name, isShipped);

        if (entryPath.empty())
        {
            // Build helpful error with available alternatives
            auto categories = getAvailableCategories();
            bool const categoryExists = std::find(categories.begin(), categories.end(), category)
                                        != categories.end();

            if (!categoryExists)
            {
                return createToolError(
                  fmt::format("Category '{}' not found", category),
                  {{"category", categories.empty() ? "primitives" : categories.front()},
                   {"name", "sphere"}},
                  {{"available_categories", categories}});
            }

            auto entries = getAvailableEntries(category);
            return createToolError(
              fmt::format("Entry '{}' not found in category '{}'", name, category),
              {{"category", category},
               {"name", entries.empty() ? "example" : entries.front()}},
              {{"available_entries", entries}});
        }

        try
        {
            auto model = openStandaloneModel(entryPath);
            auto metadata = io::readLibraryMetadata(model);

            std::string description;
            std::vector<Lib3MF_uint32> taggedIds;
            if (metadata.has_value())
            {
                description = metadata->libraryDescription;
                taggedIds = io::parseResourceIds(metadata->libraryFunctions);
            }

            auto functions = extractFunctionInfo(model, taggedIds);

            return {{"success", true},
                    {"name", name},
                    {"category", category},
                    {"description", description},
                    {"path", entryPath.string()},
                    {"is_shipped", isShipped},
                    {"functions", functions}};
        }
        catch (std::exception const & e)
        {
            return createToolError(
              fmt::format("Failed to read library entry: {}", e.what()));
        }
    }

    nlohmann::json LibraryTool::createLibraryEntry(std::string const & name,
                                                    std::string const & category,
                                                    std::string const & expression,
                                                    std::string const & description,
                                                    bool overwrite)
    {
        if (name.empty() || category.empty() || expression.empty())
        {
            return createToolError(
              "Name, category, and expression are required",
              {{"name", "my-sphere"},
               {"category", "primitives"},
               {"expression", "sqrt(x*x + y*y + z*z) - 5"},
               {"description", "Sphere with radius 5"}});
        }

        auto const targetPath = getUserLibraryDir() / category / (name + ".3mf");

        // Check for existing file unless overwrite is requested
        if (fs::exists(targetPath) && !overwrite)
        {
            return createToolError(
              fmt::format("Library entry '{}' already exists in category '{}'", name, category),
              {{"name", name}, {"category", category}, {"overwrite", true}});
        }

        // Parse the expression
        ExpressionParser parser;
        if (!parser.parseExpression(expression))
        {
            return createToolError(
              fmt::format("Expression parsing failed: {}", parser.getLastError()),
              {{"name", "my-sphere"},
               {"category", "primitives"},
               {"expression", "sqrt(x*x + y*y + z*z) - 5"},
               {"description", "Sphere with radius 5"}},
              {{"supported_syntax",
                {{"variables", "x, y, z"},
                 {"operators", "+, -, *, /"},
                 {"functions", "sin, cos, sqrt, abs, min, max, pow"}}}});
        }

        try
        {
            // Auto-detect x,y,z and transform to pos.x, pos.y, pos.z
            auto variables = parser.getVariables();
            std::vector<FunctionArgument> arguments;
            std::string transformedExpression = expression;

            bool usesXYZ = std::any_of(variables.begin(), variables.end(),
                                       [](auto const & v)
                                       { return v == "x" || v == "y" || v == "z"; });

            if (usesXYZ)
            {
                arguments.emplace_back("pos", ArgumentType::Vector);
                transformedExpression = transformVariablesToComponentAccess(expression);
            }
            else
            {
                for (auto const & variable : variables)
                {
                    arguments.emplace_back(variable, ArgumentType::Scalar);
                }
            }

            FunctionOutput output("shape", ArgumentType::Scalar);

            // Create a nodes::Model, convert expression to graph, and save as 3MF
            nodes::Model tempModel;
            tempModel.setDisplayName(name);
            tempModel.createBeginEnd();

            auto resultNodeId = ExpressionToGraphConverter::convertExpressionToGraph(
              transformedExpression, tempModel, parser, arguments, output);

            if (resultNodeId == 0)
            {
                return createToolError("Failed to convert expression to node graph");
            }

            // Ensure target directory exists
            fs::create_directories(targetPath.parent_path());

            // Save the function to a 3MF file
            io::saveFunctionTo3mfFile(targetPath, tempModel);

            // Reopen the saved file to stamp library metadata
            auto model3mf = openStandaloneModel(targetPath);

            // Find the function's ModelResourceID
            auto funcIter = model3mf->GetFunctions();
            Lib3MF_uint32 funcModelResId = 0;
            if (funcIter->MoveNext())
            {
                funcModelResId = funcIter->GetCurrentFunction()->GetModelResourceID();
            }

            io::LibraryMetadata metadata;
            metadata.libraryFunctions = io::serializeResourceIds({funcModelResId});
            metadata.libraryDescription = description;
            io::writeLibraryMetadata(model3mf, metadata);

            auto writer = model3mf->QueryWriter("3mf");
            writer->WriteToFile(targetPath.string());

            std::string msg = fmt::format("Created library entry '{}' in category '{}'", name, category);
            if (overwrite)
            {
                msg += " (overwritten)";
            }

            return {{"success", true},
                    {"path", targetPath.string()},
                    {"name", name},
                    {"category", category},
                    {"function_id", funcModelResId},
                    {"message", msg}};
        }
        catch (std::exception const & e)
        {
            // Clean up partial file on failure
            std::error_code ec;
            fs::remove(targetPath, ec);
            return createToolError(fmt::format("Failed to create library entry: {}", e.what()));
        }
    }

    nlohmann::json LibraryTool::exportToLibrary(uint32_t functionId,
                                                 std::string const & category,
                                                 std::string const & name,
                                                 std::string const & description,
                                                 bool overwrite,
                                                 bool keepScaffold)
    {
        if (!validateActiveDocument())
        {
            return createToolError(
              "No active document. Open or create a document first.",
              {{"function_id", 5},
               {"category", "primitives"},
               {"name", "my-function"},
               {"description", "My exported function"}});
        }

        auto const targetPath = getUserLibraryDir() / category / (name + ".3mf");

        if (fs::exists(targetPath) && !overwrite)
        {
            return createToolError(
              fmt::format("Library entry '{}' already exists in category '{}'", name, category),
              {{"function_id", functionId},
               {"category", category},
               {"name", name},
               {"overwrite", true}});
        }

        try
        {
            auto document = m_application->getCurrentDocument();
            if (!document)
            {
                return createToolError("Failed to access active document");
            }

            // Sync the internal node graph to the 3MF model
            document->update3mfModel();

            // Prepare the GPU and render a thumbnail PNG for the library entry.
            // This mirrors the sequence in Document::refreshWorker():
            //   updateParameterRegistration → updateParameter → updateFlatAssembly
            //   → refreshProgram → recompile → precompute SDF → bbox
            auto logger = document->getSharedLogger();
            std::vector<unsigned char> thumbnailPng;
            if (auto computeCore = document->getCore())
            {
                try
                {
                    logger->addEvent(
                      {"exportToLibrary: starting GPU prep for thumbnail",
                       events::Severity::Info});

                    // Upload parameters to GPU (must happen before compilation)
                    document->updateParameterRegistration();
                    document->updateParameter();

                    document->updateFlatAssembly();
                    computeCore->tryRefreshProgramProtected(document->getFlatAssembly());

                    if (computeCore->prepareImageRendering())
                    {
                        logger->addEvent(
                          {"exportToLibrary: GPU ready, rendering thumbnail",
                           events::Severity::Info});
                        auto image = computeCore->createThumbnailPng();
                        thumbnailPng = std::move(image.data);
                        logger->addEvent(
                          {fmt::format("exportToLibrary: thumbnail PNG {} bytes",
                                       thumbnailPng.size()),
                           events::Severity::Info});
                    }
                    else
                    {
                        logger->addEvent(
                          {"exportToLibrary: prepareImageRendering returned false",
                           events::Severity::Warning});
                    }
                }
                catch (std::exception const & e)
                {
                    logger->addEvent(
                      {fmt::format("exportToLibrary: thumbnail generation failed: {}", e.what()),
                       events::Severity::Warning});
                }
            }
            else
            {
                logger->addEvent(
                  {"exportToLibrary: no compute core available for thumbnail",
                   events::Severity::Warning});
            }

            auto sourceModel = document->get3mfModel();
            if (!sourceModel)
            {
                return createToolError("Failed to access 3MF model");
            }

            // Verify the function exists
            bool functionFound = false;
            auto funcIter = sourceModel->GetFunctions();
            while (funcIter->MoveNext())
            {
                if (funcIter->GetCurrentFunction()->GetModelResourceID() == functionId)
                {
                    functionFound = true;
                    break;
                }
            }

            if (!functionFound)
            {
                // Collect available IDs for error message
                auto availableIds = nlohmann::json::array();
                auto iter2 = sourceModel->GetFunctions();
                while (iter2->MoveNext())
                {
                    auto func = iter2->GetCurrentFunction();
                    availableIds.push_back(
                      {{"resource_id", func->GetModelResourceID()},
                       {"name", func->GetDisplayName()}});
                }
                return createToolError(
                  fmt::format("Function with resource ID {} not found in document", functionId),
                  {{"function_id", 5},
                   {"category", category},
                   {"name", name},
                   {"description", description}},
                  {{"available_functions", availableIds}});
            }

            // Stamp library metadata on the source model
            io::LibraryMetadata metadata;
            metadata.libraryFunctions =
              io::serializeResourceIds({static_cast<Lib3MF_uint32>(functionId)});
            metadata.libraryDescription = description;
            io::writeLibraryMetadata(sourceModel, metadata);

            // Ensure target directory exists and write the full model
            fs::create_directories(targetPath.parent_path());
            {
                auto writer = sourceModel->QueryWriter("3mf");
                writer->WriteToFile(targetPath.string());
            }

            // Remove library metadata from the live source model
            io::removeLibraryMetadata(sourceModel);

            size_t prunedCount = 0;
            if (!keepScaffold)
            {
                // Prune the exported file to only include the tagged function,
                // its transitive dependencies, and any example build item chain.
                prunedCount = pruneExportedLibraryFile(
                  targetPath, static_cast<Lib3MF_uint32>(functionId),
                  thumbnailPng, logger);
            }
            else if (!thumbnailPng.empty())
            {
                // When keeping scaffold, still embed the thumbnail
                auto model = openStandaloneModel(targetPath);
                if (model->HasPackageThumbnailAttachment())
                {
                    model->RemovePackageThumbnailAttachment();
                }
                auto thumb = model->CreatePackageThumbnailAttachment();
                thumb->ReadFromBuffer(thumbnailPng);
                auto writer = model->QueryWriter("3mf");
                writer->WriteToFile(targetPath.string());
            }

            auto message = fmt::format(
              "Exported function {} to library entry '{}' in category '{}'",
              functionId, name, category);
            if (keepScaffold)
            {
                message += " (full scaffold kept)";
            }
            else if (prunedCount > 0)
            {
                message += fmt::format(" ({} unrelated resources removed)", prunedCount);
            }
            if (!thumbnailPng.empty())
            {
                message += " (thumbnail embedded)";
            }

            return {{"success", true},
                    {"path", targetPath.string()},
                    {"name", name},
                    {"category", category},
                    {"function_id", functionId},
                    {"message", message}};
        }
        catch (std::exception const & e)
        {
            // Clean up metadata from live model on failure
            try
            {
                auto document = m_application->getCurrentDocument();
                if (document)
                {
                    io::removeLibraryMetadata(document->get3mfModel());
                }
            }
            catch (...)
            {
            }
            return createToolError(fmt::format("Export failed: {}", e.what()));
        }
    }

    nlohmann::json LibraryTool::importLibraryEntry(std::string const & category,
                                                    std::string const & name)
    {
        if (!validateActiveDocument())
        {
            return createToolError(
              "No active document. Open or create a document first.",
              {{"category", "primitives"}, {"name", "sphere"}});
        }

        bool isShipped = false;
        auto const entryPath = resolveEntryPath(category, name, isShipped);
        if (entryPath.empty())
        {
            auto categories = getAvailableCategories();
            bool const categoryExists = std::find(categories.begin(), categories.end(), category)
                                        != categories.end();

            if (!categoryExists)
            {
                return createToolError(
                  fmt::format("Category '{}' not found", category),
                  {{"category", categories.empty() ? "primitives" : categories.front()},
                   {"name", "sphere"}},
                  {{"available_categories", categories}});
            }

            auto entries = getAvailableEntries(category);
            return createToolError(
              fmt::format("Entry '{}' not found in category '{}'", name, category),
              {{"category", category},
               {"name", entries.empty() ? "example" : entries.front()}},
              {{"available_entries", entries}});
        }

        try
        {
            auto document = m_application->getCurrentDocument();
            if (!document)
            {
                return createToolError("Failed to access active document");
            }

            // Read metadata to find the tagged function name for matching
            auto libModel = openStandaloneModel(entryPath);
            auto metadata = io::readLibraryMetadata(libModel);

            std::string targetFuncName;
            if (metadata.has_value())
            {
                auto taggedIds = io::parseResourceIds(metadata->libraryFunctions);
                // Find the display name of the first tagged function
                auto funcIter = libModel->GetFunctions();
                while (funcIter->MoveNext())
                {
                    auto func = funcIter->GetCurrentFunction();
                    if (!taggedIds.empty()
                        && std::find(taggedIds.begin(), taggedIds.end(),
                                     func->GetModelResourceID()) != taggedIds.end())
                    {
                        targetFuncName = func->GetDisplayName();
                        break;
                    }
                }
            }

            // If no tagged function found, use the first function name
            if (targetFuncName.empty())
            {
                auto funcIter = libModel->GetFunctions();
                if (funcIter->MoveNext())
                {
                    targetFuncName = funcIter->GetCurrentFunction()->GetDisplayName();
                }
            }

            if (targetFuncName.empty())
            {
                return createToolError(
                  fmt::format("Library entry '{}' in category '{}' contains no functions",
                              name,
                              category));
            }

            // Merge and resolve the imported function
            auto match = document->mergeAndResolve(entryPath, targetFuncName);

            if (match.id == 0)
            {
                return createToolError(
                  fmt::format("Import succeeded but no matching function '{}' was resolved",
                              targetFuncName));
            }

            auto matchName = match.model ? match.model->getDisplayName().value_or(targetFuncName)
                                         : targetFuncName;

            return {{"success", true},
                    {"name", name},
                    {"category", category},
                    {"imported_function",
                     {{"resource_id", match.id}, {"name", matchName}}},
                    {"message",
                     fmt::format("Imported function '{}' (resource ID {}) from library entry "
                                 "'{}' in category '{}'",
                                 matchName,
                                 match.id,
                                 name,
                                 category)}};
        }
        catch (std::exception const & e)
        {
            return createToolError(fmt::format("Import failed: {}", e.what()));
        }
    }

    nlohmann::json LibraryTool::deleteLibraryEntry(std::string const & category,
                                                    std::string const & name)
    {
        bool isShipped = false;
        auto const entryPath = resolveEntryPath(category, name, isShipped);

        if (entryPath.empty())
        {
            auto entries = getAvailableEntries(category);
            return createToolError(
              fmt::format("Entry '{}' not found in category '{}'", name, category),
              {{"category", category},
               {"name", entries.empty() ? "example" : entries.front()}},
              {{"available_entries", entries}});
        }

        if (isShipped)
        {
            return createToolError(
              fmt::format("Cannot delete shipped library entry '{}' in category '{}'. "
                          "Only user-created entries can be deleted.",
                          name,
                          category));
        }

        try
        {
            fs::remove(entryPath);
            return {{"success", true},
                    {"name", name},
                    {"category", category},
                    {"message",
                     fmt::format("Deleted library entry '{}' from category '{}'",
                                 name,
                                 category)}};
        }
        catch (std::exception const & e)
        {
            return createToolError(fmt::format("Failed to delete library entry: {}", e.what()));
        }
    }

    nlohmann::json
    LibraryTool::setLibraryMetadata(std::vector<uint32_t> const & functionIds,
                                    std::string const & description)
    {
        if (!validateActiveDocument())
        {
            return createToolError(
              "No active document. Open or create a document first.",
              {{"function_ids", nlohmann::json::array({5})},
               {"description", "My library entry"}});
        }

        auto document = m_application->getCurrentDocument();
        auto model = document->get3mfModel();
        if (!model)
        {
            return createToolError("Failed to access 3MF model");
        }

        // Verify all function IDs exist
        for (auto const id : functionIds)
        {
            bool found = false;
            auto funcIter = model->GetFunctions();
            while (funcIter->MoveNext())
            {
                if (funcIter->GetCurrentFunction()->GetModelResourceID() == id)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return createToolError(
                  fmt::format("Function with resource ID {} not found in document", id));
            }
        }

        std::vector<Lib3MF_uint32> lib3mfIds(functionIds.begin(), functionIds.end());
        io::LibraryMetadata metadata;
        metadata.libraryFunctions = io::serializeResourceIds(lib3mfIds);
        metadata.libraryDescription = description;
        io::writeLibraryMetadata(model, metadata);

        return {{"success", true},
                {"function_ids", functionIds},
                {"description", description},
                {"message",
                 fmt::format("Library metadata set: {} tagged function(s), description='{}'",
                             functionIds.size(),
                             description)}};
    }

    std::filesystem::path LibraryTool::resolveEntryPath(std::string const & category,
                                                        std::string const & name,
                                                        bool & isShipped) const
    {
        auto const fileName = name + ".3mf";

        // Check user library first (takes precedence)
        auto userPath = getUserLibraryDir() / category / fileName;
        if (fs::exists(userPath))
        {
            isShipped = false;
            return userPath;
        }

        // Check shipped library
        auto shippedPath = getShippedLibraryDir() / category / fileName;
        if (fs::exists(shippedPath))
        {
            isShipped = true;
            return shippedPath;
        }

        isShipped = false;
        return {};
    }

    std::vector<std::string> LibraryTool::getAvailableCategories() const
    {
        std::vector<std::string> categories;
        std::unordered_set<std::string> seen;
        auto const addCategories = [&](fs::path const & root)
        {
            if (!fs::exists(root))
            {
                return;
            }
            for (auto const & entry : fs::directory_iterator(root))
            {
                if (entry.is_directory())
                {
                    auto const name = entry.path().filename().string();
                    if (seen.insert(name).second)
                    {
                        categories.push_back(name);
                    }
                }
            }
        };

        addCategories(getUserLibraryDir());
        addCategories(getShippedLibraryDir());
        std::sort(categories.begin(), categories.end());
        return categories;
    }

    std::vector<std::string> LibraryTool::getAvailableEntries(std::string const & category) const
    {
        std::vector<std::string> entries;
        std::unordered_set<std::string> seen;
        auto const addEntries = [&](fs::path const & root)
        {
            auto const catPath = root / category;
            if (!fs::exists(catPath))
            {
                return;
            }
            for (auto const & entry : fs::directory_iterator(catPath))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".3mf")
                {
                    auto const name = entry.path().stem().string();
                    if (seen.insert(name).second)
                    {
                        entries.push_back(name);
                    }
                }
            }
        };

        addEntries(getUserLibraryDir());
        addEntries(getShippedLibraryDir());
        std::sort(entries.begin(), entries.end());
        return entries;
    }

} // namespace gladius::mcp::tools
