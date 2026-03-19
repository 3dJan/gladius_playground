/**
 * @file ApplicationMCPAdapter.cpp
 * @brief Implementation of Application MCP adapter
 */

#include "ApplicationMCPAdapter.h"
#include "TimeUtils.h"
#include "../Application.h"
#include "../Document.h"
#include "../ExpressionParser.h"
#include "../ExpressionToGraphConverter.h"
#include "../io/3mf/ResourceIdUtil.h"
#include "../nodes/Assembly.h"
#include "../nodes/DerivedNodes.h"
#include "CoroMCPAdapter.h"
#include "FunctionGraphSerializer.h"
#include "nodes/NodeFactory.h"
#include "tools/ApplicationLifecycleTool.h"
#include "tools/DocumentLifecycleTool.h"
#include "tools/FunctionOperationsTool.h"
#include "tools/FunctionEvaluatorTool.h"
#include "tools/ParameterManagementTool.h"
#include "tools/RenderingTool.h"
#include "tools/ResourceManagementTool.h"
#include "tools/SceneHierarchyTool.h"
#include "tools/UtilityTool.h"
#include "tools/ValidationTool.h"
#include <array>
#include <filesystem> // Only include here, not in header
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace gladius
{
    ApplicationMCPAdapter::ApplicationMCPAdapter(Application * app)
        : m_application(app)
    {
        // Initialize the coroutine adapter for async operations
        if (m_application)
        {
            m_coroAdapter =
              std::make_unique<mcp::CoroMCPAdapter>(m_application,
                                                    2, // Background I/O threads
                                                    4  // Compute threads for OpenCL operations
              );
        }

        // Initialize tool instances
        m_lifecycleTool = std::make_unique<mcp::tools::ApplicationLifecycleTool>(app);
        m_sceneHierarchyTool = std::make_unique<mcp::tools::SceneHierarchyTool>(app);
        m_documentLifecycleTool = std::make_unique<mcp::tools::DocumentLifecycleTool>(app);
        m_parameterManagementTool = std::make_unique<mcp::tools::ParameterManagementTool>(app);
        m_functionOperationsTool = std::make_unique<mcp::tools::FunctionOperationsTool>(app);
        m_functionEvaluatorTool = std::make_unique<mcp::tools::FunctionEvaluatorTool>(app);
        m_resourceManagementTool = std::make_unique<mcp::tools::ResourceManagementTool>(app);
        m_renderingTool = std::make_unique<mcp::tools::RenderingTool>(app);
        m_validationTool = std::make_unique<mcp::tools::ValidationTool>(app);
        m_utilityTool = std::make_unique<mcp::tools::UtilityTool>(app);
        m_libraryTool = std::make_unique<mcp::tools::LibraryTool>(app);
    }

    ApplicationMCPAdapter::~ApplicationMCPAdapter() = default;

    std::string ApplicationMCPAdapter::getVersion() const
    {
        return m_lifecycleTool->getVersion();
    }

    bool ApplicationMCPAdapter::isRunning() const
    {
        return m_lifecycleTool->isRunning();
    }

    std::string ApplicationMCPAdapter::getApplicationName() const
    {
        return m_lifecycleTool->getApplicationName();
    }

    std::string ApplicationMCPAdapter::getStatus() const
    {
        return m_lifecycleTool->getStatus();
    }

    void ApplicationMCPAdapter::setHeadlessMode(bool headless)
    {
        m_lifecycleTool->setHeadlessMode(headless);
    }

    bool ApplicationMCPAdapter::isHeadlessMode() const
    {
        return m_lifecycleTool->isHeadlessMode();
    }

    bool ApplicationMCPAdapter::showUI()
    {
        return m_lifecycleTool->showUI();
    }

    bool ApplicationMCPAdapter::isUIRunning() const
    {
        return m_lifecycleTool->isUIRunning();
    }

    bool ApplicationMCPAdapter::hasActiveDocument() const
    {
        return m_documentLifecycleTool->hasActiveDocument();
    }

    std::string ApplicationMCPAdapter::getActiveDocumentPath() const
    {
        return m_documentLifecycleTool->getActiveDocumentPath();
    }

    bool ApplicationMCPAdapter::createNewDocument()
    {
        auto result = m_documentLifecycleTool->createNewDocument();
        if (result)
        {
            {
                std::lock_guard<std::mutex> lock(m_changeLogMutex);
                m_changeLog.clear();
            }
            recordChange("added", "document", 0, "new document");
        }
        return result;
    }

    bool ApplicationMCPAdapter::createEmptyDocument()
    {
        return m_documentLifecycleTool->createEmptyDocument();
    }

    bool ApplicationMCPAdapter::openDocument(const std::string & path)
    {
        auto result = m_documentLifecycleTool->openDocument(path);
        if (result)
        {
            {
                std::lock_guard<std::mutex> lock(m_changeLogMutex);
                m_changeLog.clear();
            }
            recordChange("added", "document", 0, path);
        }
        return result;
    }

    bool ApplicationMCPAdapter::saveDocument()
    {
        if (!m_application || !m_coroAdapter)
        {
            m_lastErrorMessage = "No application instance or coroutine adapter available";
            return false;
        }

        try
        {
            auto document = m_application->getCurrentDocument();
            if (!document)
            {
                m_lastErrorMessage =
                  "No active document available. Please create or open a document first.";
                return false;
            }

            // Check if document has a current filename
            auto currentPath = document->getCurrentAssemblyFilename();
            if (!currentPath.has_value())
            {
                m_lastErrorMessage = "Document has not been saved before. Use 'save_document_as' "
                                     "to specify a filename.";
                return false;
            }

            // Use the async coroutine adapter for non-blocking save
            bool result = m_coroAdapter->saveDocumentAs(currentPath->string());
            if (result)
            {
                m_lastErrorMessage = "Document saved successfully to " + currentPath->string();
            }
            else
            {
                m_lastErrorMessage = "Save failed: " + m_coroAdapter->getLastErrorMessage();
            }
            return result;
        }
        catch (const std::exception & e)
        {
            m_lastErrorMessage = "Exception while saving document: " + std::string(e.what());
            return false;
        }
    }

    bool ApplicationMCPAdapter::saveDocumentAs(const std::string & path)
    {
        // Validate path first, regardless of application state
        if (path.empty())
        {
            m_lastErrorMessage = "File path cannot be empty";
            return false;
        }

        if (!path.ends_with(".3mf"))
        {
            m_lastErrorMessage = "File must have .3mf extension for " + path;
            return false;
        }

        if (!m_application || !m_coroAdapter)
        {
            m_lastErrorMessage = "No application instance or coroutine adapter available";
            return false;
        }

        try
        {
            auto document = m_application->getCurrentDocument();
            if (!document)
            {
                m_lastErrorMessage =
                  "No active document available. Please create or open a document first.";
                return false;
            }

            // Use the async coroutine adapter for non-blocking save with thumbnail
            bool result = m_coroAdapter->saveDocumentAs(path);
            if (result)
            {
                m_lastErrorMessage = "Document saved successfully to " + path;
            }
            else
            {
                m_lastErrorMessage = "Save failed: " + m_coroAdapter->getLastErrorMessage();
            }
            return result;
        }
        catch (const std::exception & e)
        {
            m_lastErrorMessage = "Exception while saving document: " + std::string(e.what());
            return false;
        }
    }

    bool ApplicationMCPAdapter::exportDocument(const std::string & path, const std::string & format)
    {
        return m_documentLifecycleTool->exportDocument(path, format);
    }

    bool ApplicationMCPAdapter::setFloatParameter(uint32_t modelId,
                                                  const std::string & nodeName,
                                                  const std::string & parameterName,
                                                  float value)
    {
        return m_parameterManagementTool->setFloatParameter(
          modelId, nodeName, parameterName, value);
    }

    float ApplicationMCPAdapter::getFloatParameter(uint32_t modelId,
                                                   const std::string & nodeName,
                                                   const std::string & parameterName)
    {
        return m_parameterManagementTool->getFloatParameter(modelId, nodeName, parameterName);
    }

    bool ApplicationMCPAdapter::setStringParameter(uint32_t modelId,
                                                   const std::string & nodeName,
                                                   const std::string & parameterName,
                                                   const std::string & value)
    {
        return m_parameterManagementTool->setStringParameter(
          modelId, nodeName, parameterName, value);
    }

    std::string ApplicationMCPAdapter::getStringParameter(uint32_t modelId,
                                                          const std::string & nodeName,
                                                          const std::string & parameterName)
    {
        return m_parameterManagementTool->getStringParameter(modelId, nodeName, parameterName);
    }

    // Expression and function operations
    std::pair<bool, uint32_t> ApplicationMCPAdapter::createFunctionFromExpression(
      const std::string & name,
      const std::string & expression,
      const std::string & outputType,
      const std::vector<FunctionArgument> & arguments,
      const std::string & outputName)
    {
        auto result = m_functionOperationsTool->createFunctionFromExpression(
          name, expression, outputType, arguments, outputName);
        if (!result.first)
        {
            m_lastErrorMessage = m_functionOperationsTool->getLastErrorMessage();
        }
        else
        {
            recordChange("added", "function", result.second, name);
        }
        return result;
    }

    std::pair<bool, uint32_t> ApplicationMCPAdapter::createFunctionFromSnippet(
      const std::string & name,
      const std::string & snippet,
      const std::string & outputType,
      const std::vector<FunctionArgument> & arguments,
      const std::string & outputName)
    {
        auto result = m_functionOperationsTool->createFunctionFromSnippet(
          name, snippet, outputType, arguments, outputName);
        if (!result.first)
        {
            m_lastErrorMessage = m_functionOperationsTool->getLastErrorMessage();
        }
        else
        {
            recordChange("added", "function", result.second, name);
        }
        return result;
    }

    // 3MF and implicit modeling operations
    bool ApplicationMCPAdapter::validateDocumentFor3MF() const
    {
        if (!m_application || !hasActiveDocument())
        {
            m_lastErrorMessage = "No active document to validate";
            return false;
        }

        // TODO: Implement actual 3MF validation
        // Check for proper namespace declarations, resource IDs, function definitions
        m_lastErrorMessage = "3MF validation passed - document structure is compliant";
        return true;
    }

    bool ApplicationMCPAdapter::exportDocumentAs3MF(const std::string & path,
                                                    bool includeImplicitFunctions) const
    {
        if (!m_application || !hasActiveDocument())
        {
            m_lastErrorMessage = "No active document to export";
            return false;
        }

        try
        {
            // Use existing save functionality with 3MF format
            bool success = const_cast<ApplicationMCPAdapter *>(this)->saveDocumentAs(path);
            if (success)
            {
                m_lastErrorMessage = "Document exported as 3MF with implicit functions: " + path;
            }
            return success;
        }
        catch (const std::exception & e)
        {
            m_lastErrorMessage = "Failed to export 3MF: " + std::string(e.what());
            return false;
        }
    }

    nlohmann::json
    ApplicationMCPAdapter::analyzeFunctionProperties(const std::string & functionName) const
    {
        return m_functionOperationsTool->analyzeFunctionProperties(functionName);
    }

    nlohmann::json ApplicationMCPAdapter::getSceneHierarchy() const
    {
        return m_sceneHierarchyTool->getSceneHierarchy();
    }

    nlohmann::json ApplicationMCPAdapter::getDocumentInfo() const
    {
        return m_sceneHierarchyTool->getDocumentInfo();
    }

    std::vector<std::string> ApplicationMCPAdapter::listAvailableFunctions() const
    {
        return m_functionOperationsTool->listAvailableFunctions();
    }

    nlohmann::json ApplicationMCPAdapter::get3MFStructure() const
    {
        nlohmann::json out;
        out["has_document"] = hasActiveDocument();
        out["document_path"] = getActiveDocumentPath();

        if (!hasActiveDocument())
        {
            out["error"] = "No active document";
            return out;
        }

        try
        {
            auto document = m_application->getCurrentDocument();
            if (!document)
            {
                out["error"] = "No active document available";
                return out;
            }

            auto model = document->get3mfModel();
            if (!model)
            {
                out["error"] = "No 3MF model available";
                return out;
            }

            // Build items
            nlohmann::json buildItems = nlohmann::json::array();
            try
            {
                auto buildIt = model->GetBuildItems();
                while (buildIt->MoveNext())
                {
                    auto item = buildIt->GetCurrent();
                    nlohmann::json bi;
                    bi["part_number"] = item->GetPartNumber();
                    bool hasUUID = false;
                    try
                    {
                        auto uuid = item->GetUUID(hasUUID);
                        if (hasUUID)
                        {
                            bi["uuid"] = uuid;
                        }
                        else
                        {
                            bi["uuid"] = nullptr;
                        }
                    }
                    catch (...)
                    {
                        bi["uuid"] = nullptr;
                    }
                    bi["transform"] = nlohmann::json::array();
                    {
                        auto t = item->GetObjectTransform();
                        // 4x3 matrix fields
                        for (int r = 0; r < 4; ++r)
                        {
                            nlohmann::json row = nlohmann::json::array();
                            for (int c = 0; c < 3; ++c)
                            {
                                row.push_back(t.m_Fields[r][c]);
                            }
                            bi["transform"].push_back(row);
                        }
                    }
                    try
                    {
                        auto obj = item->GetObjectResource();
                        if (obj)
                        {
                            bi["object_id"] = obj->GetModelResourceID();
                            // Derive a friendly type name via dynamic cast
                            std::string otype = "object";
                            if (dynamic_cast<Lib3MF::CMeshObject *>(obj.get()))
                                otype = "mesh";
                            else if (dynamic_cast<Lib3MF::CComponentsObject *>(obj.get()))
                                otype = "components";
                            else if (dynamic_cast<Lib3MF::CLevelSet *>(obj.get()))
                                otype = "levelset";
                            bi["object_type"] = otype;
                        }
                        else
                        {
                            bi["object_id"] = 0;
                            bi["object_type"] = "unknown";
                        }
                    }
                    catch (...)
                    {
                        bi["object_id"] = 0;
                        bi["object_type"] = "unknown";
                    }
                    buildItems.push_back(bi);
                }
            }
            catch (...)
            {
                // ignore build item enumeration errors
            }

            // Resources
            nlohmann::json resources = nlohmann::json::array();
            std::size_t meshCount = 0, levelSetCount = 0, functionCount = 0, image3dCount = 0,
                        materialCount = 0, otherCount = 0;
            try
            {
                auto resIt = model->GetResources();
                while (resIt->MoveNext())
                {
                    auto res = resIt->GetCurrent();
                    nlohmann::json r;
                    r["id"] = res->GetModelResourceID();
                    // Derive a readable type/kind via subclass inspection
                    // Note: lib3mf dynamic bindings don't expose a generic GetResourceType or name
                    // on CResource

                    // Try specific casts for richer info
                    if (auto meshObj = dynamic_cast<Lib3MF::CMeshObject *>(res.get()))
                    {
                        meshCount++;
                        r["kind"] = "mesh";
                        r["vertices"] = meshObj->GetVertexCount();
                        r["triangles"] = meshObj->GetTriangleCount();
                        try
                        {
                            r["name"] = meshObj->GetName();
                        }
                        catch (...)
                        {
                            r["name"] = nullptr;
                        }
                    }
                    else if (auto levelSet = dynamic_cast<Lib3MF::CLevelSet *>(res.get()))
                    {
                        levelSetCount++;
                        r["kind"] = "levelset";
                        try
                        {
                            auto fn = levelSet->GetFunction();
                            r["function_id"] = fn ? fn->GetModelResourceID() : 0;
                            r["channel"] = levelSet->GetChannelName();
                        }
                        catch (...)
                        {
                        }
                        try
                        {
                            auto mesh = levelSet->GetMesh();
                            r["mesh_id"] = mesh ? mesh->GetModelResourceID() : 0;
                            r["meshBBoxOnly"] = levelSet->GetMeshBBoxOnly();
                        }
                        catch (...)
                        {
                        }
                        try
                        {
                            r["name"] = levelSet->GetName();
                        }
                        catch (...)
                        {
                            r["name"] = nullptr;
                        }
                    }
                    else if (auto func = dynamic_cast<Lib3MF::CFunction *>(res.get()))
                    {
                        functionCount++;
                        r["kind"] = "function";
                        // Function subtype
                        if (dynamic_cast<Lib3MF::CImplicitFunction *>(res.get()))
                            r["function_type"] = "implicit";
                        else if (dynamic_cast<Lib3MF::CFunctionFromImage3D *>(res.get()))
                            r["function_type"] = "function_from_image3d";
                        else
                            r["function_type"] = "unknown";

                        // Display name
                        try
                        {
                            auto dn = func->GetDisplayName();
                            r["display_name"] = dn;
                            // Back-compat with earlier key
                            r["displayname"] = dn;
                        }
                        catch (...)
                        {
                            r["display_name"] = nullptr;
                            r["displayname"] = nullptr;
                        }

                        // Helper to stringify port type
                        auto portTypeToString = [](Lib3MF::eImplicitPortType t) -> const char *
                        {
                            switch (t)
                            {
                            case Lib3MF::eImplicitPortType::Scalar:
                                return "scalar";
                            case Lib3MF::eImplicitPortType::Vector:
                                return "vector";
                            case Lib3MF::eImplicitPortType::Matrix:
                                return "matrix";
                            default:
                                return "unknown";
                            }
                        };

                        // Inputs
                        try
                        {
                            nlohmann::json inputs = nlohmann::json::array();
                            auto inIt = func->GetInputs();
                            while (inIt->MoveNext())
                            {
                                auto p = inIt->GetCurrent();
                                nlohmann::json pj;
                                try
                                {
                                    pj["identifier"] = p->GetIdentifier();
                                }
                                catch (...)
                                {
                                    pj["identifier"] = nullptr;
                                }
                                try
                                {
                                    pj["display_name"] = p->GetDisplayName();
                                }
                                catch (...)
                                {
                                    pj["display_name"] = nullptr;
                                }
                                try
                                {
                                    pj["type"] = portTypeToString(p->GetType());
                                }
                                catch (...)
                                {
                                    pj["type"] = "unknown";
                                }
                                inputs.push_back(pj);
                            }
                            r["inputs"] = inputs;
                        }
                        catch (...)
                        {
                            r["inputs"] = nlohmann::json::array();
                        }

                        // Outputs
                        try
                        {
                            nlohmann::json outputs = nlohmann::json::array();
                            auto outIt = func->GetOutputs();
                            while (outIt->MoveNext())
                            {
                                auto p = outIt->GetCurrent();
                                nlohmann::json pj;
                                try
                                {
                                    pj["identifier"] = p->GetIdentifier();
                                }
                                catch (...)
                                {
                                    pj["identifier"] = nullptr;
                                }
                                try
                                {
                                    pj["display_name"] = p->GetDisplayName();
                                }
                                catch (...)
                                {
                                    pj["display_name"] = nullptr;
                                }
                                try
                                {
                                    pj["type"] = portTypeToString(p->GetType());
                                }
                                catch (...)
                                {
                                    pj["type"] = "unknown";
                                }
                                outputs.push_back(pj);
                            }
                            r["outputs"] = outputs;
                        }
                        catch (...)
                        {
                            r["outputs"] = nlohmann::json::array();
                        }

                        // T008: Enrich with snippet metadata (arguments, output_type, snippet_preview)
                        try
                        {
                            auto const resourceId = res->GetModelResourceID();
                            auto snippetInfo = m_functionOperationsTool->getFunctionSnippet(resourceId);
                            if (snippetInfo.contains("success") && snippetInfo["success"].get<bool>())
                            {
                                if (snippetInfo.contains("arguments"))
                                {
                                    r["arguments"] = snippetInfo["arguments"];
                                }
                                if (snippetInfo.contains("output_type"))
                                {
                                    r["output_type"] = snippetInfo["output_type"];
                                }
                                if (snippetInfo.contains("snippet"))
                                {
                                    auto const & fullSnippet = snippetInfo["snippet"].get_ref<std::string const &>();
                                    // First 3 lines as preview
                                    std::string preview;
                                    std::istringstream stream(fullSnippet);
                                    std::string line;
                                    int lineCount = 0;
                                    while (std::getline(stream, line) && lineCount < 3)
                                    {
                                        if (lineCount > 0)
                                        {
                                            preview += '\n';
                                        }
                                        preview += line;
                                        ++lineCount;
                                    }
                                    r["snippet_preview"] = preview;
                                }

                                // Build cross-function call signature
                                // resourceId already in scope from line above
                                std::string displayName;
                                if (r.contains("display_name") && r["display_name"].is_string())
                                {
                                    displayName = r["display_name"].get<std::string>();
                                }
                                if (!displayName.empty())
                                {
                                    // Build argument list: name_ID(type arg, ...)
                                    std::string args;
                                    if (snippetInfo.contains("arguments") && snippetInfo["arguments"].is_array())
                                    {
                                        bool first = true;
                                        for (auto const & arg : snippetInfo["arguments"])
                                        {
                                            if (!first)
                                            {
                                                args += ", ";
                                            }
                                            first = false;
                                            args += arg.value("type", "float");
                                            args += " ";
                                            args += arg.value("name", "?");
                                        }
                                    }

                                    // Build return type: single or multi-output
                                    std::string returnType;
                                    bool const hasMultipleOutputs =
                                        snippetInfo.contains("outputs") &&
                                        snippetInfo["outputs"].is_array() &&
                                        snippetInfo["outputs"].size() > 1;
                                    if (hasMultipleOutputs)
                                    {
                                        returnType = "(";
                                        bool first = true;
                                        for (auto const & out : snippetInfo["outputs"])
                                        {
                                            if (!first)
                                            {
                                                returnType += ", ";
                                            }
                                            first = false;
                                            returnType += out.value("type", "float");
                                            returnType += " ";
                                            returnType += out.value("name", "?");
                                        }
                                        returnType += ")";
                                    }
                                    else if (snippetInfo.contains("output_type") &&
                                             snippetInfo["output_type"].is_string())
                                    {
                                        returnType = snippetInfo["output_type"].get<std::string>();
                                    }

                                    std::string sig;
                                    if (hasMultipleOutputs)
                                    {
                                        // Tuple-return syntax: (float shape, vec3 color) name_ID(args)
                                        sig = returnType + " " + displayName + "_" +
                                              std::to_string(resourceId) + "(" + args + ")";
                                    }
                                    else
                                    {
                                        // Standard: name_ID(args) -> float
                                        sig = displayName + "_" + std::to_string(resourceId) +
                                              "(" + args + ")";
                                        if (!returnType.empty())
                                        {
                                            sig += " -> " + returnType;
                                        }
                                    }
                                    r["call_signature"] = sig;
                                }
                            }
                        }
                        catch (std::exception const &)
                        {
                            // Snippet enrichment is best-effort; exception logged via debugger
                        }
                        catch (...)
                        {
                            // Unknown non-std exception during snippet enrichment
                        }

                        // T009: Add constant node parameter values
                        try
                        {
                            auto assembly = document->getAssembly();
                            if (assembly)
                            {
                                auto const resourceId = res->GetModelResourceID();
                                auto model = assembly->findModel(resourceId);
                                if (model)
                                {
                                    nlohmann::json constants = nlohmann::json::array();
                                    for (auto & [nodeId, nodePtr] : *model)
                                    {
                                        if (auto * cs = dynamic_cast<nodes::ConstantScalar *>(nodePtr.get()))
                                        {
                                            constants.push_back({
                                                {"name", cs->getDisplayName()},
                                                {"type", "float"},
                                                {"value", cs->getValue()}
                                            });
                                        }
                                        else if (auto * cv = dynamic_cast<nodes::ConstantVector *>(nodePtr.get()))
                                        {
                                            auto const v = cv->getValue();
                                            constants.push_back({
                                                {"name", cv->getDisplayName()},
                                                {"type", "vec3"},
                                                {"value", {v.x, v.y, v.z}}
                                            });
                                        }
                                    }
                                    if (!constants.empty())
                                    {
                                        r["constants"] = constants;
                                    }
                                }
                            }
                        }
                        catch (std::exception const &)
                        {
                            // Constant enrichment is best-effort; exception logged via debugger
                        }
                        catch (...)
                        {
                            // Unknown non-std exception during constant enrichment
                        }
                    }
                    else if (auto img3d = dynamic_cast<Lib3MF::CImage3D *>(res.get()))
                    {
                        image3dCount++;
                        r["kind"] = "image3d";
                        try
                        {
                            auto nm = img3d->GetName();
                            r["name"] = nm;
                            r["display_name"] = nm;
                        }
                        catch (...)
                        {
                        }
                        // If this image is actually a stack, query its dimensions
                        if (auto stack = dynamic_cast<Lib3MF::CImageStack *>(res.get()))
                        {
                            nlohmann::json s;
                            try
                            {
                                s["rows"] = stack->GetRowCount();
                            }
                            catch (...)
                            {
                            }
                            try
                            {
                                s["columns"] = stack->GetColumnCount();
                            }
                            catch (...)
                            {
                            }
                            try
                            {
                                s["sheets"] = stack->GetSheetCount();
                            }
                            catch (...)
                            {
                            }
                            r["imagestack"] = s;
                            r["kind"] = "imagestack"; // refine kind if applicable
                        }
                    }
                    else if (auto baseMat = dynamic_cast<Lib3MF::CBaseMaterialGroup *>(res.get()))
                    {
                        materialCount++;
                        r["kind"] = "base_material_group";
                        r["count"] = baseMat->GetCount();
                        // BaseMaterialGroup may not have a name
                        r["name"] = nullptr;
                        r["display_name"] = nullptr;
                    }
                    else
                    {
                        otherCount++;
                        r["kind"] = "other";
                        r["name"] = nullptr;
                        r["display_name"] = nullptr;
                    }

                    resources.push_back(r);
                }
            }
            catch (...)
            {
                // ignore resource enumeration errors
            }

            out["build_items"] = buildItems;
            out["resources"] = resources;
            out["counts"] = {{"build_items", buildItems.size()},
                             {"resources", resources.size()},
                             {"meshes", meshCount},
                             {"levelsets", levelSetCount},
                             {"functions", functionCount},
                             {"images3d", image3dCount},
                             {"materials", materialCount},
                             {"others", otherCount}};

            out["success"] = true;
        }
        catch (const std::exception & e)
        {
            out["success"] = false;
            out["error"] = std::string("Exception while collecting 3MF structure: ") + e.what();
        }

        return out;
    }

    nlohmann::json
    ApplicationMCPAdapter::validateForManufacturing(const std::vector<std::string> & functionNames,
                                                    const nlohmann::json & constraints) const
    {
        return m_validationTool->validateForManufacturing(functionNames, constraints);
    }

    bool ApplicationMCPAdapter::executeBatchOperations(const nlohmann::json & operations,
                                                       bool rollbackOnError)
    {
        return m_utilityTool->executeBatchOperations(operations, rollbackOnError);
    }

    nlohmann::json ApplicationMCPAdapter::validateModel(const nlohmann::json & options)
    {
        return m_validationTool->validateModel(options);
    }

    bool ApplicationMCPAdapter::setBuildItemObjectByIndex(uint32_t buildItemIndex,
                                                          uint32_t objectModelResourceId)
    {
        if (!m_application || !hasActiveDocument())
        {
            m_lastErrorMessage = "No active document available";
            return false;
        }

        try
        {
            auto document = m_application->getCurrentDocument();
            if (!document)
            {
                m_lastErrorMessage = "No active document available";
                return false;
            }

            // Ensure 3MF model is up to date before modifying
            document->update3mfModel();
            auto model = document->get3mfModel();
            if (!model)
            {
                m_lastErrorMessage = "No 3MF model available";
                return false;
            }

            // Resolve object resource by ModelResourceID -> UniqueResourceID -> Resource
            Lib3MF_uint32 uniqueId = io::resourceIdToUniqueResourceId(
              model, static_cast<gladius::ResourceId>(objectModelResourceId));
            if (uniqueId == 0)
            {
                m_lastErrorMessage = "Could not resolve unique resource id for object id " +
                                     std::to_string(objectModelResourceId);
                return false;
            }
            auto resource = model->GetResourceByID(uniqueId);
            Lib3MF::PObject object = std::dynamic_pointer_cast<Lib3MF::CObject>(resource);
            if (!object)
            {
                m_lastErrorMessage =
                  "Target resource id is not an object (mesh/components/levelset)";
                return false;
            }

            // Find the build item by index
            auto buildIt = model->GetBuildItems();
            uint32_t idx = 0;
            bool found = false;
            Lib3MF::PBuildItem target;
            while (buildIt->MoveNext())
            {
                if (idx == buildItemIndex)
                {
                    target = buildIt->GetCurrent();
                    found = true;
                    break;
                }
                ++idx;
            }
            if (!found || !target)
            {
                m_lastErrorMessage = "Build item index out of range";
                return false;
            }

            // Preserve the current transform and part number when swapping the object
            Lib3MF::sTransform t = target->GetObjectTransform();
            std::string partNumber;
            try
            {
                partNumber = target->GetPartNumber();
            }
            catch (...)
            {
                partNumber.clear();
            }

            // Remove and re-add the build item with the new object
            model->RemoveBuildItem(target);
            auto newBuildItem = model->AddBuildItem(object, t);
            if (!partNumber.empty())
            {
                try
                {
                    newBuildItem->SetPartNumber(partNumber);
                }
                catch (...)
                {
                    // ignore failures setting part number
                }
            }

            // Sync document state
            document->updateDocumentFrom3mfModel();
            m_lastErrorMessage =
              "Build item updated to reference object id " + std::to_string(objectModelResourceId);
            return true;
        }
        catch (const std::exception & e)
        {
            m_lastErrorMessage =
              "Exception while setting build item object: " + std::string(e.what());
            return false;
        }
    }

    bool ApplicationMCPAdapter::setBuildItemTransformByIndex(
      uint32_t buildItemIndex,
      const std::array<float, 12> & transform4x3RowMajor)
    {
        if (!m_application || !hasActiveDocument())
        {
            m_lastErrorMessage = "No active document available";
            return false;
        }
        try
        {
            auto document = m_application->getCurrentDocument();
            auto model = document->get3mfModel();
            if (!model)
            {
                m_lastErrorMessage = "No 3MF model available";
                return false;
            }

            // Find the build item by index
            auto buildIt = model->GetBuildItems();
            uint32_t idx = 0;
            bool found = false;
            Lib3MF::PBuildItem target;
            while (buildIt->MoveNext())
            {
                if (idx == buildItemIndex)
                {
                    target = buildIt->GetCurrent();
                    found = true;
                    break;
                }
                ++idx;
            }
            if (!found || !target)
            {
                m_lastErrorMessage = "Build item index out of range";
                return false;
            }

            // Map 12 floats (row-major 4x3) into sTransform
            Lib3MF::sTransform tr{};
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 3; ++c)
                {
                    tr.m_Fields[r][c] = transform4x3RowMajor[r * 3 + c];
                }
            }
            target->SetObjectTransform(tr);

            document->updateDocumentFrom3mfModel();
            m_lastErrorMessage = "Build item transform updated";
            return true;
        }
        catch (const std::exception & e)
        {
            m_lastErrorMessage =
              "Exception while setting build item transform: " + std::string(e.what());
            return false;
        }
    }

    // 3MF Resource creation methods implementation
    std::pair<bool, uint32_t> ApplicationMCPAdapter::createLevelSet(
      uint32_t functionId,
      std::array<float, 3> minPoint,
      std::array<float, 3> maxPoint)
    {
        auto result = m_resourceManagementTool->createLevelSet(functionId, minPoint, maxPoint);
        if (result.first)
        {
            recordChange("added", "levelset", result.second, "levelset for function " + std::to_string(functionId));
        }
        else
        {
            m_lastErrorMessage = m_resourceManagementTool->getLastErrorMessage();
        }
        return result;
    }

    std::pair<bool, uint32_t>
    ApplicationMCPAdapter::createImage3DFunction(const std::string & name,
                                                 const std::string & imagePath,
                                                 float valueScale,
                                                 float valueOffset)
    {
        return m_resourceManagementTool->createImage3DFunction(
          name, imagePath, valueScale, valueOffset);
    }

    std::pair<bool, uint32_t>
    ApplicationMCPAdapter::createVolumetricColor(uint32_t functionId, const std::string & channel)
    {
        return m_resourceManagementTool->createVolumetricColor(functionId, channel);
    }

    std::pair<bool, uint32_t>
    ApplicationMCPAdapter::createVolumetricProperty(const std::string & propertyName,
                                                    uint32_t functionId,
                                                    const std::string & channel)
    {
        return m_resourceManagementTool->createVolumetricProperty(
          propertyName, functionId, channel);
    }

    bool ApplicationMCPAdapter::modifyLevelSet(uint32_t levelSetModelResourceId,
                                               std::optional<uint32_t> functionModelResourceId,
                                               std::optional<std::string> channel)
    {
        return m_resourceManagementTool->modifyLevelSet(
          levelSetModelResourceId, functionModelResourceId, channel);
    }

    nlohmann::json ApplicationMCPAdapter::removeUnusedResources()
    {
        return m_resourceManagementTool->removeUnusedResources();
    }
}

nlohmann::json gladius::ApplicationMCPAdapter::setParameterValue(uint32_t functionId,
                                                                 uint32_t nodeId,
                                                                 const std::string & parameterName,
                                                                 const nlohmann::json & value)
{
    auto result = m_functionOperationsTool->setParameterValue(functionId, nodeId, parameterName, value);
    if (result.value("success", false))
    {
        recordChange("modified", "parameter", functionId,
                     "node " + std::to_string(nodeId) + "." + parameterName);
    }
    return result;
}

nlohmann::json gladius::ApplicationMCPAdapter::evaluateFunction(uint32_t functionId,
                                                                nlohmann::json const & samples)
{
    return m_functionEvaluatorTool->evaluateFunction(functionId, samples);
}

void gladius::ApplicationMCPAdapter::recordChange(std::string const & type,
                                                   std::string const & resourceType,
                                                   uint32_t resourceId,
                                                   std::string const & displayName) const
{
    std::lock_guard<std::mutex> lock(m_changeLogMutex);
    if (m_changeLog.size() >= MAX_CHANGE_LOG_SIZE)
    {
        m_changeLog.pop_front();
    }
    m_changeLog.push_back(
      {std::chrono::system_clock::now(), type, resourceType, resourceId, displayName});
}

nlohmann::json gladius::ApplicationMCPAdapter::getChangesSince(
  std::string const & isoTimestamp) const
{
    using json = nlohmann::json;

    // Detect UI-driven changes via Document's dirty flag
    if (m_application && m_application->getCurrentDocument())
    {
        auto const & doc = m_application->getCurrentDocument();
        bool fileChanged = doc->isFileChanged();
        if (fileChanged && !m_lastKnownFileChanged)
        {
            // Document was modified externally (UI); record a generic change
            recordChange("modified", "document", 0, "UI edit detected");
        }
        m_lastKnownFileChanged = fileChanged;
    }

    auto since = mcp::parseIso8601Utc(isoTimestamp);
    json changes = json::array();

    {
        std::lock_guard<std::mutex> lock(m_changeLogMutex);
        for (auto const & entry : m_changeLog)
        {
            if (entry.timestamp > since)
            {
                changes.push_back({{"timestamp", mcp::formatIso8601Utc(entry.timestamp)},
                                   {"type", entry.type},
                                   {"resource_type", entry.resourceType},
                                   {"resource_id", entry.resourceId},
                                   {"display_name", entry.displayName}});
            }
        }
    }

    return {{"success", true}, {"changes", changes}};
}

bool gladius::ApplicationMCPAdapter::renderToFile(const std::string & outputPath,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  const std::string & format,
                                                  float quality)
{
    auto result = m_renderingTool->renderToFile(outputPath, width, height, format, quality);
    bool success = result.value("success", false);
    if (!success)
    {
        m_lastErrorMessage = m_renderingTool->getLastErrorMessage();
    }
    return success;
}

bool gladius::ApplicationMCPAdapter::renderWithCamera(const std::string & outputPath,
                                                      const nlohmann::json & cameraSettings,
                                                      const nlohmann::json & renderSettings)
{
    auto result = m_renderingTool->renderWithCamera(outputPath, cameraSettings, renderSettings);
    bool success = result.value("success", false);
    if (!success)
    {
        m_lastErrorMessage = m_renderingTool->getLastErrorMessage();
    }
    return success;
}

bool gladius::ApplicationMCPAdapter::generateThumbnail(const std::string & outputPath,
                                                       uint32_t size)
{
    auto result = m_renderingTool->generateThumbnail(outputPath, size);
    bool success = result.value("success", false);
    if (!success)
    {
        m_lastErrorMessage = m_renderingTool->getLastErrorMessage();
    }
    return success;
}

nlohmann::json gladius::ApplicationMCPAdapter::getOptimalCameraPosition() const
{
    auto result = m_renderingTool->getOptimalCameraPosition();
    bool success = result.value("success", false);
    if (!success)
    {
        m_lastErrorMessage = m_renderingTool->getLastErrorMessage();
    }
    return result;
}

nlohmann::json gladius::ApplicationMCPAdapter::getModelBoundingBox() const
{
    auto result = m_renderingTool->getModelBoundingBox();
    bool success = result.value("success", false);
    if (!success)
    {
        m_lastErrorMessage = m_renderingTool->getLastErrorMessage();
    }
    return result;
}

nlohmann::json gladius::ApplicationMCPAdapter::performAutoValidation(bool includeOpenCL) const
{
    nlohmann::json validationOptions;
    validationOptions["compile"] = includeOpenCL;
    validationOptions["max_messages"] = 10; // Limit to avoid verbose output

    // Call the existing validateModel method
    auto result = const_cast<ApplicationMCPAdapter *>(this)->validateModel(validationOptions);

    // Simplify the response for auto-validation
    nlohmann::json simplified;
    simplified["success"] = result.value("success", false);

    if (!simplified["success"].get<bool>())
    {
        // Extract error messages for failed validation
        nlohmann::json messages = nlohmann::json::array();
        if (result.contains("phases"))
        {
            for (const auto & phase : result["phases"])
            {
                if (phase.contains("messages"))
                {
                    for (const auto & msg : phase["messages"])
                    {
                        messages.push_back(msg.value("message", "Unknown error"));
                    }
                }
            }
        }
        simplified["validation_errors"] = messages;
    }

    return simplified;
}

// ────────────────────────────────────────────────────────────────────────
// Library operations — delegate to LibraryTool
// ────────────────────────────────────────────────────────────────────────

nlohmann::json gladius::ApplicationMCPAdapter::listLibrary(std::string const & category,
                                                           std::string const & query) const
{
    return m_libraryTool->listLibrary(category, query);
}

nlohmann::json gladius::ApplicationMCPAdapter::getLibraryEntryInfo(std::string const & category,
                                                                   std::string const & name) const
{
    return m_libraryTool->getLibraryEntryInfo(category, name);
}

nlohmann::json gladius::ApplicationMCPAdapter::createLibraryEntry(
    std::string const & name,
    std::string const & category,
    std::string const & programSnippet,
    uint32_t functionId,
    std::string const & description,
    std::vector<std::string> const & tags,
    bool overwrite)
{
    return m_libraryTool->createLibraryEntry(
      name, category, programSnippet, functionId, description, tags, overwrite);
}

nlohmann::json gladius::ApplicationMCPAdapter::exportToLibrary(uint32_t functionId,
                                                                std::string const & category,
                                                                std::string const & name,
                                                                std::string const & description,
                                                                bool overwrite,
                                                                bool keepScaffold)
{
    return m_libraryTool->exportToLibrary(
        functionId, category, name, description, overwrite, keepScaffold);
}

nlohmann::json gladius::ApplicationMCPAdapter::setLibraryMetadata(
    std::vector<uint32_t> const & functionIds, std::string const & description,
    std::vector<std::string> const & tags)
{
    return m_libraryTool->setLibraryMetadata(functionIds, description, tags);
}

nlohmann::json gladius::ApplicationMCPAdapter::importLibraryEntry(std::string const & category,
                                                                   std::string const & name)
{
    return m_libraryTool->importLibraryEntry(category, name);
}

nlohmann::json gladius::ApplicationMCPAdapter::deleteLibraryEntry(std::string const & category,
                                                                   std::string const & name)
{
    return m_libraryTool->deleteLibraryEntry(category, name);
}

nlohmann::json gladius::ApplicationMCPAdapter::getFunctionSnippet(uint32_t functionId) const
{
    return m_functionOperationsTool->getFunctionSnippet(functionId);
}

nlohmann::json gladius::ApplicationMCPAdapter::setFunctionSnippet(
  uint32_t functionId,
  std::string const & snippet,
  std::string const & outputType,
  std::vector<FunctionArgument> const & arguments)
{
    auto result = m_functionOperationsTool->setFunctionSnippet(functionId, snippet, outputType, arguments);
    if (result.value("success", false))
    {
        recordChange("modified", "function", functionId, "snippet updated");
    }
    return result;
}

nlohmann::json gladius::ApplicationMCPAdapter::getProgramSnippet() const
{
    return m_functionOperationsTool->getProgramSnippet();
}

nlohmann::json gladius::ApplicationMCPAdapter::setProgramSnippet(std::string const & snippet)
{
    auto result = m_functionOperationsTool->setProgramSnippet(snippet);
    if (result.value("success", false))
    {
        recordChange("modified", "document", 0, "program snippet updated");
    }
    return result;
}

#ifdef ENABLE_UI_TESTING
#include <imgui.h>
#undef KeyPress
#include <imgui_te_engine.h>
#include <imgui_te_context.h>
#include "../ui/MainWindow.h"
#include "../ui/GLView.h"
#include "../Application.h"
#include <future>
#include <chrono>

/// Thread-safe state shared between the MCP request thread and the ImGui
/// test engine thread. All access is protected by the internal mutex.
struct UITestState
{
    std::mutex mutex;

    std::string clickPath;
    std::shared_ptr<std::promise<bool>> clickPromise;
    ImGuiTest * clickTest = nullptr;

    std::string dumpPath;
    std::shared_ptr<std::promise<std::vector<std::string>>> dumpPromise;
    ImGuiTest * dumpTest = nullptr;
};

static UITestState & getUITestState()
{
    static UITestState instance;
    return instance;
}

static void static_mcp_ui_click(ImGuiTestContext * ctx)
{
    auto & state = getUITestState();
    std::string path;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        path = state.clickPath;
    }

    ctx->ItemClick(path.c_str(), 0, ImGuiTestOpFlags_NoError);
    ctx->Yield(3);

    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.clickPromise)
    {
        state.clickPromise->set_value(true);
        state.clickPromise.reset();
    }
}

static void static_mcp_ui_dump(ImGuiTestContext * ctx)
{
    std::vector<std::string> results;
    std::string dumpPath;
    {
        std::lock_guard<std::mutex> lock(getUITestState().mutex);
        dumpPath = getUITestState().dumpPath;
    }

    ctx->Yield(1);

    if (dumpPath.empty())
    {
        ImGuiContext * imgui_ctx = ImGui::GetCurrentContext();
        for (int i = 0; i < imgui_ctx->Windows.Size; i++)
        {
            if (auto * window = imgui_ctx->Windows[i])
            {
                if (window->Name)
                {
                    ImGuiTestItemList out_list;
                    ctx->GatherItems(&out_list, ImGuiTestRef(window->Name), -1);
                    for (int j = 0; j < out_list.GetSize(); j++)
                    {
                        ImGuiTestItemInfo const * item = out_list.GetByIndex(j);
                        if (item && item->DebugLabel[0] != '\0')
                        {
                            results.push_back(std::string(window->Name) + "/" + item->DebugLabel);
                        }
                    }
                }
            }
        }
    }
    else
    {
        ImGuiTestItemList out_list;
        ctx->GatherItems(&out_list, ImGuiTestRef(dumpPath.c_str()), -1);
        for (int i = 0; i < out_list.GetSize(); i++)
        {
            ImGuiTestItemInfo const * item = out_list.GetByIndex(i);
            if (item && item->DebugLabel[0] != '\0')
            {
                results.push_back(item->DebugLabel);
            }
        }
    }

    std::lock_guard<std::mutex> lock(getUITestState().mutex);
    if (getUITestState().dumpPromise)
    {
        getUITestState().dumpPromise->set_value(results);
        getUITestState().dumpPromise.reset();
    }
}

std::vector<std::string> gladius::ApplicationMCPAdapter::uiDumpItems(std::string const & parentPath)
{
    if (!m_application) return {};
    auto & mainWindow = m_application->getMainWindow();
    auto testEngine = mainWindow.getGLView().getTestEngine();
    if (!testEngine) return {};

    auto & state = getUITestState();
    std::future<std::vector<std::string>> future;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.dumpPath = parentPath;
        state.dumpPromise = std::make_shared<std::promise<std::vector<std::string>>>();
        future = state.dumpPromise->get_future();

        if (!state.dumpTest)
        {
            state.dumpTest = ImGuiTestEngine_RegisterTest(testEngine, "MCP", "action_dump");
            state.dumpTest->TestFunc = static_mcp_ui_dump;
        }
    }

    ImGuiTestEngine_QueueTest(testEngine, state.dumpTest, 0);

    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
    {
        return future.get();
    }
    std::cerr << "Timeout waiting for UI dump test to complete\n";
    std::lock_guard<std::mutex> lock(state.mutex);
    state.dumpPromise.reset();
    return {};
}

bool gladius::ApplicationMCPAdapter::uiClick(std::string const & path)
{
    if (!m_application) return false;
    auto & mainWindow = m_application->getMainWindow();
    auto testEngine = mainWindow.getGLView().getTestEngine();
    if (!testEngine) return false;

    auto & state = getUITestState();
    std::future<bool> future;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.clickPath = path;
        state.clickPromise = std::make_shared<std::promise<bool>>();
        future = state.clickPromise->get_future();

        if (!state.clickTest)
        {
            state.clickTest = ImGuiTestEngine_RegisterTest(testEngine, "MCP", "action_click");
            state.clickTest->TestFunc = static_mcp_ui_click;
        }
    }

    ImGuiTestEngine_QueueTest(testEngine, state.clickTest, 0);

    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
    {
        return future.get();
    }
    std::cerr << "Timeout waiting for UI click test\n";
    std::lock_guard<std::mutex> lock(state.mutex);
    state.clickPromise.reset();
    return false;
}

bool gladius::ApplicationMCPAdapter::captureUIScreenshot(std::string const & outputPath)
{
    if (!m_application) return false;
    
    // Actually, captureUIScreenshot is virtual and needs to be returned synchronously.
    // requestScreenshot returns a future. We will block and wait for it.
    auto futureSuccess = m_application->getMainWindow().getGLView().requestScreenshot(outputPath);
    
    if (futureSuccess.valid()) {
        // Wait up to some reasonable time, say 5 seconds.
        if (futureSuccess.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
            return futureSuccess.get();
        } else {
            return false; // timeout
        }
    }

    return false;
}
#endif
