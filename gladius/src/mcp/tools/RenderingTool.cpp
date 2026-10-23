/**
 * @file RenderingTool.cpp
 * @brief Implementation of RenderingTool for rendering operations
 */

#include "RenderingTool.h"
#include "../../Application.h"
#include "../../Document.h"
#include "compute/RenderContracts.h"
#include "compute/TranslucencyConfig.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#ifdef GLADIUS_ENABLE_OPENCL
#include "../../ui/OrbitalCamera.h"
#endif

namespace gladius
{
    namespace mcp::tools
    {
        namespace
        {
            using Vector3 = std::array<float, 3>;

            [[nodiscard]] Vector3 readVector3(nlohmann::json const & value,
                                              char const * const fieldName)
            {
                if (!value.is_array() || value.size() != 3u)
                {
                    throw std::invalid_argument(std::string{fieldName} +
                                                " must be an array of 3 numbers");
                }

                Vector3 result{};
                for (std::size_t index = 0; index < result.size(); ++index)
                {
                    if (!value[index].is_number())
                    {
                        throw std::invalid_argument(std::string{fieldName} +
                                                    " must contain only numbers");
                    }
                    result[index] = value[index].get<float>();
                    if (!std::isfinite(result[index]))
                    {
                        throw std::invalid_argument(std::string{fieldName} +
                                                    " must contain finite values");
                    }
                }
                return result;
            }

            [[nodiscard]] Vector3 subtract(Vector3 const & lhs, Vector3 const & rhs)
            {
                return {lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2]};
            }

            [[nodiscard]] Vector3 cross(Vector3 const & lhs, Vector3 const & rhs)
            {
                return {lhs[1] * rhs[2] - lhs[2] * rhs[1],
                        lhs[2] * rhs[0] - lhs[0] * rhs[2],
                        lhs[0] * rhs[1] - lhs[1] * rhs[0]};
            }

            [[nodiscard]] Vector3 normalized(Vector3 value, char const * const fieldName)
            {
                auto const length = std::sqrt(value[0] * value[0] + value[1] * value[1] +
                                              value[2] * value[2]);
                if (!std::isfinite(length) || length <= 1.0e-6f)
                {
                    throw std::invalid_argument(std::string{fieldName} + " must be non-zero");
                }
                for (auto & component : value)
                {
                    component /= length;
                }
                return value;
            }

            [[nodiscard]] compute::RenderCamera parseRenderCamera(
              nlohmann::json const & cameraSettings)
            {
                if (!cameraSettings.contains("eye_position") ||
                    !cameraSettings.contains("target_position"))
                {
                    throw std::invalid_argument(
                      "Camera settings must contain 'eye_position' and 'target_position'");
                }

                auto const eye = readVector3(cameraSettings.at("eye_position"), "eye_position");
                auto const target =
                  readVector3(cameraSettings.at("target_position"), "target_position");
                auto forward = normalized(subtract(target, eye), "Camera direction");
                auto up = cameraSettings.contains("up_vector")
                            ? normalized(readVector3(cameraSettings.at("up_vector"), "up_vector"),
                                         "up_vector")
                            : Vector3{0.0f, 0.0f, 1.0f};
                auto right = cross(forward, up);
                auto const rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1] +
                                                   right[2] * right[2]);
                if (rightLength <= 1.0e-6f)
                {
                    up = std::abs(forward[2]) > 0.99f ? Vector3{0.0f, 1.0f, 0.0f}
                                                     : Vector3{0.0f, 0.0f, 1.0f};
                    right = cross(forward, up);
                }
                right = normalized(right, "Camera right direction");
                up = normalized(cross(right, forward), "Camera up direction");
                return {.eyePosition = eye,
                        .forwardDirection = forward,
                        .rightDirection = right,
                        .upDirection = up};
            }

            /**
             * @brief Read the translucency knobs of a render request.
             *
             * "translucency" switches the front-to-back integrator on; the remaining keys tune how
             * the interior optical depth is integrated. Out-of-range values are clamped instead of
             * rejected, mirroring the UI sliders. Returns no value when the caller asked for nothing
             * translucency-related, so backends keep their own current settings.
             */
            [[nodiscard]] std::optional<compute::RenderSettingsSnapshot>
              parseTranslucencySettings(nlohmann::json const & renderSettings)
            {
                static constexpr char const * const keys[] = {"translucency",
                                                              "translucency_reference_thickness_mm",
                                                              "translucency_interior_step_mm",
                                                              "translucency_max_surfaces",
                                                              "translucency_transmittance_floor",
                                                              "translucency_max_interior_steps"};
                bool anyPresent = false;
                for (auto const * const key : keys)
                {
                    anyPresent = anyPresent || renderSettings.contains(key);
                }
                if (!anyPresent)
                {
                    return std::nullopt;
                }

                compute::RenderSettingsSnapshot settings{};
                compute::TranslucencyConfig config = compute::readTranslucency(settings);
                config.enabled = renderSettings.value("translucency", false);
                config.referenceThicknessMm = renderSettings.value(
                  "translucency_reference_thickness_mm", config.referenceThicknessMm);
                config.interiorStepSizeMm =
                  renderSettings.value("translucency_interior_step_mm", config.interiorStepSizeMm);
                config.maxSurfaces =
                  renderSettings.value("translucency_max_surfaces", config.maxSurfaces);
                config.transmittanceFloor =
                  renderSettings.value("translucency_transmittance_floor", config.transmittanceFloor);
                config.maxInteriorSteps =
                  renderSettings.value("translucency_max_interior_steps", config.maxInteriorSteps);
                compute::applyTranslucency(settings, config);
                return settings;
            }

            /// @brief Describe the translucency a render actually used, for the tool response.
            [[nodiscard]] nlohmann::json describeTranslucency(compute::TranslucencyConfig const & config)
            {
                return nlohmann::json{{"translucency", config.enabled},
                                      {"translucency_reference_thickness_mm",
                                       config.referenceThicknessMm},
                                      {"translucency_interior_step_mm", config.interiorStepSizeMm},
                                      {"translucency_max_surfaces", config.maxSurfaces},
                                      {"translucency_transmittance_floor", config.transmittanceFloor},
                                      {"translucency_max_interior_steps", config.maxInteriorSteps}};
            }

            /// @brief Describe the translucency a render actually used, for the tool response.
            [[nodiscard]] nlohmann::json describeTranslucency(
              std::optional<compute::RenderSettingsSnapshot> const & settings)
            {
                if (!settings.has_value())
                {
                    return describeTranslucency(compute::TranslucencyConfig{});
                }
                return describeTranslucency(compute::readTranslucency(*settings));
            }

            [[nodiscard]] nlohmann::json renderWebGpuPng(
              Document & document,
              std::string const & outputPath,
              unsigned int const width,
              unsigned int const height,
              std::string const & format,
              float const quality,
              std::optional<compute::RenderCamera> camera,
              std::string const & message,
              nlohmann::json additionalData = nlohmann::json::object(),
              std::optional<compute::RenderSettingsSnapshot> settings = std::nullopt)
            {
                if (format != "png")
                {
                    throw std::invalid_argument(
                      "The selected WebGPU thumbnail renderer currently supports PNG output only");
                }

                auto image = document.createThumbnailPng(width, height, std::move(camera), settings);
                auto const outputFile = std::filesystem::path(outputPath);
                if (!outputFile.parent_path().empty())
                {
                    std::filesystem::create_directories(outputFile.parent_path());
                }
                std::ofstream output(outputFile, std::ios::binary);
                if (!output)
                {
                    throw std::runtime_error("Unable to open thumbnail output file");
                }
                output.write(reinterpret_cast<char const *>(image.data.data()),
                             static_cast<std::streamsize>(image.data.size()));
                if (!output)
                {
                    throw std::runtime_error("Unable to write thumbnail output file");
                }

                nlohmann::json response = { {"success", true},
                                            {"message", message},
                                            {"outputPath", outputPath},
                                            {"actualWidth", image.width},
                                            {"actualHeight", image.height},
                                            {"requestedWidth", width},
                                            {"requestedHeight", height},
                                            {"format", "png"},
                                            {"quality", quality} };
                for (auto const & [key, value] : additionalData.items())
                {
                    response[key] = value;
                }
                return response;
            }
        }

        RenderingTool::RenderingTool(Application * app)
            : MCPToolBase(app)
        {
        }

#ifndef GLADIUS_ENABLE_OPENCL
                nlohmann::json RenderingTool::renderToFile(const std::string & outputPath,
                                                           unsigned int width,
                                                           unsigned int height,
                                                           const std::string & format,
                                                           float quality)
                {
                    if (!validateActiveDocument())
                    {
                        return createToolError(getLastErrorMessage());
                    }

                    try
                    {
                        auto document = m_application->getCurrentDocument();
                        if (!document)
                        {
                            throw std::runtime_error("No active document");
                        }
                        return renderWebGpuPng(*document,
                                               outputPath,
                                               width,
                                               height,
                                               format,
                                               quality,
                                               {},
                                               "Rendering completed successfully");
                    }
                    catch (std::exception const & error)
                    {
                        setErrorMessage("Rendering failed: " + std::string{error.what()});
                        return createToolError(getLastErrorMessage());
                    }
                }

                nlohmann::json RenderingTool::renderWithCamera(
                  const std::string & outputPath,
                  const nlohmann::json & cameraSettings,
                  const nlohmann::json & renderSettings)
                {
                    if (!validateActiveDocument())
                    {
                        return createToolError(getLastErrorMessage());
                    }

                    try
                    {
                        auto document = m_application->getCurrentDocument();
                        if (!document)
                        {
                            throw std::runtime_error("No active document");
                        }
                        auto const width = renderSettings.value("width", 256u);
                        auto const height = renderSettings.value("height", 256u);
                        auto const format = renderSettings.value("format", std::string{"png"});
                        auto const quality = renderSettings.value("quality", 0.9f);
                        auto const translucency = parseTranslucencySettings(renderSettings);
                        auto additionalData = nlohmann::json{
                          {"cameraSettings", cameraSettings},
                          {"renderSettings", renderSettings},
                          {"translucency", describeTranslucency(translucency)}};
                        return renderWebGpuPng(*document,
                                               outputPath,
                                               width,
                                               height,
                                               format,
                                               quality,
                                               parseRenderCamera(cameraSettings),
                                               "Camera-based rendering completed successfully",
                                               std::move(additionalData),
                                               translucency);
                    }
                    catch (std::exception const & error)
                    {
                        setErrorMessage("Camera-based rendering failed: " +
                                        std::string{error.what()});
                        return createToolError(getLastErrorMessage());
                    }
                }

                nlohmann::json RenderingTool::generateThumbnail(const std::string & outputPath,
                                        unsigned int size)
                {
                    if (!validateActiveDocument())
                    {
                        return createToolError(getLastErrorMessage());
                    }

                    try
                    {
                        auto document = m_application->getCurrentDocument();
                        if (!document)
                        {
                            throw std::runtime_error("No active document");
                        }

                        return renderWebGpuPng(*document,
                                               outputPath,
                                               size,
                                               size,
                                               "png",
                                               1.0f,
                                               {},
                                               "Thumbnail generated successfully",
                                               {{"size", size}});
                    }
                    catch (const std::exception & e)
                    {
                        setErrorMessage("Thumbnail generation failed: " + std::string(e.what()));
                        return createToolError(getLastErrorMessage());
                    }
                }

                nlohmann::json RenderingTool::getOptimalCameraPosition()
                {
                        return createToolError(
                            "Optimal camera calculation is unavailable in a pure WebGPU MCP build.");
                }

                nlohmann::json RenderingTool::getModelBoundingBox()
                {
                        return createToolError(
                            "MCP rendering is unavailable in a pure WebGPU build.");
                }

#else
        std::shared_ptr<ComputeCore> RenderingTool::getValidatedComputeCore()
        {
            auto document = m_application->getCurrentDocument();
            if (!document)
            {
                setErrorMessage("No active document");
                return nullptr;
            }

            auto core = document->getCore();
            if (!core)
            {
                setErrorMessage("No compute core available");
                return nullptr;
            }

            return core;
        }

        bool RenderingTool::prepareModelForRendering(std::shared_ptr<ComputeCore> core)
        {
            auto document = m_application->getCurrentDocument();
            if (!document)
            {
                setErrorMessage("No active document");
                return false;
            }

            // Ensure assembly is updated before rendering
            try
            {
                document->refreshModelBlocking();
            }
            catch (const std::exception & e)
            {
                setErrorMessage("Assembly update failed: " + std::string(e.what()));
                return false;
            }

            // Prepare the model for rendering
            if (!core->prepareImageRendering())
            {
                setErrorMessage(
                  "Model preparation for rendering failed: This may be due to model "
                  "compilation errors, SDF precomputation failure, or invalid bounding box. "
                  "Check model validation for detailed OpenCL compilation errors.");
                return false;
            }

            return true;
        }

        bool RenderingTool::validateRenderFormat(const std::string & format)
        {
            if (format != "png" && format != "jpg")
            {
                setErrorMessage("Unsupported format: " + format + ". Supported formats: png, jpg");
                return false;
            }
            return true;
        }

        void RenderingTool::createOutputDirectory(const std::string & outputPath)
        {
            std::filesystem::path outputFilePath(outputPath);
            std::filesystem::create_directories(outputFilePath.parent_path());
        }

        nlohmann::json RenderingTool::createSuccessResponse(const std::string & message,
                                                            const std::string & outputPath,
                                                            unsigned int requestedWidth,
                                                            unsigned int requestedHeight,
                                                            const std::string & format,
                                                            float quality,
                                                            const nlohmann::json & additionalData)
        {
            nlohmann::json response;
            response["success"] = true;
            response["message"] = message;
            response["outputPath"] = outputPath;
            response["actualWidth"] = 256;  // Current thumbnail system limitation
            response["actualHeight"] = 256; // Current thumbnail system limitation
            response["requestedWidth"] = requestedWidth;
            response["requestedHeight"] = requestedHeight;
            response["format"] = format;
            response["quality"] = quality;

            // Merge any additional data
            for (auto & [key, value] : additionalData.items())
            {
                response[key] = value;
            }

            return response;
        }

        nlohmann::json RenderingTool::renderToFile(const std::string & outputPath,
                                                   unsigned int width,
                                                   unsigned int height,
                                                   const std::string & format,
                                                   float quality)
        {
            nlohmann::json response;

            if (!validateActiveDocument())
            {
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }

            // Validate format
            if (!validateRenderFormat(format))
            {
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }

            try
            {
                auto document = m_application->getCurrentDocument();
                if (!document)
                {
                    throw std::runtime_error("No active document");
                }
                auto const backend = document->getBackendKind();
                if (backend.has_value() &&
                    *backend == compute::ComputeBackendKind::WebGPU)
                {
                    return renderWebGpuPng(*document,
                                           outputPath,
                                           width,
                                           height,
                                           format,
                                           quality,
                                           {},
                                           "Rendering completed successfully");
                }

                // Get validated compute core
                auto core = getValidatedComputeCore();
                if (!core)
                {
                    response["success"] = false;
                    response["error"] = getLastErrorMessage();
                    return response;
                }

                // Prepare model for rendering
                if (!prepareModelForRendering(core))
                {
                    response["success"] = false;
                    response["error"] = getLastErrorMessage();
                    return response;
                }

                // Create output directory and render
                createOutputDirectory(outputPath);
                core->saveThumbnail(outputPath);

                return createSuccessResponse(
                  "Rendering completed successfully (using thumbnail system - "
                  "currently limited to 256x256 resolution)",
                  outputPath,
                  width,
                  height,
                  format,
                  quality);
            }
            catch (const std::exception & e)
            {
                setErrorMessage("Rendering failed: " + std::string(e.what()));
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }
        }

        nlohmann::json RenderingTool::renderWithCamera(const std::string & outputPath,
                                                       const nlohmann::json & cameraSettings,
                                                       const nlohmann::json & renderSettings)
        {
            nlohmann::json response;

            if (!validateActiveDocument())
            {
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }

            // Parse camera settings
            if (!cameraSettings.contains("eye_position") ||
                !cameraSettings.contains("target_position"))
            {
                setErrorMessage(
                  "Camera settings must contain 'eye_position' and 'target_position'");
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }

            auto eyePos = cameraSettings["eye_position"];
            auto targetPos = cameraSettings["target_position"];

            if (!eyePos.is_array() || eyePos.size() != 3 || !targetPos.is_array() ||
                targetPos.size() != 3)
            {
                setErrorMessage("Camera positions must be arrays of 3 numbers [x, y, z]");
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }

            // Extract render settings (with defaults)
            std::string format = renderSettings.value("format", "png");
            unsigned int width = renderSettings.value("width", 256);
            unsigned int height = renderSettings.value("height", 256);
            float quality = renderSettings.value("quality", 0.9f);

            // Validate format
            if (!validateRenderFormat(format))
            {
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }

            try
            {
                auto document = m_application->getCurrentDocument();
                if (!document)
                {
                    throw std::runtime_error("No active document");
                }
                auto const backend = document->getBackendKind();
                auto const translucency = parseTranslucencySettings(renderSettings);
                if (backend.has_value() &&
                    *backend == compute::ComputeBackendKind::WebGPU)
                {
                    auto additionalData = nlohmann::json{
                      {"cameraSettings", cameraSettings},
                      {"renderSettings", renderSettings},
                      {"translucency", describeTranslucency(translucency)}};
                    return renderWebGpuPng(*document,
                                           outputPath,
                                           width,
                                           height,
                                           format,
                                           quality,
                                           parseRenderCamera(cameraSettings),
                                           "Camera-based rendering completed successfully",
                                           std::move(additionalData),
                                           translucency);
                }

                // Get validated compute core
                auto core = getValidatedComputeCore();
                if (!core)
                {
                    response["success"] = false;
                    response["error"] = getLastErrorMessage();
                    return response;
                }

                // Prepare model for rendering
                if (!prepareModelForRendering(core))
                {
                    response["success"] = false;
                    response["error"] = getLastErrorMessage();
                    return response;
                }

                // Store current camera settings to restore later
                auto originalEyePos = core->getResourceContext()->getEyePosition();
                auto originalViewMat = core->getResourceContext()->getModelViewPerspectiveMat();
                auto const originalTranslucency =
                  compute::readTranslucency(core->getResourceContext()->getRenderingSettings());
                if (translucency.has_value())
                {
                    // The OpenCL backend reads its rendering settings from the resource context,
                    // so a one-off request has to be pushed for this render and popped afterwards.
                    compute::applyTranslucency(core->getResourceContext()->getRenderingSettings(),
                                               compute::readTranslucency(*translucency));
                }

                try
                {
                    // Create and configure orbital camera with provided settings
                    ui::OrbitalCamera customCamera;

                    // Set the look-at target
                    ui::Position lookAtPos(targetPos[0].get<float>(),
                                           targetPos[1].get<float>(),
                                           targetPos[2].get<float>());
                    customCamera.setLookAt(lookAtPos);

                    // Calculate angles from eye position and target
                    ui::Position eyePosition(
                      eyePos[0].get<float>(), eyePos[1].get<float>(), eyePos[2].get<float>());

                    // Calculate direction vector from target to eye
                    ui::Position direction = eyePosition - lookAtPos;

                    // Calculate pitch and yaw from direction vector
                    float distance = direction.norm();
                    float pitch = std::asin(direction.z() / distance);
                    float yaw = std::atan2(direction.y(), direction.x());

                    customCamera.setAngle(pitch, yaw);
                    customCamera.snapToTarget();

                    // Apply the custom camera settings
                    core->applyCamera(customCamera);

                    // Create output directory and render
                    createOutputDirectory(outputPath);
                    core->saveThumbnail(outputPath);

                    // Read back what the render really used - the core clears the flag again when
                    // the model has no translucency entry point to answer it with.
                    auto const effective =
                      compute::readTranslucency(core->getResourceContext()->getRenderingSettings());

                    // Restore original camera settings
                    core->getResourceContext()->setEyePosition(originalEyePos);
                    core->getResourceContext()->setModelViewPerspectiveMat(originalViewMat);
                    compute::applyTranslucency(core->getResourceContext()->getRenderingSettings(),
                                               originalTranslucency);

                    nlohmann::json additionalData = {{"cameraSettings", cameraSettings},
                                                     {"renderSettings", renderSettings},
                                                     {"translucency", describeTranslucency(effective)}};

                    return createSuccessResponse(
                      "Camera-based rendering completed successfully (using thumbnail system - "
                      "currently limited to 256x256 resolution)",
                      outputPath,
                      width,
                      height,
                      format,
                      quality,
                      additionalData);
                }
                catch (const std::exception & /* e */)
                {
                    // Restore original camera settings on error
                    core->getResourceContext()->setEyePosition(originalEyePos);
                    core->getResourceContext()->setModelViewPerspectiveMat(originalViewMat);
                    compute::applyTranslucency(core->getResourceContext()->getRenderingSettings(),
                                               originalTranslucency);
                    throw; // Re-throw the exception
                }
            }
            catch (const std::exception & e)
            {
                setErrorMessage("Camera-based rendering failed: " + std::string(e.what()));
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }
        }

        nlohmann::json RenderingTool::generateThumbnail(const std::string & outputPath,
                                                        unsigned int size)
        {
            nlohmann::json response;

            if (!validateActiveDocument())
            {
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }

            try
            {
                auto document = m_application->getCurrentDocument();
                if (!document)
                {
                    throw std::runtime_error("No active document");
                }
                auto const backend = document->getBackendKind();
                if (backend.has_value() &&
                    *backend == compute::ComputeBackendKind::WebGPU)
                {
                    return renderWebGpuPng(*document,
                                           outputPath,
                                           size,
                                           size,
                                           "png",
                                           1.0f,
                                           {},
                                           "Thumbnail generated successfully",
                                           {{"size", size}});
                }

                // Get validated compute core
                auto core = getValidatedComputeCore();
                if (!core)
                {
                    response["success"] = false;
                    response["error"] = getLastErrorMessage();
                    return response;
                }

                // Prepare model for rendering
                if (!prepareModelForRendering(core))
                {
                    response["success"] = false;
                    response["error"] = getLastErrorMessage();
                    return response;
                }

                // Create output directory and generate thumbnail
                createOutputDirectory(outputPath);
                core->saveThumbnail(outputPath);

                nlohmann::json additionalData = {{"size", size}};
                return createSuccessResponse("Thumbnail generated successfully",
                                             outputPath,
                                             size,
                                             size,
                                             "png",
                                             1.0f,
                                             additionalData);
            }
            catch (const std::exception & e)
            {
                setErrorMessage("Thumbnail generation failed: " + std::string(e.what()));
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }
        }

        nlohmann::json RenderingTool::getOptimalCameraPosition()
        {
            nlohmann::json response;

            if (!validateActiveDocument())
            {
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }

            try
            {
                // Get validated compute core
                auto core = getValidatedComputeCore();
                if (!core)
                {
                    response["success"] = false;
                    response["error"] = getLastErrorMessage();
                    return response;
                }

                // Get the bounding box to calculate optimal camera position
                auto boundingBoxOpt = core->getBoundingBox();
                if (!boundingBoxOpt.has_value())
                {
                    setErrorMessage("No bounding box available for camera position calculation");
                    response["success"] = false;
                    response["error"] = getLastErrorMessage();
                    return response;
                }

                auto bbox = boundingBoxOpt.value();

                // Create an orbital camera and set it up optimally for the bounding box
                ui::OrbitalCamera optimalCamera;

                // Set good viewing angles (similar to thumbnail generation)
                optimalCamera.setAngle(0.6f, -2.0f);

                // Center the camera on the bounding box
                optimalCamera.centerView(bbox);

                // Update camera position
                optimalCamera.snapToTarget();

                // Adjust distance to fit the model in view (using reasonable viewport size)
                const float viewportSize = 512.0f;
                optimalCamera.adjustDistanceToTarget(bbox, viewportSize, viewportSize);

                // Final update
                optimalCamera.snapToTarget();

                // Get the calculated camera parameters
                auto eyePosition = optimalCamera.getEyePosition();
                auto lookAt = optimalCamera.getLookAt();

                response["success"] = true;
                response["message"] = "Optimal camera position calculated successfully";
                response["camera_settings"] = {
                  {"eye_position", {eyePosition.x, eyePosition.y, eyePosition.z}},
                  {"target_position", {lookAt.x, lookAt.y, lookAt.z}},
                  {"up_vector", {0.0f, 0.0f, 1.0f}}, // Standard up vector
                  {"field_of_view", 45.0f}           // Standard FOV
                };
                return response;
            }
            catch (const std::exception & e)
            {
                setErrorMessage("Camera position calculation failed: " + std::string(e.what()));
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }
        }

        nlohmann::json RenderingTool::getModelBoundingBox()
        {
            nlohmann::json response;

            if (!validateActiveDocument())
            {
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }

            try
            {
                // Get validated compute core
                auto core = getValidatedComputeCore();
                if (!core)
                {
                    response["success"] = false;
                    response["error"] = getLastErrorMessage();
                    return response;
                }

                // Get the bounding box from the compute core
                auto boundingBoxOpt = core->getBoundingBox();
                if (!boundingBoxOpt.has_value())
                {
                    setErrorMessage(
                      "No bounding box available - model may not be compiled or valid");
                    response["success"] = false;
                    response["error"] = getLastErrorMessage();
                    return response;
                }

                auto bbox = boundingBoxOpt.value();

                // Calculate center and size
                std::array<float, 3> min = {bbox.min.x, bbox.min.y, bbox.min.z};
                std::array<float, 3> max = {bbox.max.x, bbox.max.y, bbox.max.z};
                std::array<float, 3> center = {(bbox.min.x + bbox.max.x) / 2.0f,
                                               (bbox.min.y + bbox.max.y) / 2.0f,
                                               (bbox.min.z + bbox.max.z) / 2.0f};
                std::array<float, 3> size = {
                  bbox.max.x - bbox.min.x, bbox.max.y - bbox.min.y, bbox.max.z - bbox.min.z};

                response["success"] = true;
                response["message"] = "Bounding box calculated successfully";
                response["boundingBox"] = {
                  {"min", min}, {"max", max}, {"center", center}, {"size", size}};
                return response;
            }
            catch (const std::exception & e)
            {
                setErrorMessage("Bounding box calculation failed: " + std::string(e.what()));
                response["success"] = false;
                response["error"] = getLastErrorMessage();
                return response;
            }
        }
#endif
    }
}
