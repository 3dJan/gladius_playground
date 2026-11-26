/**
 * @file ResourceManagementTool.cpp
 * @brief Implementation of ResourceManagementTool for 3MF resource operations
 */

#include "ResourceManagementTool.h"
#include "../../Application.h"
#include "../../Document.h"
#include "../../io/3mf/ResourceIdUtil.h"
#include "../../io/3mf/VolumetricScaffold.h"
#include <array>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace gladius
{
    namespace mcp::tools
    {
        ResourceManagementTool::ResourceManagementTool(Application * app)
            : MCPToolBase(app)
        {
        }

        std::pair<bool, uint32_t> ResourceManagementTool::createLevelSet(
          uint32_t functionId,
          std::array<float, 3> minPoint,
          std::array<float, 3> maxPoint)
        {
            if (!validateActiveDocument())
            {
                return {false, 0};
            }

            try
            {
                auto document = m_application->getCurrentDocument();
                if (!document)
                {
                    setErrorMessage("No active document available");
                    return {false, 0};
                }

                // Update the 3MF model to ensure all resources are up to date
                document->update3mfModel();

                auto const scaffold = [&]() -> std::optional<io::VolumetricScaffoldResult>
                {
                    // Node-graph inspection and mutation run while the assembly token is held;
                    // updateDocumentFrom3mfModel() locks the mutex itself and must stay outside.
                    auto const assemblyToken = document->waitForAssemblyToken();

                    // Update the assembly to process function graphs and outputs
                    document->getAssembly()->updateInputsAndOutputs();

                    auto model3mf = document->get3mfModel();
                    if (!model3mf)
                    {
                        setErrorMessage("No 3MF model available");
                        return std::nullopt;
                    }

                    auto functionResource =
                      io::findFunctionResource(model3mf,
                                               static_cast<gladius::ResourceId>(functionId));
                    if (!functionResource)
                    {
                        setErrorMessage("Function with ID " + std::to_string(functionId)
                                        + " not found or is not a function");
                        return std::nullopt;
                    }

                    return io::createVolumetricOutputChain(model3mf,
                                                           functionResource,
                                                           minPoint,
                                                           maxPoint,
                                                           document->getSharedLogger());
                }();
                if (!scaffold)
                {
                    return {false, 0};
                }
                if (!scaffold->success)
                {
                    setErrorMessage("Exception during level set creation: " + scaffold->error);
                    return {false, 0};
                }

                // Update the document from the 3MF model
                document->updateDocumentFrom3mfModel();

                setErrorMessage("Level set created successfully from function ID: "
                                + std::to_string(functionId) + " using mesh ID: "
                                + std::to_string(scaffold->domainMeshId));
                return {true, scaffold->levelSetId};
            }
            catch (const std::exception & e)
            {
                setErrorMessage("Exception during level set creation: " + std::string(e.what()));
                return {false, 0};
            }
        }

        std::pair<bool, uint32_t>
        ResourceManagementTool::createImage3DFunction(const std::string & name,
                                                      const std::string & imagePath,
                                                      float valueScale,
                                                      float valueOffset)
        {
            if (!validateActiveDocument())
            {
                return {false, 0};
            }

            // TODO: Implement actual Image3D function creation when API is available
            // For now, create a function placeholder and return resource ID
            setErrorMessage("Image3D function '" + name + "' would be created from: " + imagePath +
                            " with scale: " + std::to_string(valueScale) +
                            ", offset: " + std::to_string(valueOffset));

            // Create a simple placeholder function for now
            // Note: This would need access to FunctionOperationsTool, but we'll keep it simple for
            // now
            return {false, 0}; // Placeholder - needs proper implementation
        }

        std::pair<bool, uint32_t>
        ResourceManagementTool::createVolumetricColor(uint32_t functionId,
                                                      const std::string & channel)
        {
            if (!validateActiveDocument())
            {
                return {false, 0};
            }

            // TODO: Implement actual volumetric color creation when API is available
            setErrorMessage("Volumetric color data would be created from function ID: " +
                            std::to_string(functionId) + " using channel: " + channel);
            return {true, functionId + 2000}; // Placeholder: offset function ID for color data ID
        }

        std::pair<bool, uint32_t>
        ResourceManagementTool::createVolumetricProperty(const std::string & propertyName,
                                                         uint32_t functionId,
                                                         const std::string & channel)
        {
            if (!validateActiveDocument())
            {
                return {false, 0};
            }

            // TODO: Implement actual volumetric property creation when API is available
            setErrorMessage("Volumetric property '" + propertyName +
                            "' would be created from function ID: " + std::to_string(functionId) +
                            " using channel: " + channel);
            return {true,
                    functionId + 3000}; // Placeholder: offset function ID for property data ID
        }

        bool ResourceManagementTool::modifyLevelSet(uint32_t levelSetModelResourceId,
                                                    std::optional<uint32_t> functionModelResourceId,
                                                    std::optional<std::string> channel)
        {
            if (!validateActiveDocument())
            {
                return false;
            }

            try
            {
                auto document = m_application->getCurrentDocument();
                auto model = document->get3mfModel();
                if (!model)
                {
                    setErrorMessage("No 3MF model available");
                    return false;
                }

                // Resolve level set by ModelResourceID
                Lib3MF_uint32 uniqueLevelSetId = io::resourceIdToUniqueResourceId(
                  model, static_cast<gladius::ResourceId>(levelSetModelResourceId));
                if (uniqueLevelSetId == 0)
                {
                    setErrorMessage("Could not resolve levelset unique id");
                    return false;
                }
                auto lsRes = model->GetResourceByID(uniqueLevelSetId);
                auto levelSet = dynamic_cast<Lib3MF::CLevelSet *>(lsRes.get());
                if (!levelSet)
                {
                    setErrorMessage("Resource is not a levelset");
                    return false;
                }

                // Optionally update the function reference
                if (functionModelResourceId.has_value())
                {
                    Lib3MF_uint32 uniqueFnId = io::resourceIdToUniqueResourceId(
                      model, static_cast<gladius::ResourceId>(functionModelResourceId.value()));
                    if (uniqueFnId == 0)
                    {
                        setErrorMessage("Could not resolve function unique id");
                        return false;
                    }
                    auto fnRes = model->GetResourceByID(uniqueFnId);
                    auto fn = dynamic_cast<Lib3MF::CFunction *>(fnRes.get());
                    if (!fn)
                    {
                        setErrorMessage("Target resource is not a function");
                        return false;
                    }
                    levelSet->SetFunction(fn);
                }

                // Optionally update the channel name
                if (channel.has_value())
                {
                    levelSet->SetChannelName(channel.value());
                }

                // Update document/assembly links post-change
                document->updateDocumentFrom3mfModel();
                setErrorMessage("Level set modified successfully");
                return true;
            }
            catch (const std::exception & e)
            {
                setErrorMessage("Exception while modifying level set: " + std::string(e.what()));
                return false;
            }
        }

        nlohmann::json ResourceManagementTool::removeUnusedResources()
        {
            nlohmann::json out;
            if (!validateActiveDocument())
            {
                out["success"] = false;
                out["removed_count"] = 0;
                out["error"] = getLastErrorMessage();
                return out;
            }

            try
            {
                auto document = m_application->getCurrentDocument();
                if (!document)
                {
                    out["success"] = false;
                    out["removed_count"] = 0;
                    out["error"] = "No active document";
                    return out;
                }

                // Ensure resource dependency graph is up-to-date, then delete all unused resources
                // Document::removeUnusedResources() already updates and logs internally
                std::size_t removed = document->removeUnusedResources();
                out["success"] = true;
                out["removed_count"] = static_cast<uint32_t>(removed);
                if (removed == 0)
                {
                    out["message"] = "No unused resources found";
                }
                return out;
            }
            catch (const std::exception & e)
            {
                out["success"] = false;
                out["removed_count"] = 0;
                out["error"] = std::string("Failed to remove unused resources: ") + e.what();
                return out;
            }
        }
    }
}
