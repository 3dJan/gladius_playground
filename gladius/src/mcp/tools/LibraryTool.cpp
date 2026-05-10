/**
 * @file LibraryTool.cpp
 * @brief Implementation of MCP library tool operations
 */

#include "LibraryTool.h"

#include "DocumentLifecycleTool.h"
#include "FunctionOperationsTool.h"

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
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fmt/format.h>
#include <lib3mf_implicit.hpp>
#include <sstream>
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
              {"tags", ""},
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
                    entry["tags"] = metadata->libraryTags;
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

        /// @brief Check if a 3MF function has an output with the given identifier.
        bool functionHasOutput(Lib3MF::PModel const & model,
                              Lib3MF_uint32 functionId,
                              std::string const & outputName)
        {
            auto funcIter = model->GetFunctions();
            while (funcIter->MoveNext())
            {
                auto func = funcIter->GetCurrentFunction();
                if (func->GetModelResourceID() != functionId)
                {
                    continue;
                }
                auto outIt = func->GetOutputs();
                while (outIt->MoveNext())
                {
                    if (outIt->GetCurrent()->GetIdentifier() == outputName)
                    {
                        return true;
                    }
                }
                return false;
            }
            return false;
        }

        /// @brief Validate that all resource references in the 3MF model can be resolved.
        ///
        /// Checks each levelset's function + channel reference and any volumetric color
        /// references, mirroring what the Importer/Builder would verify during loading.
        /// Returns an empty string on success, or an error message on failure.
        std::string validate3mfReferences(Lib3MF::PModel const & model)
        {
            auto resIt = model->GetResources();
            while (resIt->MoveNext())
            {
                auto resource = resIt->GetCurrent();
                auto * levelSet = dynamic_cast<Lib3MF::CLevelSet *>(resource.get());
                if (levelSet == nullptr)
                {
                    continue;
                }

                // Check levelset -> function -> channel output
                auto fn = levelSet->GetFunction();
                if (!fn)
                {
                    return fmt::format("Levelset (ID: {}) references no function.",
                                       resource->GetModelResourceID());
                }
                auto funcId = fn->GetModelResourceID();
                auto channel = levelSet->GetChannelName();
                if (channel.empty())
                {
                    channel = "shape";
                }
                if (!functionHasOutput(model, funcId, channel))
                {
                    return fmt::format(
                      "Levelset references function (ID: {}) with channel '{}', "
                      "but that function has no output named '{}'. "
                      "Ensure main uses the multi-output syntax: "
                      "(float shape) main_3(vec3 pos) {{ ... }}",
                      funcId, channel, channel);
                }

                // Check volumetric color reference
                try
                {
                    auto volumeData = levelSet->GetVolumeData();
                    if (volumeData)
                    {
                        auto color = volumeData->GetColor();
                        if (color)
                        {
                            auto colorFuncUniqueId = color->GetFunctionResourceID();
                            auto colorRes = model->GetResourceByID(colorFuncUniqueId);
                            if (!colorRes)
                            {
                                return fmt::format(
                                  "Volumetric color references function (unique ID: {}), "
                                  "but that resource does not exist.",
                                  colorFuncUniqueId);
                            }
                            auto colorFuncId = colorRes->GetModelResourceID();
                            if (!functionHasOutput(model, colorFuncId, "color"))
                            {
                                return fmt::format(
                                  "Volumetric color references function (ID: {}), "
                                  "but it has no output named 'color'. "
                                  "Either add a color output to main or remove the "
                                  "volumetric color reference from the template.",
                                  colorFuncId);
                            }
                        }
                    }
                }
                catch (...)
                {
                    // GetVolumeData may throw if not present; that's OK
                }
            }
            return {};
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

    nlohmann::json LibraryTool::listLibrary(std::string const & category,
                                             std::string const & query) const
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

            // Filter by query if provided (case-insensitive substring match)
            if (!query.empty())
            {
                std::string lowerQuery = query;
                std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                nlohmann::json filtered = nlohmann::json::array();
                for (auto const & e : entries)
                {
                    auto matchField = [&](std::string const & key) -> bool
                    {
                        if (!e.contains(key) || !e[key].is_string())
                            return false;
                        std::string val = e[key].get<std::string>();
                        std::transform(val.begin(), val.end(), val.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        return val.find(lowerQuery) != std::string::npos;
                    };
                    if (matchField("name") || matchField("description") || matchField("tags"))
                    {
                        filtered.push_back(e);
                    }
                }
                entries = std::move(filtered);
            }

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
            std::string tags;
            std::vector<Lib3MF_uint32> taggedIds;
            if (metadata.has_value())
            {
                description = metadata->libraryDescription;
                tags = metadata->libraryTags;
                taggedIds = io::parseResourceIds(metadata->libraryFunctions);
            }

            auto functions = extractFunctionInfo(model, taggedIds);

            // Decode tags into array
            nlohmann::json tagsArray = nlohmann::json::array();
            if (!tags.empty())
            {
                std::istringstream stream(tags);
                std::string tag;
                while (std::getline(stream, tag, ','))
                {
                    // Trim whitespace
                    auto const start = tag.find_first_not_of(" \t");
                    auto const end = tag.find_last_not_of(" \t");
                    if (start != std::string::npos)
                    {
                        tagsArray.push_back(tag.substr(start, end - start + 1));
                    }
                }
            }

            return {{"success", true},
                    {"name", name},
                    {"category", category},
                    {"description", description},
                    {"tags", tagsArray},
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

    nlohmann::json LibraryTool::createLibraryEntry(
        std::string const & name,
        std::string const & category,
        std::string const & programSnippet,
        uint32_t functionId,
        std::string const & description,
        std::vector<std::string> const & tags,
        bool overwrite)
    {
        if (name.empty() || category.empty() || programSnippet.empty())
        {
            return createToolError(
              "name, category, and program_snippet are required",
              {{"name", "helix-spring"},
               {"category", "mechanical"},
               {"function_id", 1},
               {"program_snippet",
                "// Function: helix_spring (ID: 1)\nfloat helix_spring_1(vec3 pos) {...}\n\n"
                "// Function: main (ID: 3) [root]\n(float shape) main_3(vec3 pos) {\n"
                "  shape = helix_spring_1(pos);\n}"},
               {"description", "A parametric helix spring"}});
        }

        auto const targetPath = getUserLibraryDir() / category / (name + ".3mf");

        if (fs::exists(targetPath) && !overwrite)
        {
            return createToolError(
              fmt::format("Library entry '{}' already exists in category '{}'", name, category),
              {{"name", name}, {"category", category}, {"overwrite", true}});
        }

        try
        {
            auto stepStart = std::chrono::steady_clock::now();
            auto logStep = [&stepStart](char const * label) {
                auto now = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - stepStart).count();
                fprintf(stderr, "[createLibraryEntry] %s took %lld ms\n", label, static_cast<long long>(ms));
                fflush(stderr);
                stepStart = now;
            };

            // Step 1: Create a fresh document from template
            DocumentLifecycleTool docTool(m_application);
            if (!docTool.createNewDocument())
            {
                return createToolError("Failed to create document from template");
            }

            auto document = m_application->getCurrentDocument();
            if (!document)
            {
                return createToolError("Failed to access newly created document");
            }
            logStep("Step 1: create document from template");

            // Step 2: Apply the full program snippet (includes main + library function)
            auto assembly = document->getAssembly();
            if (!assembly)
            {
                return createToolError("No assembly available in document");
            }

            // Verify the snippet includes a main function definition.
            // Without main, the template's default main survives and the library
            // function has no demo / thumbnail.
            if (programSnippet.find("// Function: main (ID: 3)") == std::string::npos)
            {
                return createToolError(
                  "program_snippet must include a main function definition "
                  "(\"// Function: main (ID: 3) [root]\") that demonstrates the library "
                  "function with concrete example parameters.",
                  {{"program_snippet_example",
                    "// Function: my_func (ID: 1)\n"
                    "float my_func_1(float a, float b) { ... }\n\n"
                    "// Function: main (ID: 3) [root]\n"
                    "(float shape) main_3(vec3 pos) {\n"
                    "  float sA = length(in_pos) - 8.0;\n"
                    "  float sB = length(in_pos - vec3(10,0,0)) - 6.0;\n"
                    "  shape = my_func_1(sA, sB);\n}"}});
            }

            ExpressionParser parser;
            ExpressionToGraphConverter::setProgramSnippet(programSnippet, *assembly, parser);
            logStep("Step 2: setProgramSnippet");

            // If the template references a volumetric color on a function that
            // doesn't have a "color" output (because the snippet only declared
            // "shape"), add a default constant color so the reference stays valid.
            // Note: We iterate assembly models (not 3MF resources) because
            // setProgramSnippet invalidates the 3MF resource pointers.
            {
                // The template uses functionid=3 (main) for its color reference.
                // Check the main function's End node for a "color" output.
                static constexpr nodes::ResourceId MAIN_FUNCTION_ID = 3;
                auto funcModel = assembly->findModel(MAIN_FUNCTION_ID);
                if (funcModel)
                {
                    auto * endNode = funcModel->getEndNode();
                    if (endNode != nullptr
                        && endNode->getParameter(nodes::FieldNames::Color) == nullptr)
                    {
                        // Add a constant default color and wire it to the end node.
                        auto * constVec = funcModel->create<nodes::ConstantVector>();
                        constVec->parameter().at(nodes::FieldNames::X) =
                            nodes::VariantParameter(0.5f);
                        constVec->parameter().at(nodes::FieldNames::Y) =
                            nodes::VariantParameter(0.5f);
                        constVec->parameter().at(nodes::FieldNames::Z) =
                            nodes::VariantParameter(0.5f);
                        funcModel->addFunctionOutput(
                            nodes::FieldNames::Color,
                            nodes::VariantParameter(nodes::float3{0.5f, 0.5f, 0.5f}));
                        funcModel->addLink(
                            constVec->getOutputs().at(nodes::FieldNames::Vector).getId(),
                            endNode->parameter().at(nodes::FieldNames::Color).getId(),
                            /*skipCheck=*/true);
                        funcModel->updateGraphAndOrderIfNeeded();
                    }
                }
            }

            // Sync changes to 3MF model
            logStep("Step 2a: default color fixup");
            document->update3mfModel();
            document->rebuildResourceDependencyGraph();
            logStep("Step 2a: update3mfModel + rebuildDependencyGraph");

            // Step 2b: Force a fresh compilation and check for errors
            document->refreshModelBlocking();
            logStep("Step 2b: refreshModelBlocking (GPU compile)");

            auto const core = document->getCore();
            if (!core)
            {
                return createToolError("Failed to access compute core");
            }

            auto const slicerProgram = core->getSlicerProgram();
            if (!slicerProgram || !slicerProgram->isValid())
            {
                return createToolError(
                  "Compilation failed: the slicer program is invalid. "
                  "Check the program_snippet for syntax errors or unsupported constructs.");
            }

            if (!slicerProgram->compilationSucceeded())
            {
                return createToolError(
                  "Compilation failed. The program_snippet contains errors. "
                  "Check for unsupported functions, missing parameters, or syntax issues.");
            }
            logStep("Step 2b: compilation check");

            // Step 3: Verify the tagged function exists
            auto sourceModel = document->get3mfModel();
            if (!sourceModel)
            {
                return createToolError("Failed to access 3MF model");
            }

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
                  fmt::format("Function with resource ID {} not found in document. "
                              "Check your program_snippet function IDs.", functionId),
                  {{"function_id", 1},
                   {"category", category},
                   {"name", name}},
                  {{"available_functions", availableIds}});
            }

            // Step 4: Validate bounding box (must be meaningful and smaller than eval domain)
            try
            {
                auto bbox = document->computeBoundingBox();
                bool const isDegenerate =
                  std::isnan(bbox.min.x) || std::isnan(bbox.min.y) || std::isnan(bbox.min.z)
                  || std::isnan(bbox.max.x) || std::isnan(bbox.max.y) || std::isnan(bbox.max.z)
                  || (bbox.max.x - bbox.min.x <= 0.f)
                  || (bbox.max.y - bbox.min.y <= 0.f)
                  || (bbox.max.z - bbox.min.z <= 0.f);
                if (isDegenerate)
                {
                    return {{"success", false},
                            {"reason", "invalid_bounding_box"},
                            {"message",
                             "The main function produces a degenerate or zero-volume bounding box. "
                             "Verify that main calls the library function with valid demo parameters "
                             "and produces a visible surface."}};
                }

                // The bbox must be strictly smaller than the evaluation domain (build volume).
                // If it matches the build volume, the function likely has no real surface and
                // the bbox is just the fallback eval domain.
                auto const & buildVolume =
                  core->getResourceContext()->getBuildVolume();
                float constexpr MARGIN = 1.f; // mm
                bool const matchesBuildVolume =
                  (bbox.min.x <= buildVolume.min.x + MARGIN)
                  && (bbox.min.y <= buildVolume.min.y + MARGIN)
                  && (bbox.min.z <= buildVolume.min.z + MARGIN)
                  && (bbox.max.x >= buildVolume.max.x - MARGIN)
                  && (bbox.max.y >= buildVolume.max.y - MARGIN)
                  && (bbox.max.z >= buildVolume.max.z - MARGIN);
                if (matchesBuildVolume)
                {
                    return {{"success", false},
                            {"reason", "bounding_box_matches_eval_domain"},
                            {"message",
                             "The bounding box matches the evaluation domain, which means the "
                             "function likely has no real surface or the SDF is always negative. "
                             "Verify that the main function produces a visible, bounded shape."}};
                }
            }
            catch (std::exception const & e)
            {
                return {{"success", false},
                        {"reason", "invalid_bounding_box"},
                        {"message",
                         fmt::format("Failed to compute bounding box: {}", e.what())}};
            }
            logStep("Step 4: bounding box validation");

            // Step 5: Render thumbnail
            std::vector<unsigned char> const thumbnailPng = renderThumbnailPng(document);
            if (thumbnailPng.empty())
            {
                return {{"success", false},
                        {"reason", "thumbnail_render_failed"},
                        {"message",
                         "Failed to render thumbnail. The main function may not produce "
                         "a visible surface, or the GPU pipeline is unavailable."}};
            }
            logStep("Step 5: render thumbnail");

            // Step 6: Write model to library with scaffold and metadata
            fs::create_directories(targetPath.parent_path());
            {
                auto writer = sourceModel->QueryWriter("3mf");
                writer->WriteToFile(targetPath.string());
            }

            // Stamp library metadata and thumbnail on the standalone copy
            {
                auto exportedModel = openStandaloneModel(targetPath);

                io::LibraryMetadata metadata;
                metadata.libraryFunctions =
                  io::serializeResourceIds({static_cast<Lib3MF_uint32>(functionId)});
                metadata.libraryDescription = description;
                if (!tags.empty())
                {
                    std::string tagStr;
                    for (size_t i = 0; i < tags.size(); ++i)
                    {
                        if (i > 0)
                        {
                            tagStr += ",";
                        }
                        tagStr += tags[i];
                    }
                    metadata.libraryTags = tagStr;
                }
                io::writeLibraryMetadata(exportedModel, metadata);

                if (!thumbnailPng.empty())
                {
                    if (exportedModel->HasPackageThumbnailAttachment())
                    {
                        exportedModel->RemovePackageThumbnailAttachment();
                    }
                    auto thumb = exportedModel->CreatePackageThumbnailAttachment();
                    thumb->ReadFromBuffer(thumbnailPng);
                }

                auto writer = exportedModel->QueryWriter("3mf");
                writer->WriteToFile(targetPath.string());

                // Step 7: Validate the written file's 3MF references.
                // Re-read the final file and check that all levelset channel and
                // volumetric color references resolve to existing function outputs.
                auto finalModel = openStandaloneModel(targetPath);
                auto refError = validate3mfReferences(finalModel);
                if (!refError.empty())
                {
                    fs::remove(targetPath);
                    return createToolError(
                      fmt::format("3MF reference validation failed: {}", refError));
                }
            }

            auto msg = fmt::format(
              "Created library entry '{}' in category '{}' (with scaffold and thumbnail)",
              name, category);
            if (overwrite)
            {
                msg += " (overwritten)";
            }

            return {{"success", true},
                    {"path", targetPath.string()},
                    {"name", name},
                    {"category", category},
                    {"function_id", functionId},
                    {"message", msg}};
        }
        catch (std::exception const & e)
        {
            std::error_code ec;
            fs::remove(targetPath, ec);
            return createToolError(
              fmt::format("Failed to create library entry: {}", e.what()));
        }
    }

    std::vector<unsigned char> LibraryTool::renderThumbnailPng(std::shared_ptr<Document> document) const
    {
        auto logger = document->getSharedLogger();
        auto computeCore = document->getCore();
        if (!computeCore)
        {
            logger->addEvent(
              {"renderThumbnailPng: no compute core available", events::Severity::Warning});
            return {};
        }

        try
        {
            logger->addEvent(
              {"renderThumbnailPng: starting GPU prep", events::Severity::Info});

            document->updateParameterRegistration();
            document->updateParameter();
            document->updateFlatAssembly();
            computeCore->tryRefreshProgramProtected(document->getFlatAssembly());

            if (!computeCore->prepareImageRendering())
            {
                logger->addEvent(
                  {"renderThumbnailPng: prepareImageRendering returned false",
                   events::Severity::Warning});
                return {};
            }

            auto image = computeCore->createThumbnailPng();
            logger->addEvent(
              {fmt::format("renderThumbnailPng: {} bytes", image.data.size()),
               events::Severity::Info});
            return std::move(image.data);
        }
        catch (std::exception const & e)
        {
            logger->addEvent(
              {fmt::format("renderThumbnailPng: failed: {}", e.what()),
               events::Severity::Warning});
            return {};
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

            // FR-014b: Validate thumbnail and bounding box before writing
            auto logger = document->getSharedLogger();

            // Check bounding box validity
            try
            {
                auto bbox = document->computeBoundingBox();
                bool const isDegenerate =
                  std::isnan(bbox.min.x) || std::isnan(bbox.min.y) || std::isnan(bbox.min.z)
                  || std::isnan(bbox.max.x) || std::isnan(bbox.max.y) || std::isnan(bbox.max.z)
                  || (bbox.max.x - bbox.min.x <= 0.f)
                  || (bbox.max.y - bbox.min.y <= 0.f)
                  || (bbox.max.z - bbox.min.z <= 0.f);
                if (isDegenerate)
                {
                    return {{"success", false},
                            {"reason", "invalid_bounding_box"},
                            {"message",
                             "Function produces a degenerate or zero-volume bounding box. "
                             "The SDF may not define a valid surface."}};
                }
            }
            catch (std::exception const & e)
            {
                return {{"success", false},
                        {"reason", "invalid_bounding_box"},
                        {"message",
                         fmt::format("Failed to compute bounding box: {}", e.what())}};
            }

            // Render thumbnail
            std::vector<unsigned char> const thumbnailPng = renderThumbnailPng(document);
            if (thumbnailPng.empty())
            {
                return {{"success", false},
                        {"reason", "thumbnail_render_failed"},
                        {"message",
                         "Failed to render thumbnail for the library entry. "
                         "The function may not produce a visible surface."}};
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

            // Write the live model to the target file first, without touching the
            // in-memory model's metadata. This avoids a lib3mf issue where stamping
            // metadata on the live model and then removing it leaves residual state
            // that causes "Duplicate Model Metadata" on a subsequent export.
            fs::create_directories(targetPath.parent_path());
            {
                auto writer = sourceModel->QueryWriter("3mf");
                writer->WriteToFile(targetPath.string());
            }

            // Now stamp library metadata on the standalone copy, along with the thumbnail.
            {
                auto exportedModel = openStandaloneModel(targetPath);

                io::LibraryMetadata metadata;
                metadata.libraryFunctions =
                  io::serializeResourceIds({static_cast<Lib3MF_uint32>(functionId)});
                metadata.libraryDescription = description;
                io::writeLibraryMetadata(exportedModel, metadata);

                if (!thumbnailPng.empty())
                {
                    if (exportedModel->HasPackageThumbnailAttachment())
                    {
                        exportedModel->RemovePackageThumbnailAttachment();
                    }
                    auto thumb = exportedModel->CreatePackageThumbnailAttachment();
                    thumb->ReadFromBuffer(thumbnailPng);
                }

                auto writer = exportedModel->QueryWriter("3mf");
                writer->WriteToFile(targetPath.string());
            }

            size_t prunedCount = 0;
            if (!keepScaffold)
            {
                // Prune the exported file to only include the tagged function,
                // its transitive dependencies, and any example build item chain.
                prunedCount = pruneExportedLibraryFile(
                  targetPath, static_cast<Lib3MF_uint32>(functionId),
                  thumbnailPng, logger);
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
            auto const binCategoryDir = getBinDir() / category;
            fs::create_directories(binCategoryDir);

            auto const destPath =
              disambiguateFilename(binCategoryDir, name, ".3mf");
            fs::rename(entryPath, destPath);

            return {{"success", true},
                    {"name", name},
                    {"category", category},
                    {"bin_path", destPath.string()},
                    {"message",
                     fmt::format(
                       "Moved library entry '{}' from category '{}' to bin. "
                       "Use browse_bin to see binned items or restore_bin_entry to recover.",
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
                                    std::string const & description,
                                    std::vector<std::string> const & tags,
                                    std::string const & category,
                                    std::string const & name)
    {
        // Encode tags as comma-separated string
        std::string tagStr;
        for (size_t i = 0; i < tags.size(); ++i)
        {
            if (i > 0) tagStr += ',';
            tagStr += tags[i];
        }

        // If category and name are provided, update the library entry file directly
        if (!category.empty() && !name.empty())
        {
            bool isShipped = false;
            auto const entryPath = resolveEntryPath(category, name, isShipped);
            if (entryPath.empty())
            {
                return createToolError(
                  fmt::format("Library entry '{}' not found in category '{}'", name, category));
            }
            if (isShipped)
            {
                return createToolError(
                  fmt::format("Cannot modify shipped library entry '{}/{}'", category, name));
            }

            try
            {
                auto model = openStandaloneModel(entryPath);

                std::vector<Lib3MF_uint32> lib3mfIds(functionIds.begin(), functionIds.end());
                io::LibraryMetadata metadata;
                metadata.libraryFunctions = io::serializeResourceIds(lib3mfIds);
                metadata.libraryDescription = description;
                metadata.libraryTags = tagStr;
                io::writeLibraryMetadata(model, metadata);

                auto writer = model->QueryWriter("3mf");
                writer->WriteToFile(entryPath.string());

                return {{"success", true},
                        {"category", category},
                        {"name", name},
                        {"function_ids", functionIds},
                        {"description", description},
                        {"tags", tags},
                        {"message",
                         fmt::format("Updated metadata on library entry '{}/{}': {} tagged "
                                     "function(s), description='{}'",
                                     category,
                                     name,
                                     functionIds.size(),
                                     description)}};
            }
            catch (std::exception const & e)
            {
                return createToolError(
                  fmt::format("Failed to update library entry metadata: {}", e.what()));
            }
        }

        // Otherwise, update the currently active document
        if (!validateActiveDocument())
        {
            return createToolError(
              "No active document. Open or create a document first, or specify "
              "category and name to target a library entry.",
              {{"function_ids", nlohmann::json::array({5})},
               {"description", "My library entry"},
               {"category", "primitives"},
               {"name", "my_entry"}});
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
        metadata.libraryTags = tagStr;
        io::writeLibraryMetadata(model, metadata);

        return {{"success", true},
                {"function_ids", functionIds},
                {"description", description},
                {"tags", tags},
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
            isShipped = isShippedEntry(category, name);
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
        auto const addCategories = [&categories, &seen, this](fs::path const & root)
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
                    if (!name.empty() && name[0] == '.')
                    {
                        continue;
                    }
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
        auto const addEntries = [&entries, &seen, &category, this](fs::path const & root)
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
                    if (!name.empty() && name[0] == '.')
                    {
                        continue;
                    }
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

    nlohmann::json LibraryTool::browseBin(std::string const & category) const
    {
        auto const binDir = getBinDir();
        if (!fs::exists(binDir))
        {
            return {{"success", true},
                    {"categories", nlohmann::json::array()},
                    {"total_entries", 0},
                    {"message", "Bin is empty."}};
        }

        nlohmann::json categories = nlohmann::json::array();
        std::size_t totalEntries = 0;

        for (auto const & catEntry : fs::directory_iterator(binDir))
        {
            if (!catEntry.is_directory())
            {
                continue;
            }
            auto const catName = catEntry.path().filename().string();
            if (!category.empty() && catName != category)
            {
                continue;
            }

            nlohmann::json entries = nlohmann::json::array();
            for (auto const & fileEntry : fs::directory_iterator(catEntry.path()))
            {
                if (fileEntry.is_regular_file() && fileEntry.path().extension() == ".3mf")
                {
                    entries.push_back({{"name", fileEntry.path().stem().string()}});
                }
            }

            if (!entries.empty())
            {
                categories.push_back({{"name", catName}, {"entries", entries}});
                totalEntries += entries.size();
            }
        }

        return {{"success", true},
                {"categories", categories},
                {"total_entries", totalEntries},
                {"message",
                 totalEntries == 0 ? "Bin is empty."
                                   : fmt::format("Bin contains {} entry/entries.", totalEntries)}};
    }

    nlohmann::json LibraryTool::restoreBinEntry(std::string const & category,
                                                 std::string const & name)
    {
        auto const binPath = getBinDir() / category / (name + ".3mf");
        if (!fs::exists(binPath))
        {
            return createToolError(
              fmt::format("Entry '{}' not found in bin category '{}'", name, category));
        }

        try
        {
            auto const userCatDir = getUserLibraryDir() / category;
            fs::create_directories(userCatDir);

            auto const destPath = disambiguateFilename(userCatDir, name, ".3mf");
            fs::rename(binPath, destPath);

            // Clean up empty bin category dir
            if (fs::is_empty(getBinDir() / category))
            {
                fs::remove(getBinDir() / category);
            }

            auto const restoredName = destPath.stem().string();
            return {{"success", true},
                    {"name", restoredName},
                    {"category", category},
                    {"path", destPath.string()},
                    {"message",
                     fmt::format("Restored '{}' to category '{}'.", restoredName, category)}};
        }
        catch (std::exception const & e)
        {
            return createToolError(
              fmt::format("Failed to restore bin entry: {}", e.what()));
        }
    }

    nlohmann::json LibraryTool::deleteBinEntry(std::string const & category,
                                                std::string const & name)
    {
        auto const binPath = getBinDir() / category / (name + ".3mf");
        if (!fs::exists(binPath))
        {
            return createToolError(
              fmt::format("Entry '{}' not found in bin category '{}'", name, category));
        }

        try
        {
            fs::remove(binPath);

            // Clean up empty bin category dir
            if (fs::is_empty(getBinDir() / category))
            {
                fs::remove(getBinDir() / category);
            }

            return {{"success", true},
                    {"name", name},
                    {"category", category},
                    {"message",
                     fmt::format(
                       "Permanently deleted '{}' from bin category '{}'.", name, category)}};
        }
        catch (std::exception const & e)
        {
            return createToolError(
              fmt::format("Failed to delete bin entry: {}", e.what()));
        }
    }

    nlohmann::json LibraryTool::emptyBin()
    {
        auto const binDir = getBinDir();
        if (!fs::exists(binDir))
        {
            return {{"success", true},
                    {"removed_count", 0},
                    {"message", "Bin was already empty."}};
        }

        try
        {
            std::size_t removedCount = 0;
            for (auto const & catEntry : fs::directory_iterator(binDir))
            {
                if (!catEntry.is_directory())
                {
                    continue;
                }
                for (auto const & fileEntry : fs::directory_iterator(catEntry.path()))
                {
                    if (fileEntry.is_regular_file())
                    {
                        fs::remove(fileEntry.path());
                        ++removedCount;
                    }
                }
                fs::remove(catEntry.path());
            }

            return {{"success", true},
                    {"removed_count", removedCount},
                    {"message",
                     fmt::format("Permanently deleted {} entry/entries from bin.", removedCount)}};
        }
        catch (std::exception const & e)
        {
            return createToolError(fmt::format("Failed to empty bin: {}", e.what()));
        }
    }

} // namespace gladius::mcp::tools
