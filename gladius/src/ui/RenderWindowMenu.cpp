#include "RenderWindow.h"

#include <cstdint>
#include <string>

#include <IconsFontAwesome5.h>
#include "OverflowMenuBar.h"
#include "ShortcutManager.h"
#include "Widgets.h"
#include "compute/ComputeBackendSettings.h"
#if defined(GLADIUS_ENABLE_OPENCL)
#include "compute/ComputeCore.h"
#endif
#include "imgui.h"

namespace gladius::ui
{
    void RenderWindow::renderMenuBar(bool includeAdvancedOptions)
    {
        if (!ImGui::BeginMenuBar())
        {
            return;
        }

        OverflowMenuBar overflow;
        overflow.begin("RenderWindowBar");

        overflow.item(ICON_FA_COMPRESS_ARROWS_ALT "\tCenter View",
                      [&]
                      {
                          if (ImGui::MenuItem(reinterpret_cast<char const *>(
                                ICON_FA_COMPRESS_ARROWS_ALT "\tCenter View")))
                          {
                              auto const boundingBox = tryFetchBoundingBox(true);
                              bool asyncActive = false;
#if defined(GLADIUS_ENABLE_OPENCL)
                              asyncActive = isAsyncBackendActive();
#endif
                              if (boundingBox.has_value() || asyncActive)
                              {
                                  centerView();
                              }
                          }
                      });

        overflow.item(ICON_FA_CROSSHAIRS "\tPermanent Centering",
                      [&]
                      {
                          if (ImGui::MenuItem(reinterpret_cast<char const *>(
                                                ICON_FA_CROSSHAIRS "\tPermanent Centering"),
                                              nullptr,
                                              m_permanentCenteringEnabled))
                          {
                              togglePermanentCentering();
                          }

                          if (ImGui::IsItemHovered())
                          {
                              std::string shortcutText = "No shortcut assigned";
                              if (m_shortcutManager)
                              {
                                  auto const shortcut = m_shortcutManager->getShortcut(
                                    "camera.togglePermanentCentering");
                                  if (!shortcut.isEmpty())
                                  {
                                      shortcutText = shortcut.toString();
                                  }
                              }

                              ImGui::SetTooltip(
                                "Automatically center view when model changes, camera moves, or "
                                "viewport resizes\nShortcut: %s",
                                shortcutText.c_str());
                          }
                      });

        if (includeAdvancedOptions)
        {
            overflow.item(ICON_FA_ROBOT "\tHQ",
                          [&]
                          {
                              bool const wasHighQualityEnabled = m_enableHQRendering;
                              toggleButton(
                                {reinterpret_cast<char const *>(ICON_FA_ROBOT "\tHQ")},
                                &m_enableHQRendering);
                              if (wasHighQualityEnabled != m_enableHQRendering &&
                                  m_renderSettingsChangedCallback)
                              {
                                  m_renderSettingsChangedCallback();
                              }
                          });
        }

        overflow.item("Rendering Options",
                      [&]
                      {
#if defined(GLADIUS_ENABLE_OPENCL)
                          bool const hasOpenCLCore = m_core != nullptr;
#else
                          bool const hasOpenCLCore = false;
#endif
                          int renderingFlags = 0;
                          float quality = m_renderWindowState.renderQuality;
#if defined(GLADIUS_ENABLE_OPENCL)
                          if (hasOpenCLCore)
                          {
                              renderingFlags = static_cast<int>(
                                m_core->getResourceContext()->getRenderingSettings().flags);
                              quality = m_core->getResourceContext()->getRenderingSettings().quality;
                          }
#endif
                          if (!hasOpenCLCore && m_neutralBackendActive)
                          {
                              renderingFlags = static_cast<int>(m_neutralRenderSettings.flags);
                              quality = m_neutralRenderSettings.quality;
                          }

                          bool flagsChanged = false;
                          bool settingsChanged = false;
                          if (ImGui::BeginMenu("..."))
                          {
                              flagsChanged |= ImGui::CheckboxFlags(
                                "Show Build Plate", &renderingFlags, RF_SHOW_BUILDPLATE);
                              flagsChanged |= ImGui::CheckboxFlags(
                                "Cut Off Object", &renderingFlags, RF_CUT_OFF_OBJECT);
                              flagsChanged |= ImGui::CheckboxFlags(
                                "Show Field", &renderingFlags, RF_SHOW_FIELD);
                              flagsChanged |= ImGui::CheckboxFlags(
                                "Show Stack", &renderingFlags, RF_SHOW_STACK);
                              flagsChanged |= ImGui::CheckboxFlags(
                                "Show Coordinate System", &renderingFlags, RF_SHOW_COORDINATE_SYSTEM);

                              ImGui::Separator();
                              ImGui::SetNextItemWidth(150.f * m_uiScale);
                              bool const qualityChanged =
                                ImGui::SliderFloat("Quality", &quality, 0.1f, 2.0f);

                              if (ImGui::IsItemHovered())
                              {
                                  ImGui::SetTooltip(
                                    "Rendering quality (0.1 = Fast, 2.0 = Highest Quality)");
                              }
                              if (qualityChanged)
                              {
#if defined(GLADIUS_ENABLE_OPENCL)
                                  if (hasOpenCLCore)
                                  {
                                      m_core->getResourceContext()->getRenderingSettings().quality =
                                        quality;
                                  }
#endif
                                  if (!hasOpenCLCore && m_neutralBackendActive)
                                  {
                                      m_neutralRenderSettings.quality = quality;
                                  }
                                  setRenderQuality(quality);
                                  settingsChanged = true;
                                  invalidateView();
                              }
                              m_renderWindowState.renderQuality = quality;

                              if (includeAdvancedOptions)
                              {
                                  ImGui::Separator();
                                  if (ImGui::BeginMenu("Realtime Raymarch"))
                                  {
                                      auto config = m_renderUpdateCoordinator.realtimeConfig();
                                      auto const setMode =
                                        [&](async_rendering::RealtimeRaymarchMode mode)
                                      {
                                          config.mode = mode;
                                          m_renderUpdateCoordinator.configureRealtime(config);
                                          saveRealtimeRaymarchMode(mode);
                                      };

                                      if (ImGui::MenuItem(
                                            "Auto",
                                            nullptr,
                                            config.mode == async_rendering::RealtimeRaymarchMode::Auto))
                                      {
                                          setMode(async_rendering::RealtimeRaymarchMode::Auto);
                                      }
                                      if (ImGui::MenuItem(
                                            "Off",
                                            nullptr,
                                            config.mode == async_rendering::RealtimeRaymarchMode::Off))
                                      {
                                          setMode(async_rendering::RealtimeRaymarchMode::Off);
                                      }
                                      if (ImGui::MenuItem(
                                            "Force",
                                            nullptr,
                                            config.mode == async_rendering::RealtimeRaymarchMode::Force))
                                      {
                                          setMode(async_rendering::RealtimeRaymarchMode::Force);
                                      }

                                      ImGui::Separator();
                                      ImGui::Text("Budget: %.1f ms", config.targetFrameTimeMs);
                                      if (ImGui::IsItemHovered())
                                      {
                                          ImGui::SetTooltip(
                                            "Auto attempts full-resolution async raymarching when "
                                            "recent timings fit this budget; otherwise Gladius "
                                            "falls back to low-res preview/progressive rendering.");
                                      }
                                      ImGui::EndMenu();
                                  }
                              }

                              ImGui::EndMenu();
                          }

#if defined(GLADIUS_ENABLE_OPENCL)
                          if (hasOpenCLCore)
                          {
                              m_core->getResourceContext()->getRenderingSettings().flags =
                                renderingFlags;
                          }
#endif
                          if (!hasOpenCLCore && m_neutralBackendActive)
                          {
                              m_neutralRenderSettings.flags =
                                static_cast<std::uint32_t>(renderingFlags);
                          }

                          if (flagsChanged)
                          {
                              settingsChanged = true;
                              invalidateView();
                          }
                          if (settingsChanged && m_renderSettingsChangedCallback)
                          {
                              m_renderSettingsChangedCallback();
                          }
                      });

        overflow.end();

#if defined(GLADIUS_ENABLE_OPENCL)
        if (m_core && m_core->isAnyCompilationInProgressNonBlocking())
        {
            ImGui::TextUnformatted("Compilation in progress");
        }
#endif

        ImGui::EndMenuBar();
    }
} // namespace gladius::ui
