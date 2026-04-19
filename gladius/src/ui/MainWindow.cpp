#include "MainWindow.h"
#include "Theme.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <shellapi.h>
#endif

#include "../CliReader.h"
#include "../CliWriter.h"
#include "../EventLogger.h"
#include "../IconFontCppHeaders/IconsFontAwesome5.h"
#include "../TimeMeasurement.h"
#include "../io/MeshExporter.h"
#include "AboutDialog.h"
#include "FileChooser.h"
#include "FileSystemUtils.h"
#include "GLView.h"
#include "LibraryBrowser.h"
#include "Profiling.h"
#include "SvgWriter.h"
#include "compute/ComputeCore.h"
#include "exceptions.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "io/3mf/ImageStackCreator.h"
#include "io/3mf/Writer3mf.h"
#include "io/ImageStackExporter.h"
#include <nodes/ToCommandStreamVisitor.h>

namespace gladius::ui
{
    bool bigMenuItem(char const * label)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
        const bool result = ImGui::Button(label);
        ImGui::PopStyleColor();
        return result;
    }

    MainWindow::MainWindow()
        : m_logger(std::make_shared<events::Logger>())
        , m_shortcutSettingsDialog(nullptr) // Initialize with nullptr, we'll set it later
    {
        m_mainView.setRequestCloseCallBack([&]() { close(); });
    }

    void MainWindow::setup(std::shared_ptr<ComputeCore> core,
                           std::shared_ptr<Document> doc,
                           events::SharedLogger logger)
    {
        m_core = std::move(core);
        m_doc = std::move(doc);
        m_logger = std::move(logger);
        m_outline.setDocument(m_doc);

        // Set UI mode to true since we're using the MainWindow (UI interface)
        m_doc->setUiMode(true);

        m_modelEditor.setDocument(m_doc);

        // Sync shipped library items into the user's persistent library directory.
        // This copies new files without overwriting existing user customizations.
        (void) syncShippedLibrary();
        m_modelEditor.setLibraryRootDirectory(getUserLibraryDir());

        using namespace gladius;

        // Initialize keyboard shortcuts
        initializeShortcuts();

        m_renderWindow.initialize(m_core.get(), &m_mainView, m_shortcutManager, m_configManager);
        m_renderWindow.setDocument(m_doc.get());
        m_renderWindow.setExportState(&m_exportState);
        LOG_LOCATION
        m_core->getPreviewRenderProgram()->setOnProgramSwapCallBack([&]()
                                                                    { onPreviewProgramSwap(); });

        m_core->getOptimzedRenderProgram()->setOnProgramSwapCallBack([&]()
                                                                     { onPreviewProgramSwap(); });

        m_dirty = true;

        m_renderCallback = [&]() { updateModel(); };

        m_mainView.setRenderCallback(m_renderCallback);

        m_mainView.clearViewCallback();
        m_mainView.addViewCallBack([&]() { render(); });
        nodeEditor();

        m_mainView.setFileDropCallback([&](std::filesystem::path const & path) { open(path); });

        // Set up welcome screen callbacks
        m_welcomeScreen.setNewModelCallback(
          [this]()
          {
              newModel();
              m_welcomeScreen.hide();
          });

        m_welcomeScreen.setOpenFileCallback(
          [this](const std::filesystem::path & path)
          {
              if (path.empty())
              {
                  open();
              }
              else
              {
                  open(path);
              }
              m_welcomeScreen.hide();
          });

        // Set the logger for the welcome screen
        m_welcomeScreen.setLogger(m_logger);

        // Set backup manager for the welcome screen
        m_welcomeScreen.setBackupManager(&m_doc->getBackupManager());

        // Set backup restore callback
        m_welcomeScreen.setRestoreBackupCallback([this](const std::filesystem::path & backupPath)
                                                 { open(backupPath); });

        // Set recent files
        m_welcomeScreen.setRecentFiles(getRecentFiles(100));

        // Set examples directory
        m_welcomeScreen.setExamplesDirectory(getAppDir() / "examples");

        // Wire up export state to dialogs and editors that need it
        m_meshExporterDialog.setExportState(&m_exportState);
        m_modelEditor.setExportState(&m_exportState);

        // Defer the initial template load while the welcome screen is visible.
        // Starting it now would set isLoadingInProgress(), causing a click on a
        // thumbnail to be silently dropped by open().  The template is loaded
        // later when the welcome screen closes without a file selection.
        if (!m_welcomeScreen.isVisible())
        {
            newModel();
        }
        loadRenderSettings();
    }

    void MainWindow::dragParameter(std::string const & label,
                                   float * valuePtr,
                                   float minVal,
                                   float maxVal)
    {
        bool const changed = ImGui::DragFloat(label.c_str(), valuePtr, 0.001f, minVal, maxVal);
        m_contoursDirty = changed || m_contoursDirty;
        m_parameterDirty = changed || m_parameterDirty;
    }

    void MainWindow::renderSettingsDialog()
    {
        ImGui::Begin("Settings");

        if (ImGui::CollapsingHeader("Rendering"))
        {
            if (!m_computeAvailable)
            {
                ImGui::TextWrapped("Rendering and compute settings are unavailable because "
                                   "OpenCL/compute initialization failed.\nThe UI remains usable, "
                                   "but 3D preview and slicing are disabled.");
                ImGui::Separator();
                if (ImGui::Button("Close"))
                {
                    // Nothing else to do when compute is disabled
                }
            }
            else
            {
                // Add save/load buttons for settings if ConfigManager is available
                if (m_configManager)
                {
                    if (ImGui::Button("Save Settings"))
                    {
                        saveRenderSettings();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Load Settings"))
                    {
                        loadRenderSettings();
                        refreshModel();
                    }
                    ImGui::Separator();
                }

                ImGui::SliderFloat("Ray marching tolerance",
                                   &m_core->getResourceContext()->getRenderingSettings().quality,
                                   0.1f,
                                   20.0f);

                // Toggle SDF visualization using rendering flags
                auto & rs = m_core->getResourceContext()->getRenderingSettings();
                bool enableSdfRendering = (rs.flags & RF_SHOW_FIELD) != 0u;
                if (ImGui::Checkbox("Show Distance field", &enableSdfRendering))
                {
                    if (enableSdfRendering)
                        rs.flags |= RF_SHOW_FIELD;
                    else
                        rs.flags &= ~RF_SHOW_FIELD;
                    refreshModel();
                }
            }
        }

        if (ImGui::CollapsingHeader("Appearance"))
        {
            auto & names = themeNames();
            int currentIdx = static_cast<int>(m_mainView.getCurrentTheme());
            if (ImGui::Combo("Theme", &currentIdx, names.data(), THEME_COUNT))
            {
                m_mainView.setCurrentTheme(static_cast<ThemeId>(currentIdx));
            }
        }

        if (ImGui::CollapsingHeader("Keyboard Shortcuts"))
        {
            if (ImGui::Button("Configure Shortcuts"))
            {
                showShortcutSettings();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset to Defaults") && m_shortcutManager)
            {
                m_shortcutManager->resetAllShortcutsToDefault();
                m_logger->addEvent(
                  {"Keyboard shortcuts reset to defaults", events::Severity::Info});
            }

            ImGui::Text("Use Ctrl+K to open the keyboard shortcuts dialog");
            ImGui::Separator();

            // Show a few common shortcuts
            ImGui::Text("Common Shortcuts:");
            if (m_shortcutManager)
            {
                ImGui::Text("New: %s",
                            m_shortcutManager->getShortcut("file.new").toString().c_str());
                ImGui::Text("Open: %s",
                            m_shortcutManager->getShortcut("file.open").toString().c_str());
                ImGui::Text("Save: %s",
                            m_shortcutManager->getShortcut("file.save").toString().c_str());
                ImGui::Text("Save As: %s",
                            m_shortcutManager->getShortcut("file.saveAs").toString().c_str());
            }
        }

        auto z = m_core->getSliceHeight();
        if (ImGui::SliderFloat("Slice Position [mm]", &z, -20.f, 300.))
        {
            m_core->setSliceHeight(z);
        }
        bool tmp_m_dirty = m_dirty.load();

        ImGui::Checkbox("m_dirty", &tmp_m_dirty);
        ImGui::Checkbox("m_moving", &m_moving);

        if (ImGui::Button("Show Events"))
        {
            m_logView.show();
        }
        ImGui::End();
    }

    void MainWindow::setup()
    {
        ProfileFunction;
        LOG_SCOPE_DURATION_NAMED("MainWindow::setup()");
        m_initialized = true;

        // Create the GL context up-front so any GL-backed resources can be created safely
        {
            LOG_SCOPE_DURATION_NAMED("MainWindow::setup() - ensureInitialized");
            m_mainView.ensureInitialized();
        }

        // Set up minimal UI immediately so window appears responsive
        {
            LOG_SCOPE_DURATION_NAMED("MainWindow::setup() - setLogger");
            m_welcomeScreen.setLogger(m_logger);
        }
        {
            LOG_SCOPE_DURATION_NAMED("MainWindow::setup() - getRecentFiles");
            m_welcomeScreen.setRecentFiles(getRecentFiles(100));
        }

        // Set up minimal callbacks - will be replaced after compute init completes
        m_mainView.clearViewCallback();
        m_renderCallback = [&]() { /* no-op until compute ready */ };
        m_mainView.setRenderCallback(m_renderCallback);
        m_mainView.addViewCallBack([&]() { render(); });
        m_mainView.setFileDropCallback([&](std::filesystem::path const & path) { open(path); });

        // Start async OpenCL initialization
        startAsyncComputeInit();
    }

    void MainWindow::startAsyncComputeInit()
    {
        LOG_SCOPE_DURATION_NAMED("MainWindow::startAsyncComputeInit()");

        if (m_computeInitState != ComputeInitState::NotStarted)
        {
            return; // Already started or completed
        }

        m_computeInitState = ComputeInitState::InProgress;

        // Phase 1: Run the slow device enumeration on a background thread
        // The GL context is NOT required for this phase
        m_computeInitFuture = std::async(
          std::launch::async,
          []() -> ComputeEnumResult
          {
              ComputeEnumResult result;
              try
              {
                  // This is the slow part - OpenCL platform/device enumeration
                  std::ostringstream logStream;
                  auto accelerators = queryAccelerators(logStream);

                  if (accelerators.empty())
                  {
                      result.success = false;
                      result.errorMessage = "No suitable OpenCL devices found";
                      return result;
                  }

                  // Sort by performance estimation (best first)
                  std::stable_sort(std::begin(accelerators),
                                   std::end(accelerators),
                                   [](Accelerator const & lhs, Accelerator const & rhs)
                                   {
                                       return lhs.capabilities.performanceEstimation >
                                              rhs.capabilities.performanceEstimation;
                                   });

                  result.accelerators = std::move(accelerators);
                  result.success = true;
              }
              catch (GladiusException const & e)
              {
                  result.success = false;
                  result.errorMessage = e.what();
              }
              catch (std::exception const & e)
              {
                  result.success = false;
                  result.errorMessage = e.what();
              }
              return result;
          });
    }

    void MainWindow::pollComputeInit()
    {
        if (m_computeInitState != ComputeInitState::InProgress)
        {
            return;
        }

        // Check if future is ready (non-blocking)
        if (!m_computeInitFuture.valid())
        {
            return;
        }

        if (m_computeInitFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        {
            return; // Still in progress
        }

        // Future is ready - get the result and finalize on main thread
        m_computeInitState = ComputeInitState::Completed;

        try
        {
            auto result = m_computeInitFuture.get();

            if (result.success && !result.accelerators.empty())
            {
                // Phase 2: Create the OpenCL context on the main thread (GL context is current)
                auto const & selectedAccelerator = result.accelerators.front();

                auto context = std::make_shared<ComputeContext>(
                  EnableGLOutput::enabled, selectedAccelerator);
                context->setLogger(m_logger);
                context->setDebugOutputEnabled(m_openclDebugEnabled);
                gladius::setGlobalLogger(m_logger);

                if (!context->isValid())
                {
                    throw OpenCLContextCreationError("Context invalid after initialization");
                }

                m_core = std::make_shared<ComputeCore>(
                  context, RequiredCapabilities::OpenGLInterop, m_logger);
                m_doc = std::make_shared<Document>(m_core);
                m_computeAvailable = true;
                m_computeErrorMessage.clear();

                // Load render settings now that compute is available
                if (m_configManager)
                {
                    loadRenderSettings();
                }

                // Complete full setup with compute
                setup(m_core, m_doc, m_logger);

                // Open the startup file now that compute is available
                if (m_startupFile)
                {
                    loadFileDeferred(*m_startupFile);
                    m_startupFile.reset();
                }

                if (m_logger)
                {
                    m_logger->addEvent({"OpenCL initialized successfully", events::Severity::Info});
                }
            }
            else
            {
                m_computeAvailable = false;
                m_computeErrorMessage = result.errorMessage;
                m_showComputeErrorModal = true;
                m_welcomeScreen.hide();

                if (m_logger)
                {
                    m_logger->addEvent({std::string("Compute disabled: ") + result.errorMessage,
                                        events::Severity::Warning});
                }
            }
        }
        catch (const std::exception & e)
        {
            m_computeAvailable = false;
            m_computeErrorMessage = e.what();
            m_showComputeErrorModal = true;
            m_welcomeScreen.hide();

            if (m_logger)
            {
                m_logger->addEvent(
                  {std::string("Compute init error: ") + e.what(), events::Severity::Warning});
            }
        }

        m_computeInitState = ComputeInitState::Finalized;
    }

    void MainWindow::setupHeadless(events::SharedLogger logger)
    {
        ProfileFunction;
        // Only run once
        if (m_initialized && m_doc && m_core)
        {
            return;
        }

        m_logger = std::move(logger);
        m_initialized = true;

        // Initialize compute stack without OpenGL interop for headless safety
        try
        {
            auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
            context->setLogger(m_logger);
            gladius::setGlobalLogger(m_logger);
            context->setDebugOutputEnabled(m_openclDebugEnabled);
            if (!context->isValid())
            {
                throw OpenCLContextCreationError("Context invalid after initialization (headless)");
            }

            m_core =
              std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, m_logger);
            m_doc = std::make_shared<Document>(m_core);

            // Explicitly mark document as non-UI mode to disable backups and UI-only behaviors
            m_doc->setUiMode(false);

            m_computeAvailable = true;
            m_computeErrorMessage.clear();
        }
        catch (const GladiusException & e)
        {
            m_computeAvailable = false;
            m_computeErrorMessage = e.what();
            if (m_logger)
            {
                m_logger->addEvent({std::string("Headless compute disabled: ") + e.what(),
                                    events::Severity::Warning});
            }
        }
        catch (const std::exception & e)
        {
            m_computeAvailable = false;
            m_computeErrorMessage = e.what();
            if (m_logger)
            {
                m_logger->addEvent({std::string("Headless compute disabled: ") + e.what(),
                                    events::Severity::Warning});
            }
        }
    }

    void MainWindow::render()
    {
        ProfileFunction;
        m_uiScale = ImGui::GetIO().FontGlobalScale * 2.0f;

        // Poll for async compute initialization completion
        pollComputeInit();

        // Detect completion of async file load and refresh editors to the new Assembly.
        // (MainWindow::open() starts the async load; we defer resetEditorState() until loading finishes.)
        if (m_computeAvailable && m_doc)
        {
            bool const loadingNow = m_doc->isLoadingInProgress();
            if (m_asyncLoadState != AsyncLoadState::Idle && !loadingNow)
            {
                if (m_asyncLoadState == AsyncLoadState::LoadingWithReset)
                {
                    resetEditorState();
                    m_renderWindow.invalidateViewDuetoModelUpdate();
                    m_renderWindow.centerView();
                }
                m_asyncLoadState = AsyncLoadState::Idle;
            }
        }

        // Check if welcome screen is visible first
        bool welcomeScreenVisible = m_welcomeScreen.isVisible();

        bool welcomeScreenHasbeenClosed = !welcomeScreenVisible && m_wasWelcomeScreenVisible;
        if (welcomeScreenHasbeenClosed)
        {
            m_overlayFadeoutActive = true;
            m_mainView.startAnimationMode();

            // Process any pending file open from the welcome screen
            if (auto pendingPath = m_welcomeScreen.processFileOpen())
            {
                if (std::filesystem::exists(*pendingPath))
                {
                    open(*pendingPath);
                }
                else
                {
                    m_logger->addEvent(
                      {fmt::format("File not found: {}", pendingPath->string()),
                       events::Severity::Error});
                }
            }
            else if (m_asyncLoadState == AsyncLoadState::Idle &&
                     !m_asyncFileDialog.isActive())
            {
                // Welcome screen closed without selecting a file and no other
                // operation was already started (e.g. "New Project" or "Open
                // Existing").  Load the default template so the user has a
                // blank model to work with.
                newModel();
            }
        }
        m_wasWelcomeScreenVisible = welcomeScreenVisible;

        // Check for keyboard shortcuts
        ImGuiIO & io = ImGui::GetIO();
        processShortcuts(ShortcutContext::Global);

        // If compute is available, validate context
        if (m_computeAvailable && m_core)
        {
            // try to get the compute token
            auto computeToken = m_core->requestComputeToken();
            if (computeToken)
            {
                if (!m_core->getComputeContext()->isValid())
                {
                    m_logger->addEvent({"Reinitializing compute context", events::Severity::Info});

                    try
                    {
                        const auto context =
                          std::make_shared<ComputeContext>(EnableGLOutput::enabled);
                        context->setLogger(m_logger);
                        gladius::setGlobalLogger(m_logger);
                        context->setDebugOutputEnabled(m_openclDebugEnabled);
                        if (!context->isValid())
                        {
                            throw OpenCLContextCreationError("Context invalid after reinit");
                        }
                        m_core->setComputeContext(context);
                    }
                    catch (const std::exception & e)
                    {
                        // Switch to compute-disabled mode
                        m_computeAvailable = false;
                        m_computeErrorMessage = e.what();
                        m_logger->addEvent(
                          {std::string("Compute disabled after failure: ") + e.what(),
                           events::Severity::Error});
                    }
                }
            }
        }

        try
        {
            // If welcome screen is visible or fadeout is active, create a blocking overlay
            if (welcomeScreenVisible || (m_overlayFadeoutActive && m_overlayOpacity > 0.0f))
            {
                // Get the entire viewport size
                const ImVec2 viewportSize = ImGui::GetIO().DisplaySize;

                // Update overlay opacity if fadeout is active
                if (m_overlayFadeoutActive)
                {
                    // Reduce opacity based on frame time (smooth transition)
                    float const deltaTime = ImGui::GetIO().DeltaTime;
                    m_overlayOpacity -= deltaTime * 1.0f; // Adjust speed by changing multiplier

                    // Clamp to avoid negative values
                    if (m_overlayOpacity <= 0.0f)
                    {
                        m_overlayOpacity = 0.0f;

                        m_welcomeScreen.hide();
                        m_overlayFadeoutActive = false;
                    }

                    // Trigger animation mode to ensure continuous rendering during fadeout
                    m_mainView.startAnimationMode();
                }

                // Create a fullscreen, top-level modal overlay
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(viewportSize);

                // Special flags to ensure it blocks all input and stays on top
                ImGuiWindowFlags overlayFlags =
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                  ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;

                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

                // Begin a blocking overlay
                ImGui::Begin("##WelcomeScreenFullOverlay", nullptr, overlayFlags);

                // Draw a fully opaque rect over the entire screen
                ImDrawList * drawList = ImGui::GetWindowDrawList();
                drawList->AddRectFilled(
                  ImVec2(0, 0),
                  viewportSize,
                  ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, m_overlayOpacity)));

                ImGui::End();
                ImGui::PopStyleVar();
            }

            // Always render the docking area to preserve layout (even behind welcome screen)
            // This ensures the dock space state is maintained across welcome screen transitions
            mainWindowDockingArea();

            // Only render the normal UI if welcome screen is not visible and fadeout is complete
            if (!welcomeScreenVisible)
            {
                // If compute is not available, show a non-blocking banner in status areas

                if (m_showStyleEditor)
                {
                    ImGui::Begin("Style Editor", &m_showStyleEditor);
                    ImGui::ShowStyleEditor();
                    ImGui::End();
                }

                if (m_mainView.isViewSettingsVisible())
                {
                    renderSettingsDialog();
                }

                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    {12.f * m_uiScale, 8.f * m_uiScale});
                if (ImGui::BeginMainMenuBar())
                {
                    if (bigMenuItem(reinterpret_cast<const char *>(ICON_FA_BARS)))
                    {
                        m_showMainMenu = true;
                    }

                    if (m_modelEditor.isVisible())
                    {
                        if (ImGui::Button(
                              reinterpret_cast<const char *>(ICON_FA_PROJECT_DIAGRAM "\tGraph")))
                        {
                            m_modelEditor.setVisibility(false);
                        }
                    }
                    else
                    {
                        if (bigMenuItem(
                              reinterpret_cast<const char *>(ICON_FA_PROJECT_DIAGRAM "\tGraph")))
                        {
                            m_modelEditor.setVisibility(true);
                        }
                    }

                    if (m_renderWindow.isVisible())
                    {
                        if (ImGui::Button(
                              reinterpret_cast<const char *>(ICON_FA_DESKTOP "\tPreview")))
                        {
                            m_renderWindow.hide();
                        }
                    }
                    else
                    {
                        if (bigMenuItem(
                              reinterpret_cast<const char *>(ICON_FA_DESKTOP "\tPreview")))
                        {
                            m_renderWindow.show();
                        }
                    }

                    if (m_isSlicePreviewVisible)
                    {
                        if (ImGui::Button(
                              reinterpret_cast<const char *>(ICON_FA_LAYER_GROUP "\tSlice")))
                        {
                            m_sliceView.hide();
                        }
                    }
                    else
                    {
                        if (bigMenuItem(
                              reinterpret_cast<const char *>(ICON_FA_LAYER_GROUP "\tSlice")))
                        {
                            m_sliceView.show();
                        }
                    }

                    // Window mode buttons: toggle fullscreen and span (if available)
                    {
                        using gladius::FullscreenMode;
                        auto mode = m_mainView.getFullscreenMode();
                        bool const isWindowed = (mode == FullscreenMode::Windowed);
                        bool const isSpanning = (mode == FullscreenMode::SpanAllSameHeight);

                        // Toggle between windowed and fullscreen (single monitor)
                        if (bigMenuItem(reinterpret_cast<const char *>(
                              isWindowed ? ICON_FA_EXPAND "" : ICON_FA_COMPRESS "")))
                        {
                            m_mainView.setFullscreenMode(isWindowed ? FullscreenMode::SingleMonitor
                                                                    : FullscreenMode::Windowed);
                        }
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s", isWindowed ? "Fullscreen" : "Windowed");
                        }

                        // Span across monitors button (only show if available)
                        if (m_mainView.isSpanModeAvailable())
                        {
                            // Use different style when span mode is active
                            if (isSpanning)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Button,
                                                      ImVec4(1.0f, 0.0f, 0.0f, 0.6f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                                      ImVec4(1.0f, 0.0f, 0.0f, 0.8f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                                      ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
                            }

                            if (bigMenuItem(
                                  reinterpret_cast<const char *>(ICON_FA_ARROWS_ALT_H "")))
                            {
                                m_mainView.setFullscreenMode(isSpanning
                                                               ? FullscreenMode::Windowed
                                                               : FullscreenMode::SpanAllSameHeight);
                            }

                            if (isSpanning)
                            {
                                ImGui::PopStyleColor(3);
                            }

                            if (ImGui::IsItemHovered())
                            {
                                ImGui::SetTooltip(
                                  "%s", isSpanning ? "Exit Span Mode" : "Span Across Displays");
                            }
                        }
                    }

                    // Add keyboard shortcuts button
                    if (bigMenuItem(reinterpret_cast<const char *>(ICON_FA_KEYBOARD "\tShortcuts")))
                    {
                        showShortcutSettings();
                    }

                    if (m_currentAssemblyFileName)
                    {
                        if (m_fileChanged)
                        {
                            if (bigMenuItem(
                                  reinterpret_cast<const char *>(ICON_FA_SAVE "\tSave")))
                            {
                                save();
                            }
                            ImGui::TextUnformatted(
                              fmt::format("*{}", m_currentAssemblyFileName.value().string())
                                .c_str());
                        }
                        else
                        {
                            ImGui::TextUnformatted(
                              fmt::format("{}", m_currentAssemblyFileName.value().string())
                                .c_str());
                        }
                    }

                    ImGui::EndMainMenuBar();
                }
                ImGui::PopStyleVar();

                if (m_computeAvailable)
                {
                    sliceWindow();
                    renderWindow();

                    // Render export overlay BEFORE dialogs so they appear on top
                    renderExportOverlay();

                    meshExportDialog();
                    cliExportDialog();
                }

                // Library export dialog (modal, renders independently of compute)
                m_libraryExportDialog.render();
                if (m_libraryExportDialog.wasExportCompleted())
                {
                    m_modelEditor.refreshLibraryDirectories();
                }
                if (m_libraryExportDialog.hadError())
                {
                    m_logView.show();
                }

                mainMenu();
                showExitPopUp();
                showExportInProgressWarning();
                showSaveBeforeFileOperationPopUp();

                // Render library browser only when the model editor is visible.
                if (m_modelEditor.isVisible())
                {
                    m_modelEditor.renderLibraryBrowser();
                }

                if (m_shortcutSettingsDialog.isVisible())
                {
                    m_shortcutSettingsDialog.render();
                }
            }

            m_welcomeScreen.render();

            logViewer();
            m_about.render();
            // Camera update moved to RenderWindow::render() methods where core is guaranteed to be
            // ready m_renderWindow.updateCamera();

            // Render compute error modal ALWAYS (even when welcome screen is visible)
            // This ensures critical errors are shown immediately
            renderComputeErrorModal();

            // Process async file dialog results (must be after UI but before exception catch)
            processAsyncFileDialog();

            // Render status bar if welcome screen is not visible
            if (!welcomeScreenVisible)
            {
                renderStatusBar();
            }

            // Library browser is rendered from the main UI loop.
        }
        catch (OpenCLError & e)
        {
            m_logger->addEvent(
              {fmt::format("Unexpected exception: {}", e.what()), events::Severity::Error});
            m_logView.show();
        }
        catch (std::exception & e)
        {
            m_logger->addEvent(fmt::format("Unexpected exception: {}", e.what()));
            m_logView.show();
        }

        // Update event counts for status bar (removed automatic popup)
        m_lastEventCount = m_logger->getErrorCount();
        m_lastWarningCount = m_logger->getWarningCount();
    }

    void MainWindow::processAsyncFileDialog()
    {
        auto result = m_asyncFileDialog.checkResult();
        if (!result)
        {
            return; // No result yet
        }

        auto const operation = m_asyncDialogOp;
        m_asyncDialogOp = AsyncDialogOperation::None;

        // User cancelled the dialog
        if (!result->has_value())
        {
            // If we were waiting for OpenAfterSavePrompt, reset the popup state
            if (operation == AsyncDialogOperation::OpenAfterSavePrompt)
            {
                m_showSaveBeforeFileOperation = false;
                m_pendingFileOperation = PendingFileOperation::None;
                m_pendingOpenFilename.reset();
            }
            return;
        }

        std::filesystem::path const filename = result->value();

        switch (operation)
        {
        case AsyncDialogOperation::ExportCliCurrentLayer:
        {
            CliWriter writer;
            writer.saveCurrentLayer(filename, *m_core);
#ifdef WIN32
            ShellExecuteW(
              nullptr, L"open", writer.getFilename().c_str(), nullptr, nullptr, SW_SHOW);
#endif
            break;
        }
        case AsyncDialogOperation::ExportCliSliced:
        {
            auto exportPath = filename;
            exportPath.replace_extension(".cli");
            m_cliExportDialog.beginExport(exportPath, *m_core);
            break;
        }
        case AsyncDialogOperation::ExportSvgCurrentLayer:
        {
            SvgWriter svgWriter;
            svgWriter.saveCurrentLayer(filename, *m_core);
#ifdef WIN32
            ShellExecuteW(nullptr, L"open", filename.c_str(), nullptr, nullptr, SW_SHOW);
#endif
            break;
        }
        case AsyncDialogOperation::ExportVdb:
        {
            vdb::MeshExporter exporter;
            exporter.setQualityLevel(1);
            exporter.beginExport(filename, *m_core);
            while (exporter.advanceExport(*m_core)) {}
            exporter.finalizeExportVdb();
            break;
        }
        case AsyncDialogOperation::ExportNvdb:
        {
            vdb::MeshExporter exporter;
            exporter.setQualityLevel(1);
            exporter.beginExport(filename, *m_core);
            while (exporter.advanceExport(*m_core)) {}
            exporter.finalizeExportNanoVdb();
            break;
        }
        case AsyncDialogOperation::Import:
        {
            throw std::runtime_error("Import not implemented");
        }
        case AsyncDialogOperation::Open:
        {
            open(filename);
            break;
        }
        case AsyncDialogOperation::Merge:
        {
            m_doc->merge(filename);
            break;
        }
        case AsyncDialogOperation::SaveAs:
        {
            auto savePath = filename;
            savePath.replace_extension(".3mf");
            bool writeThumbnail = m_computeAvailable && m_core;
            m_doc->saveAs(savePath, writeThumbnail);
            m_renderWindow.invalidateViewDuetoModelUpdate();
            m_fileChanged = false;
            m_currentAssemblyFileName = savePath;
            addToRecentFiles(savePath);
            break;
        }
        case AsyncDialogOperation::SaveCurrentFunction:
        {
            auto function = m_modelEditor.currentModel();
            if (function)
            {
                auto savePath = filename;
                savePath.replace_extension(".3mf");
                gladius::io::saveFunctionTo3mfFile(savePath, *function);
            }
            break;
        }
        case AsyncDialogOperation::ImportImageStack:
        {
            io::ImageStackCreator creator;
            auto result = creator.importDirectoryWithPadding(m_doc->get3mfModel(), filename);

            // T052: Show notification if any images were padded
            if (result.hasPaddedFiles() && m_logger)
            {
                std::string message = fmt::format(
                    "ImageStack imported with padding to {}x{}. Padded {} file(s): ",
                    result.maxWidth,
                    result.maxHeight,
                    result.paddedFiles.size());

                // List first few padded files
                size_t const maxFilesToShow = 5;
                for (size_t i = 0; i < std::min(result.paddedFiles.size(), maxFilesToShow); ++i)
                {
                    if (i > 0)
                    {
                        message += ", ";
                    }
                    message += result.paddedFiles[i];
                }
                if (result.paddedFiles.size() > maxFilesToShow)
                {
                    message += fmt::format(
                        " and {} more", result.paddedFiles.size() - maxFilesToShow);
                }

                m_logger->addEvent({message, events::Severity::Info});
            }
            break;
        }
        case AsyncDialogOperation::OpenAfterSavePrompt:
        {
            loadFileDeferred(filename);

            // Close the save-before-file-operation popup
            m_showSaveBeforeFileOperation = false;
            m_pendingFileOperation = PendingFileOperation::None;
            m_pendingOpenFilename.reset();
            break;
        }
        case AsyncDialogOperation::None:
            break;
        }
    }

    void MainWindow::refreshModel()
    {
        if (m_doc->refreshModelIfNoCompilationIsRunning())
        {
            // Clear errors and warnings from events list when compilation is successful
            m_logger->clear();

            m_renderWindow.invalidateViewDuetoModelUpdate();
            m_modelEditor.markModelAsUpToDate();
        }
        m_renderWindow.invalidateView();
    }

    void MainWindow::nodeEditor()
    {
        m_mainView.addViewCallBack(
          [&]()
          {
              if (!m_modelEditor.isVisible())
              {
                  return;
              }

              // Process model editor shortcuts if visible and hovered
              if (m_modelEditor.isHovered())
              {
                  processShortcuts(ShortcutContext::ModelEditor);
              }

              const auto parameterModifiedByModelEditor = m_modelEditor.showAndEdit();
              // T047: Route parameter changes through throttle
              if (parameterModifiedByModelEditor)
              {
                  m_parameterThrottle.onParameterChanged();
              }
              m_parameterDirty = parameterModifiedByModelEditor || m_parameterDirty;
              m_dirty = m_parameterDirty || m_dirty;
              bool const modelWasModified = m_modelEditor.modelWasModified();
              bool const compileRequested = m_modelEditor.isCompileRequested();

              // updateInputsAndOutputs() and updateParameterRegistration() are called at the
              // start of refreshWorker() on the background thread. Running them here on the
              // UI thread was a redundant O(N·models) stall per structural edit. Nodes
              // self-register in create()/insert(); the lazy updateGraphAndOrderIfNeeded()
              // in visitNodes() keeps the render path consistent for the current frame.

              if (modelWasModified || parameterModifiedByModelEditor)
              {
                  markFileAsChanged();
              }

              // Refresh model when compile is explicitly requested (structural changes)
              // Instead of triggering immediately, signal a structural edit so the
              // debouncer can coalesce rapid sequential edits (e.g. paste, multi-link).
              if (compileRequested)
              {
                  m_doc->signalStructuralEdit();
                  m_parameterThrottle.reset(); // Full compile resets throttle state
                  m_contoursDirty = true;
              }
              // For parameter-only changes, m_parameterDirty is already set above.
              // updateModel() will handle it using the fast updateParameter() path
              // and call invalidateViewDueToParameterChange() which bumps the epoch.

              // Clear modified flags after signaling. The debouncer in
              // Document::dispatchStructuralUpdateIfReady() handles retry
              // when a compilation is already in-progress — the UI-side
              // flags no longer need to survive across frames.
              if (modelWasModified || parameterModifiedByModelEditor)
              {
                  m_modelEditor.markModelAsUpToDate();
              }
          });
    }

    void MainWindow::mainWindowDockingArea()
    {
        ImGuiWindowFlags window_flags =
          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing |
          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar;
#ifdef IMGUI_HAS_DOCK
        window_flags |= ImGuiWindowFlags_NoDocking;
#endif

        // Measure the menu bar height using the SAME padding as the actual menu bar in render()
        // The actual menu bar uses: ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.f * m_uiScale, 8.f * m_uiScale});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.f * m_uiScale, 8.f * m_uiScale});
        ImGui::BeginMainMenuBar();
        float const menuBarHeight = ImGui::GetWindowHeight();
        ImGui::EndMainMenuBar();
        ImGui::PopStyleVar();

        const auto & io = ImGui::GetIO();

        ImGui::SetNextWindowBgAlpha(0.0f);
        auto constexpr silderWidth = 1;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("MainWindowDockingArea", nullptr, window_flags);

        // Account for status bar height
        float const statusBarHeight = ImGui::GetFrameHeight();
        ImGui::SetWindowSize(ImVec2(io.DisplaySize.x - silderWidth,
                                    io.DisplaySize.y - menuBarHeight - statusBarHeight));

#ifdef IMGUI_HAS_DOCK
        const auto dockspaceID = ImGui::GetID("MainDockingSpace");
        ImGui::DockSpace(dockspaceID,
                         ImVec2(0.0f, 0.0f),
                         ImGuiDockNodeFlags_None |
                           ImGuiDockNodeFlags_PassthruCentralNode /*|ImGuiDockNodeFlags_NoResize*/);

#endif
        ImGui::SetWindowPos("MainWindowDockingArea", {0, menuBarHeight}, ImGuiCond_Always);
        ImGui::End();
        ImGui::PopStyleVar();
    }

    void MainWindow::newModel()
    {
        if (!m_computeAvailable || !m_doc)
        {
            if (m_logger)
            {
                m_logger->addEvent({"New model is unavailable: compute/renderer disabled",
                                    events::Severity::Warning});
            }
            return;
        }
        // Don't allow new model while loading is in progress
        if (m_doc->isLoadingInProgress())
        {
            return;
        }
        if (m_fileChanged)
        {
            m_pendingFileOperation = PendingFileOperation::NewModel;
            m_pendingOpenFilename.reset();
            m_showSaveBeforeFileOperation = true;
            return;
        }

        // Cancel all in-flight async GPU work before loading (same reason as loadFileDeferred).
        m_renderWindow.cancelAllAsyncWork();

        // Defer editor reset until the async load inside newFromTemplate() completes.
        // (Same deferred pattern used by loadFileDeferred().)
        m_asyncLoadState = AsyncLoadState::LoadingWithReset;
        m_doc->newFromTemplate();
    }

    void MainWindow::renderWindow()
    {
        if (!m_computeAvailable)
        {
            return; // skip rendering UI when compute is disabled
        }
        // Process render window shortcuts
        if (m_renderWindow.isVisible() && m_renderWindow.isHovered() && m_renderWindow.isFocused())
        {
            processShortcuts(ShortcutContext::RenderWindow);
        }

        m_renderWindow.renderWindow();
    }

    void MainWindow::mainMenu()
    {
        if (!m_showMainMenu)
        {
            return;
        }

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
#ifdef IMGUI_HAS_DOCK
        window_flags |= ImGuiWindowFlags_NoDocking;
#endif
        ImGui::BeginMainMenuBar();
        const auto menuBarHeight = ImGui::GetWindowHeight();
        ImGui::EndMainMenuBar();
        auto & io = ImGui::GetIO();
        const auto menuWidth = 400.f * m_uiScale;
        auto closeMenu = [&]()
        {
            m_showMainMenu = false;
            m_mainMenuPosX = -menuWidth;
        };

        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {20 * m_uiScale, 20 * m_uiScale});

        ImGui::Begin("Menu", &m_showMainMenu, window_flags);

        ImGui::SetWindowSize(ImVec2(menuWidth, io.DisplaySize.y - menuBarHeight));

        // Check if export or file loading is in progress - disable model-modifying operations
        bool const exportInProgress = m_exportState.isExportInProgress();
        bool const loadingInProgress = m_doc && m_doc->isLoadingInProgress();
        bool const operationInProgress = exportInProgress || loadingInProgress;

        if (loadingInProgress)
        {
            ImGui::TextColored(ImVec4{0.2F, 0.6F, 1.0F, 1.0F},
                               ICON_FA_HOURGLASS_HALF " Loading file...");
            ImGui::Separator();
        }
        else if (exportInProgress)
        {
            ImGui::TextColored(ImVec4{1.0F, 0.6F, 0.2F, 1.0F},
                               ICON_FA_HOURGLASS_HALF " Export in progress...");
            ImGui::Separator();
        }

        ImGui::BeginDisabled(operationInProgress);
        if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_FILE "\tNew")))
        {
            closeMenu();
            newModel();
        }
        if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_FOLDER_OPEN "\tOpen")))
        {
            closeMenu();
            open();
        }

        if (m_showAuthoringTools)
        {
            if (ImGui::MenuItem(
                  reinterpret_cast<const char *>(ICON_FA_FOLDER_OPEN "\tImport functions")))
            {
                closeMenu();
                merge();
            }

            if (ImGui::MenuItem(
                  reinterpret_cast<const char *>(ICON_FA_FOLDER_OPEN "\tImport Image Stack")))
            {
                closeMenu();
                importImageStack();
            }
            if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_SAVE "\tSave As")))
            {
                closeMenu();
                saveAs();
            }

            if (ImGui::MenuItem(
                  reinterpret_cast<const char *>(ICON_FA_SAVE "\tSave Current Function As")))
            {
                closeMenu();
                saveCurrentFunction();
            }

            if (ImGui::MenuItem(
                  reinterpret_cast<const char *>(ICON_FA_BOOK "\tExport to Library...")))
            {
                closeMenu();
                m_libraryExportDialog.open(m_doc, getUserLibraryDir());
            }

            if (m_currentAssemblyFileName)
            {
                if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_SAVE "\tSave")))
                {
                    closeMenu();
                    save();
                }
            }
        }
        ImGui::EndDisabled(); // End export lock for file operations

        if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_HOME "\tHome")))
        {
            closeMenu();
            showWelcomeScreen();
        }

        CliWriter writer;

        ImGui::Separator();
        ImGui::TextUnformatted("Export");

        // Disable export menu items if async dialog is active
        bool const dialogActive = m_asyncFileDialog.isActive();

        if (m_computeAvailable)
        {
            ImGui::BeginDisabled(dialogActive);
            if (ImGui::MenuItem(reinterpret_cast<const char *>("\t" ICON_FA_MINUS
                                                               "\tExport current layer as CLI")))
            {
                closeMenu();
                m_asyncDialogOp = AsyncDialogOperation::ExportCliCurrentLayer;
                m_asyncFileDialog.saveFile({"*.cli"});
            }

            if (ImGui::MenuItem(reinterpret_cast<const char *>("\t" ICON_FA_ALIGN_JUSTIFY
                                                               "\tSliced Geometry as CLI")))
            {
                closeMenu();
                m_asyncDialogOp = AsyncDialogOperation::ExportCliSliced;
                std::filesystem::path defaultPath = "part.cli";
                if (m_currentAssemblyFileName.has_value())
                {
                    defaultPath = m_currentAssemblyFileName.value();
                    defaultPath.replace_extension("cli");
                }
                m_asyncFileDialog.saveFile({"*.cli"}, defaultPath);
            }

            if (ImGui::MenuItem(reinterpret_cast<const char *>("\t" ICON_FA_MINUS
                                                               "\tExport current layer as SVG")))
            {
                closeMenu();
                m_asyncDialogOp = AsyncDialogOperation::ExportSvgCurrentLayer;
                m_asyncFileDialog.saveFile({"*.svg"});
            }

            if (ImGui::MenuItem(reinterpret_cast<const char *>("\t" ICON_FA_FILE_CODE "\tOpenVDB")))
            {
                closeMenu();
                m_asyncDialogOp = AsyncDialogOperation::ExportVdb;
                m_asyncFileDialog.saveFile({"*.vdb"});
            }

            if (ImGui::MenuItem(reinterpret_cast<const char *>("\t" ICON_FA_FILE_CODE "\tNanoVDB")))
            {
                closeMenu();
                m_asyncDialogOp = AsyncDialogOperation::ExportNvdb;
                m_asyncFileDialog.saveFile({"*.nvdb"});
            }

            if (ImGui::MenuItem(
                  reinterpret_cast<const char *>("\t" ICON_FA_FILE_CODE "\tMesh Export...")))
            {
                closeMenu();
                // Open dialog with suggested filename based on current assembly
                std::filesystem::path suggestedFilename;
                if (m_currentAssemblyFileName.has_value())
                {
                    suggestedFilename = m_currentAssemblyFileName.value();
                    // Strip .implicit or other compound extensions from stem
                    auto stem = suggestedFilename.stem();
                    while (stem.extension() == ".implicit" || stem.extension() == ".model")
                    {
                        stem = stem.stem();
                    }
                    suggestedFilename =
                      suggestedFilename.parent_path() / (stem.string() + ".model.3mf");
                }
                else
                {
                    suggestedFilename = "part.model.3mf";
                }
                m_meshExporterDialog.setDocument(m_doc.get());
                m_meshExporterDialog.show(suggestedFilename);
            }
            ImGui::EndDisabled();
        }
        else
        {
            ImGui::BeginDisabled();
            ImGui::TextDisabled("Compute is disabled: export functions are unavailable.");
            ImGui::EndDisabled();
        }

        ImGui::Separator();

        // Add Library Browser menu item
        if (ImGui::MenuItem(
              reinterpret_cast<const char *>(ICON_FA_FOLDER_OPEN "\tLibrary Browser")))
        {
            closeMenu();
            if (!m_modelEditor.isVisible())
            {
                m_modelEditor.setVisibility(true);
            }
            m_modelEditor.setLibraryRootDirectory(getUserLibraryDir());
            m_modelEditor.setLibraryVisibility(true);
            m_isLibraryBrowserVisible = true;
        }

        if (m_showSettings)
        {
            if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_COG "\tSettings")))
            {
                closeMenu();
                m_mainView.setViewSettingsVisible(true);
            }

            if (ImGui::MenuItem(
                  reinterpret_cast<const char *>(ICON_FA_KEYBOARD "\tKeyboard Shortcuts")))
            {
                closeMenu();
                showShortcutSettings();
            }

            if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_LIST "\tShow Log")))
            {
                closeMenu();
                m_logView.show();
            }
        }

        if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_QUESTION "\tAbout Gladius")))
        {
            closeMenu();
            m_about.show();
        }

        ImGui::Separator();
        if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_POWER_OFF "\tExit")))
        {
            closeMenu();
            close();
        }

        // Hide menu when anything else is clicked
        if (!ImGui::IsWindowFocused())
        {
            closeMenu();
        }

        ImGui::End();

        ImGui::PopStyleVar();

        // Animation
        const auto deltaTime = ImGui::GetIO().DeltaTime;
        m_mainMenuPosX -= m_mainMenuPosX * 20.f * deltaTime;
        m_mainMenuPosX = std::min(m_mainMenuPosX, 0.f);
        if (m_mainMenuPosX < 0.f)
        {
            m_mainView.startAnimationMode();
        }
        const auto window_pos = ImVec2(m_mainMenuPosX, menuBarHeight);
        ImGui::SetWindowPos("Menu", window_pos, ImGuiCond_Always);
    }

    void MainWindow::sliceWindow()
    {
        updateContours();
        m_isSlicePreviewVisible = m_sliceView.render(*m_core, m_mainView);

        // Process slice window shortcuts if visible and hovered
        if (m_isSlicePreviewVisible && m_sliceView.isHovered())
        {
            processShortcuts(ShortcutContext::SlicePreview);
        }
    }

    void MainWindow::meshExportDialog()
    {
        if (m_meshExporterDialog.isVisible())
        {
            m_mainView.startAnimationMode();
            m_renderWindow.invalidateView();
        }
        m_meshExporterDialog.render(*m_core);
    }

    void MainWindow::cliExportDialog()
    {
        if (m_cliExportDialog.isVisible())
        {
            m_mainView.startAnimationMode();
            m_renderWindow.invalidateView();
        }
        m_cliExportDialog.render(*m_core);
    }

    void MainWindow::import()
    {
        if (m_asyncFileDialog.isActive())
        {
            return;
        }
        m_asyncDialogOp = AsyncDialogOperation::Import;
        m_asyncFileDialog.openFile({{"*.3mf"}});
    }

    void MainWindow::updateContours()
    {
        if (!m_contoursDirty || !m_isSlicePreviewVisible)
        {
            return;
        }
        m_core->invalidateContourCache();
        m_contoursDirty = false;
    }

    void MainWindow::markFileAsChanged()
    {
        m_fileChanged = true;
    }

    void MainWindow::open()
    {
        if (!m_computeAvailable || !m_doc || m_asyncFileDialog.isActive())
        {
            if (m_logger && !m_asyncFileDialog.isActive())
            {
                m_logger->addEvent(
                  {"Open is unavailable: compute/renderer disabled", events::Severity::Warning});
            }
            return;
        }
        // Don't allow opening while loading is in progress
        if (m_doc->isLoadingInProgress())
        {
            return;
        }
        if (m_fileChanged)
        {
            m_pendingFileOperation = PendingFileOperation::OpenFile;
            m_pendingOpenFilename.reset();
            m_showSaveBeforeFileOperation = true;
            return;
        }

        m_asyncDialogOp = AsyncDialogOperation::Open;
        m_asyncFileDialog.openFile({{"*.3mf"}});
    }

    void MainWindow::merge()
    {
        if (!m_computeAvailable || !m_doc || m_asyncFileDialog.isActive())
        {
            if (m_logger && !m_asyncFileDialog.isActive())
            {
                m_logger->addEvent(
                  {"Merge is unavailable: compute/renderer disabled", events::Severity::Warning});
            }
            return;
        }
        // Don't allow merge while loading is in progress
        if (m_doc->isLoadingInProgress())
        {
            return;
        }
        m_asyncDialogOp = AsyncDialogOperation::Merge;
        m_asyncFileDialog.openFile({{"*.3mf"}});
    }

    void MainWindow::resetEditorState()
    {
        m_modelEditor.resetEditorContext();
        m_modelEditor.setDocument(m_doc);
        m_modelEditor.invalidatePrimitiveData();
        m_renderWindow.invalidateView();
        m_dirty = true;
        m_renderCallback();
        m_modelEditor.triggerNodePositionUpdate();
        m_fileChanged = false;
        m_modelEditor.resetUndo();
    }

    void MainWindow::open(const std::filesystem::path & filename)
    {
        if (!m_computeAvailable || !m_doc)
        {
            if (m_logger)
            {
                m_logger->addEvent(
                  {"Open is unavailable: compute/renderer disabled", events::Severity::Warning});
            }
            return;
        }
        // Don't allow opening while loading is in progress
        if (m_doc->isLoadingInProgress())
        {
            return;
        }
        if (m_fileChanged)
        {
            m_pendingFileOperation = PendingFileOperation::OpenFile;
            m_pendingOpenFilename = filename;
            m_showSaveBeforeFileOperation = true;
            return;
        }

        loadFileDeferred(filename);
    }

    void MainWindow::setStartupFile(std::filesystem::path filename)
    {
        m_startupFile = std::move(filename);
    }

    void MainWindow::loadFileDeferred(const std::filesystem::path & filename)
    {
        m_currentAssemblyFileName = filename;
        m_welcomeScreen.hide();

        // Cancel all in-flight async GPU work before loading.
        // refreshWorker() (on the loading thread) rebuilds CL programs and
        // resources.  Any async job still running on the worker thread would
        // access those resources concurrently, causing a segfault.
        m_renderWindow.cancelAllAsyncWork();

        // Defer editor reset until the new Assembly has been loaded.
        m_asyncLoadState = AsyncLoadState::LoadingWithReset;
        m_doc->loadNonBlocking(filename);

        // Add to recent files list
        addToRecentFiles(filename);
    }

    void MainWindow::startMainLoop()
    {
        m_mainView.startMainLoop();
    }

    void MainWindow::save()
    {
        // Allow saving even if compute is disabled; just skip thumbnail generation.
        if (!m_doc)
        {
            return;
        }
        if (m_currentAssemblyFileName->empty())
        {
            saveAs();
            return;
        }
        bool writeThumbnail = m_computeAvailable && m_core;
        m_doc->saveAs(m_currentAssemblyFileName.value(), writeThumbnail);
        m_renderWindow.invalidateViewDuetoModelUpdate();
        m_fileChanged = false;

        // Add to recent files list
        addToRecentFiles(m_currentAssemblyFileName.value());
    }

    void MainWindow::saveAs()
    {
        // Allow saving even if compute is disabled; just skip thumbnail generation.
        if (!m_doc || m_asyncFileDialog.isActive())
        {
            return;
        }
        m_asyncDialogOp = AsyncDialogOperation::SaveAs;
        m_asyncFileDialog.saveFile({"*.implicit.3mf"},
                                   m_currentAssemblyFileName.value_or(std::filesystem::path{}));
    }

    void MainWindow::saveCurrentFunction()
    {
        if (!m_computeAvailable || !m_doc || m_asyncFileDialog.isActive())
        {
            if (m_logger && !m_asyncFileDialog.isActive())
            {
                m_logger->addEvent(
                  {"Save Current Function is unavailable: compute/renderer disabled",
                   events::Severity::Warning});
            }
            return;
        }
        auto function = m_modelEditor.currentModel();
        if (!function)
        {
            return;
        }

        m_asyncDialogOp = AsyncDialogOperation::SaveCurrentFunction;
        m_asyncFileDialog.saveFile({"*.3mf"},
                                   m_currentAssemblyFileName.value_or(std::filesystem::path{}));
    }

    void MainWindow::importImageStack()
    {
        if (!m_computeAvailable || !m_doc || m_asyncFileDialog.isActive())
        {
            if (m_logger && !m_asyncFileDialog.isActive())
            {
                m_logger->addEvent({"Import Image Stack is unavailable: compute/renderer disabled",
                                    events::Severity::Warning});
            }
            return;
        }
        m_asyncDialogOp = AsyncDialogOperation::ImportImageStack;
        m_asyncFileDialog.selectDirectory();
    }

    void MainWindow::onPreviewProgramSwap()
    {
        m_parameterDirty = true;
        m_contoursDirty = true;
        m_dirty = true;
        m_moving = true;
        m_doc->updateParameter();
        m_renderWindow.invalidateViewDuetoModelUpdate();
        m_renderWindow.updateCamera();
    }

    void MainWindow::close()
    {
        saveRenderSettings();

        // Block close if export is in progress
        if (m_exportState.isExportInProgress())
        {
            m_showExportInProgressWarning = true;
            return;
        }

        // Gracefully stop any ongoing compilations before exit
        auto & programManager = m_core->getProgramManager();
        if (programManager.isAnyCompilationInProgressNonBlocking())
        {
            programManager.requestShutdownAll();
            programManager.waitForAllCompilations();
        }

        if (m_fileChanged)
        {
            m_showSaveBeforeExit = true;
            return;
        }
        exit(EXIT_SUCCESS);
    }

    void MainWindow::showExitPopUp()
    {
        if (!m_showSaveBeforeExit)
        {
            return;
        }

        auto constexpr windowTitle = "Do you want to save before leaving Gladius?";
        if (!ImGui::IsPopupOpen(windowTitle))
        {
            ImGui::OpenPopup(windowTitle);
        }
        const ImGuiWindowFlags windowFlags =
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::BeginPopupModal(windowTitle, nullptr, windowFlags))
        {
            ImGui::NewLine();
            ImGui::NewLine();

            if (m_currentAssemblyFileName)
            {
                ImGui::TextUnformatted(
                  fmt::format("{} \nhas changed. \nDo you want to save before leaving?",
                              m_currentAssemblyFileName.value().string())
                    .c_str());

                ImGui::NewLine();
                ImGui::NewLine();

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.f, 0.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 0.f, 0.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.f, 0.f, 1.f));
                if (ImGui::Button(
                      reinterpret_cast<const char *>(ICON_FA_POWER_OFF "\tLeave without saving")))
                {
                    std::exit(EXIT_SUCCESS);
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();
                if (ImGui::Button("Continue working"))
                {
                    m_showSaveBeforeExit = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(reinterpret_cast<const char *>(ICON_FA_SAVE "\tSave")))
                {
                    save();
                    m_showSaveBeforeExit = false;
                    std::exit(EXIT_SUCCESS);
                }
                ImGui::SameLine();
                if (ImGui::Button(reinterpret_cast<const char *>(ICON_FA_SAVE "\tSave As")))
                {
                    saveAs();
                    m_showSaveBeforeExit = false;
                    std::exit(EXIT_SUCCESS);
                }
            }
            else
            {
                ImGui::TextUnformatted("The current assembly has not been saved yet. \nDo you want "
                                       "to save before leaving?");

                ImGui::NewLine();
                ImGui::NewLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.f, 0.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 0.f, 0.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.f, 0.f, 1.f));
                if (ImGui::Button(
                      reinterpret_cast<const char *>(ICON_FA_POWER_OFF "\tLeave without saving")))
                {
                    std::exit(EXIT_SUCCESS);
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();
                if (ImGui::Button("Continue working"))
                {
                    m_showSaveBeforeExit = false;
                }
                ImGui::SameLine();
                if (ImGui::Button(reinterpret_cast<const char *>(ICON_FA_SAVE "\tSave As")))
                {
                    saveAs();
                    m_showSaveBeforeExit = false;
                    std::exit(EXIT_SUCCESS);
                }
            }
            ImGui::EndPopup();
        }
    }

    void MainWindow::showExportInProgressWarning()
    {
        if (!m_showExportInProgressWarning)
        {
            return;
        }

        auto constexpr windowTitle = "Export in Progress";
        if (!ImGui::IsPopupOpen(windowTitle))
        {
            ImGui::OpenPopup(windowTitle);
        }
        ImGuiWindowFlags const windowFlags =
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::BeginPopupModal(windowTitle, nullptr, windowFlags))
        {
            ImGui::NewLine();
            ImGui::TextUnformatted("An export operation is currently in progress.");
            ImGui::TextUnformatted("Please wait for it to complete before closing the application.");
            ImGui::NewLine();

            if (ImGui::Button("OK"))
            {
                m_showExportInProgressWarning = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void MainWindow::renderExportOverlay()
    {
        if (!m_exportState.isExportInProgress())
        {
            return;
        }

        // Get the entire viewport size
        ImGuiViewport const * viewport = ImGui::GetMainViewport();
        ImVec2 const viewportPos = viewport->Pos;
        ImVec2 const viewportSize = viewport->Size;

        // Create a fullscreen overlay window
        ImGui::SetNextWindowPos(viewportPos);
        ImGui::SetNextWindowSize(viewportSize);

        ImGuiWindowFlags const overlayFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.6f));

        if (ImGui::Begin("##ExportOverlay", nullptr, overlayFlags))
        {
            // Centered message using window draw list
            ImDrawList * drawList = ImGui::GetWindowDrawList();
            char const * message = "Export in progress...";
            ImVec2 const textSize = ImGui::CalcTextSize(message);
            ImVec2 const textPos(viewportPos.x + (viewportSize.x - textSize.x) / 2.0f,
                                 viewportPos.y + (viewportSize.y - textSize.y) / 2.0f);
            drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), message);

            // Invisible button to capture clicks and redirect focus to export dialog
            ImGui::SetCursorPos(ImVec2(0, 0));
            if (ImGui::InvisibleButton("##ExportOverlayBlocker", viewportSize))
            {
                // When clicked, refocus the visible export dialog
                // Try all known export dialog window titles - SetWindowFocus is a no-op
                // if the window doesn't exist
                ImGui::SetWindowFocus("Exporting STL");
                ImGui::SetWindowFocus("Exporting 3MF");
                ImGui::SetWindowFocus("Export Mesh");
                ImGui::SetWindowFocus("Export in progress");
            }
        }
        ImGui::End();

        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    void MainWindow::renderStatusBar()
    {
        if (!m_logger)
        {
            return;
        }

        // Create status bar at the bottom of the main window
        ImGuiViewport const * viewport = ImGui::GetMainViewport();
        ImVec2 const statusBarPos =
          ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - ImGui::GetFrameHeight());
        ImVec2 const statusBarSize = ImVec2(viewport->Size.x, ImGui::GetFrameHeight());

        ImGui::SetNextWindowPos(statusBarPos);
        ImGui::SetNextWindowSize(statusBarSize);

        ImGuiWindowFlags const statusBarFlags =
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
          ImGuiWindowFlags_NoNavInputs;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));

        if (ImGui::Begin("##StatusBar", nullptr, statusBarFlags))
        {
            size_t const errorCount = m_logger->getErrorCount();
            size_t const warningCount = m_logger->getWarningCount();

            // Error indicator - clickable to show log view
            if (errorCount > 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 0.2f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.3f, 0.3f, 0.4f));
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.6f, 0.6f, 0.2f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 0.4f));
            }

            std::string const errorText = fmt::format("{} {} {}",
                                                      ICON_FA_EXCLAMATION_TRIANGLE,
                                                      errorCount,
                                                      errorCount == 1 ? "Error" : "Errors");

            if (ImGui::Button(errorText.c_str()))
            {
                m_logView.show();
            }
            ImGui::PopStyleColor(4);

            ImGui::SameLine();

            // Warning indicator - clickable to show log view
            if (warningCount > 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.7f, 0.3f, 0.2f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.7f, 0.3f, 0.4f));
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.6f, 0.6f, 0.2f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 0.4f));
            }

            std::string const warningText = fmt::format("{} {} {}",
                                                        ICON_FA_EXCLAMATION_CIRCLE,
                                                        warningCount,
                                                        warningCount == 1 ? "Warning" : "Warnings");

            if (ImGui::Button(warningText.c_str()))
            {
                m_logView.show();
            }
            ImGui::PopStyleColor(4);

            // Show UI scale, FPS, rendering mode, and compute status on the right
            ImGui::SameLine();
            
            // Get rendering mode info from ComputeCore
            // Show preview mode while camera is moving, HQ mode when still
            char const * renderModeStr = "N/A";
            char const * sdfStatusStr = "";
            if (m_core)
            {
                bool const cameraMoving = m_renderWindow.isCameraMoving();
                auto const approx = cameraMoving 
                    ? m_core->getLastUsedPreviewApproximation()
                    : m_core->getLastUsedHQApproximation();
                bool const sdfValid = m_core->isSdfValid();
                
                if (approx & AM_FULL_MODEL)
                {
                    renderModeStr = "FULL";
                }
                else if (approx & AM_ONLY_PRECOMPSDF)
                {
                    renderModeStr = "TEX3D";
                }
                else if (approx & AM_HYBRID)
                {
                    renderModeStr = "HYB";
                }
                else
                {
                    renderModeStr = "?";
                }
                
                // Show SDF status when moving: indicates SDF is still computing
                sdfStatusStr = (cameraMoving && !sdfValid) ? " (SDF...)" : "";
            }
            
            // Bounding box dimensions
            if (m_core)
            {
                auto const bb = m_core->getBoundingBox();
                if (bb.has_value())
                {
                    ImGui::Text("%s %.3f x %.3f x %.3f mm",
                                ICON_FA_CUBE,
                                bb->max.x - bb->min.x,
                                bb->max.y - bb->min.y,
                                bb->max.z - bb->min.z);
                    ImGui::SameLine();
                }
            }

            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 400.0f);
            ImGui::Text("%.0f FPS | %s%s",
                        ImGui::GetIO().Framerate,
                        renderModeStr,
                        sdfStatusStr);

            if (!m_computeAvailable)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Compute: disabled"))
                {
                    m_showComputeErrorModal = true;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("OpenCL/compute unavailable. Click for details.");
                    if (!m_computeErrorMessage.empty())
                    {
                        ImGui::Separator();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 50.0f);
                        ImGui::TextWrapped("%s", m_computeErrorMessage.c_str());
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::End();

        ImGui::PopStyleVar(3);
    }

    void MainWindow::renderComputeErrorModal()
    {
        // Compute error details modal - rendered independently of other UI elements
        // to ensure it's always visible when OpenCL initialization fails
        if (m_showComputeErrorModal)
        {
            if (!ImGui::IsPopupOpen("OpenCL/Compute Unavailable"))
            {
                ImGui::OpenPopup("OpenCL/Compute Unavailable");
            }
            if (ImGui::BeginPopupModal("OpenCL/Compute Unavailable",
                                       &m_showComputeErrorModal,
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoSavedSettings))
            {
                ImGui::TextWrapped("Gladius couldn't initialize OpenCL. The UI stays usable, but "
                                   "rendering and slicing are disabled.");
                ImGui::Separator();

                if (!m_computeErrorMessage.empty())
                {
                    ImGui::TextUnformatted("Error Details:");
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                    ImGui::BeginChild("##oclErrDetails", ImVec2(600, 160), true);
                    ImGui::PushTextWrapPos();
                    ImGui::TextWrapped("%s", m_computeErrorMessage.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndChild();
                    ImGui::PopStyleColor(2);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Provide helpful troubleshooting guidance
                ImGui::TextWrapped("Common solutions:");
                ImGui::BulletText("Install OpenCL drivers for your GPU (NVIDIA, AMD, or Intel)");
                ImGui::BulletText(
                  "On Linux: Install ocl-icd-opencl-dev and vendor-specific drivers");
                ImGui::BulletText("Check if other OpenCL applications work (e.g., clinfo)");
                ImGui::BulletText("Restart the application after installing drivers");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Retry Initialization"))
                {
                    try
                    {
                        auto const context =
                          std::make_shared<ComputeContext>(EnableGLOutput::enabled);
                        context->setLogger(m_logger);
                        gladius::setGlobalLogger(m_logger);
                        context->setDebugOutputEnabled(m_openclDebugEnabled);
                        if (!context->isValid())
                        {
                            throw OpenCLContextCreationError("Context invalid after retry");
                        }
                        if (!m_core)
                        {
                            m_core = std::make_shared<ComputeCore>(
                              context, RequiredCapabilities::OpenGLInterop, m_logger);
                            m_doc = std::make_shared<Document>(m_core);

                            // If retry successful, complete the full setup
                            setup(m_core, m_doc, m_logger);
                        }
                        else
                        {
                            m_core->setComputeContext(context);
                        }
                        m_computeAvailable = true;
                        m_computeErrorMessage.clear();
                        m_showComputeErrorModal = false;
                        loadRenderSettings();

                        if (m_logger)
                        {
                            m_logger->addEvent(
                              {"OpenCL initialized successfully!", events::Severity::Info});
                        }
                    }
                    catch (const std::exception & e)
                    {
                        m_computeAvailable = false;
                        m_computeErrorMessage = e.what();

                        if (m_logger)
                        {
                            m_logger->addEvent(
                              {std::string("Retry failed: ") + e.what(), events::Severity::Error});
                        }
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("Copy Details"))
                {
                    std::string details = "Gladius OpenCL Initialization Error\n\n";
                    details += m_computeErrorMessage.empty() ? std::string("No details available")
                                                             : m_computeErrorMessage;
                    ImGui::SetClipboardText(details.c_str());
                }

                ImGui::SameLine();
                if (ImGui::Button("Continue Without OpenCL"))
                {
                    m_showComputeErrorModal = false;
                    // Show welcome screen again so user can browse examples or documentation
                    m_welcomeScreen.show();
                }

                ImGui::EndPopup();
            }
        }
    }

    void MainWindow::showSaveBeforeFileOperationPopUp()
    {
        if (!m_showSaveBeforeFileOperation)
        {
            return;
        }

        // If async dialog is active, wait for result in processAsyncFileDialog
        bool const dialogActive = m_asyncFileDialog.isActive();

        auto constexpr windowTitle = "Do you want to save before continuing?";
        if (!ImGui::IsPopupOpen(windowTitle))
        {
            ImGui::OpenPopup(windowTitle);
        }
        const ImGuiWindowFlags windowFlags =
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::BeginPopupModal(windowTitle, nullptr, windowFlags))
        {
            ImGui::NewLine();
            ImGui::NewLine();

            // Lambda to handle the "open after" action - starts async dialog or uses pending
            // filename
            auto handleOpenAfterAction = [this, dialogActive]() -> bool
            {
                if (m_pendingFileOperation == PendingFileOperation::NewModel)
                {
                    m_doc->newFromTemplate();
                    resetEditorState();
                    m_modelFileName.clear();
                    m_currentAssemblyFileName.reset();
                    m_renderWindow.centerView();
                    return true; // Action complete
                }
                else if (m_pendingFileOperation == PendingFileOperation::OpenFile)
                {
                    if (m_pendingOpenFilename.has_value())
                    {
                        // Direct file open (e.g., from recent files)
                        loadFileDeferred(m_pendingOpenFilename.value());
                        return true;
                    }
                    else if (!dialogActive)
                    {
                        // Start async file dialog
                        m_asyncDialogOp = AsyncDialogOperation::OpenAfterSavePrompt;
                        m_asyncFileDialog.openFile({{"*.3mf"}});
                        return false; // Wait for dialog
                    }
                    return false; // Dialog in progress
                }
                return true;
            };

            if (m_currentAssemblyFileName)
            {
                const char * operationText =
                  (m_pendingFileOperation == PendingFileOperation::NewModel)
                    ? "creating a new model"
                    : "opening a file";

                ImGui::TextUnformatted(
                  fmt::format("{} \nhas changed. \nDo you want to save before {}?",
                              m_currentAssemblyFileName.value().string(),
                              operationText)
                    .c_str());

                ImGui::NewLine();
                ImGui::NewLine();

                // Disable buttons while dialog is active
                ImGui::BeginDisabled(dialogActive);

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.f, 0.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 0.f, 0.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.f, 0.f, 1.f));
                if (ImGui::Button(
                      reinterpret_cast<const char *>(ICON_FA_TIMES "\tContinue without saving")))
                {
                    if (handleOpenAfterAction())
                    {
                        m_showSaveBeforeFileOperation = false;
                        m_pendingFileOperation = PendingFileOperation::None;
                        m_pendingOpenFilename.reset();
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    m_showSaveBeforeFileOperation = false;
                    m_pendingFileOperation = PendingFileOperation::None;
                    m_pendingOpenFilename.reset();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(reinterpret_cast<const char *>(ICON_FA_SAVE "\tSave")))
                {
                    save();
                    if (handleOpenAfterAction())
                    {
                        m_showSaveBeforeFileOperation = false;
                        m_pendingFileOperation = PendingFileOperation::None;
                        m_pendingOpenFilename.reset();
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(reinterpret_cast<const char *>(ICON_FA_SAVE "\tSave As")))
                {
                    // Note: saveAs() is now async, so we can't wait for it
                    // For now, save synchronously for this popup flow
                    // This might need refinement later if saveAs truly needs to complete first
                    saveAs();
                    if (handleOpenAfterAction())
                    {
                        m_showSaveBeforeFileOperation = false;
                        m_pendingFileOperation = PendingFileOperation::None;
                        m_pendingOpenFilename.reset();
                        ImGui::CloseCurrentPopup();
                    }
                }

                ImGui::EndDisabled();
            }
            else
            {
                const char * operationText =
                  (m_pendingFileOperation == PendingFileOperation::NewModel)
                    ? "creating a new model"
                    : "opening a file";

                ImGui::TextUnformatted(
                  fmt::format(
                    "The current assembly has not been saved yet. \nDo you want to save before {}?",
                    operationText)
                    .c_str());

                ImGui::NewLine();
                ImGui::NewLine();

                // Disable buttons while dialog is active
                ImGui::BeginDisabled(dialogActive);

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.f, 0.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 0.f, 0.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.f, 0.f, 1.f));
                if (ImGui::Button(
                      reinterpret_cast<const char *>(ICON_FA_TIMES "\tContinue without saving")))
                {
                    if (handleOpenAfterAction())
                    {
                        m_showSaveBeforeFileOperation = false;
                        m_pendingFileOperation = PendingFileOperation::None;
                        m_pendingOpenFilename.reset();
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    m_showSaveBeforeFileOperation = false;
                    m_pendingFileOperation = PendingFileOperation::None;
                    m_pendingOpenFilename.reset();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(reinterpret_cast<const char *>(ICON_FA_SAVE "\tSave As")))
                {
                    // Note: saveAs() is now async, so we can't wait for it
                    // For now, save synchronously for this popup flow
                    saveAs();
                    if (handleOpenAfterAction())
                    {
                        m_showSaveBeforeFileOperation = false;
                        m_pendingFileOperation = PendingFileOperation::None;
                        m_pendingOpenFilename.reset();
                        ImGui::CloseCurrentPopup();
                    }
                }

                ImGui::EndDisabled();
            }
            ImGui::EndPopup();
        }
    }

    void MainWindow::logViewer()
    {
        if (!m_logger)
        {
            return;
        }
        m_logView.render(*m_logger);
    }

    void MainWindow::updateModel()
    {
        // Avoid touching document/compute state while a file is being loaded on a background thread.
        if (m_doc && m_doc->isLoadingInProgress())
        {
            return;
        }

        // Dispatch debounced structural edits to the background worker.
        if (m_doc && m_doc->dispatchStructuralUpdateIfReady())
        {
            // A compilation was launched — mirror the side-effects of refreshModel().
            if (m_logger)
            {
                m_logger->clear();
            }
            m_renderWindow.invalidateViewDuetoModelUpdate();
            m_renderWindow.invalidateView();
        }

        // Suppress HQ front-buffer display when a parameter change is pending.
        // This runs before renderWindow() (in displayUI()) so the stale HQ image
        // (with shadows) is not shown while the parameter push is in progress.
        // Unlike invalidateView(), this does not bump the epoch or disrupt
        // in-flight SDF/preview work.
        if (m_parameterDirty)
        {
            m_renderWindow.suppressHQDisplay();
        }

        auto const timeSinceLastUpdate = std::chrono::steady_clock::now() - m_lastUpateTime;
        auto const rateLimit = std::chrono::milliseconds(static_cast<int>(ImGui::GetIO().DeltaTime * 5000.f));
        
        // For parameter changes, use a shorter rate limit for more responsive updates
        bool const hasParameterChange = m_parameterDirty;
        auto const effectiveRateLimit = hasParameterChange ? std::chrono::milliseconds(16) : rateLimit;

        if (timeSinceLastUpdate < effectiveRateLimit)
        {
            return;
        }

        m_lastUpateTime = std::chrono::steady_clock::now();

        // Skip if nothing to do, but allow parameter updates even during rendering
        if (!hasParameterChange && (!(m_dirty || m_contoursDirty) || m_renderWindow.isRenderingInProgress() ||
            !m_core->isRendererReady()))
        {
            return;
        }
        
        // For non-parameter updates, still check rendering state
        if (!hasParameterChange && !m_core->isRendererReady())
        {
            return;
        }

        if (m_modelEditor.primitveDataNeedsUpdate())
        {
            m_doc->invalidatePrimitiveData();
            m_modelEditor.markPrimitiveDataAsUpToDate();
        }

        if (m_modelEditor.isCompileRequested() && m_core->isRendererReady())
        {
            // Route manual compile requests through the debouncer instead of
            // calling refreshModel() directly, which would bypass the async
            // structural-edit pipeline and block the UI thread.
            m_doc->signalStructuralEdit();
            m_modelEditor.markModelAsUpToDate();
        }

        if (m_core->getSlicerProgram()->isCompilationInProgress() ||
            m_modelEditor.isCompileRequested() || !m_core->isRendererReady())
        {
            return;
        }

        if (m_parameterDirty)
        {
            // T047: Only update parameters when throttle allows (debounce interval expired).
            // When the throttle has no pending recompile it means it already fired but the
            // previous push failed (compute lock held by background worker).  In that case
            // bypass the throttle — the debounce already elapsed, we just need to retry.
            if (m_parameterThrottle.shouldRecompile() || !m_parameterThrottle.hasPendingRecompile())
            {
                // Streaming preview mode: the worker coroutine becomes the sole GPU
                // writer, reading the latest Assembly values and pushing them to the
                // parameter buffer in a tight loop.  This eliminates the UI-frame
                // round-trip latency that limits one-shot preview scheduling.
                if (!m_renderWindow.isStreamingPreviewActive())
                {
                    m_renderWindow.invalidateViewDueToParameterChange();
                    m_renderWindow.startStreamingPreview();
                }
                m_parameterDirty = false;
                m_contoursDirty = true;
            }
        }
        updateContours();
    }

    /**
     * @brief Add a file to the list of recently modified files
     * @param filePath Path to the file that has been modified
     */
    void MainWindow::addToRecentFiles(const std::filesystem::path & filePath)
    {
        if (m_recentFilesManager)
        {
            m_recentFilesManager->addFile(filePath);
        }
    }

    /**
     * @brief Get the list of recently modified files
     * @param maxCount Maximum number of files to return
     * @return List of pairs containing file paths and timestamps
     */
    std::vector<std::pair<std::filesystem::path, std::time_t>>
    MainWindow::getRecentFiles(size_t maxCount) const
    {
        if (m_recentFilesManager)
        {
            return m_recentFilesManager->getFiles(maxCount);
        }
        return {};
    }

    void MainWindow::initializeShortcuts()
    {
        if (!m_configManager)
        {
            return;
        }

        // Create shortcut manager
        m_shortcutManager = std::make_shared<ShortcutManager>(
          std::shared_ptr<ConfigManager>(m_configManager, [](ConfigManager *) {}));
        m_shortcutSettingsDialog.setShortcutManager(m_shortcutManager);

        // Register global shortcuts
        m_shortcutManager->registerAction("file.new",
                                          "New",
                                          "Create a new model",
                                          ShortcutContext::Global,
                                          ShortcutCombo(ImGuiKey_N, true), // Ctrl+N
                                          [this]() { newModel(); });

        m_shortcutManager->registerAction("file.open",
                                          "Open",
                                          "Open an existing model",
                                          ShortcutContext::Global,
                                          ShortcutCombo(ImGuiKey_O, true), // Ctrl+O
                                          [this]() { open(); });

        m_shortcutManager->registerAction("file.save",
                                          "Save",
                                          "Save the current model",
                                          ShortcutContext::Global,
                                          ShortcutCombo(ImGuiKey_S, true), // Ctrl+S
                                          [this]() { save(); });

        m_shortcutManager->registerAction(
          "file.saveAs",
          "Save As",
          "Save the current model with a new name",
          ShortcutContext::Global,
          ShortcutCombo(ImGuiKey_S, true, false, true), // Ctrl+Shift+S
          [this]() { saveAs(); });

        m_shortcutManager->registerAction(
          "edit.library",
          "Toggle Library Browser",
          "Show or hide the library browser",
          ShortcutContext::Global,
          ShortcutCombo(ImGuiKey_B, true), // Ctrl+B
          [this]()
          {
              if (!m_modelEditor.isVisible())
              {
                  m_modelEditor.setVisibility(true);
              }
              m_modelEditor.setLibraryRootDirectory(getUserLibraryDir());
              m_modelEditor.toggleLibraryVisibility();
              m_isLibraryBrowserVisible = m_modelEditor.isLibraryVisible();
          });

        m_shortcutManager->registerAction("view.resetView",
                                          "Reset View",
                                          "Reset the camera view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_R), // R
                                          [this]() { m_renderWindow.centerView(); });

        // Model editor shortcuts
        m_shortcutManager->registerAction("edit.undo",
                                          "Undo",
                                          "Undo the last action",
                                          ShortcutContext::ModelEditor,
                                          ShortcutCombo(ImGuiKey_Z, true), // Ctrl+Z
                                          [this]()
                                          {
                                              // Handle this in the UI code since undo/redo are
                                              // private
                                              if (ImGui::GetIO().KeyCtrl &&
                                                  ImGui::IsKeyPressed(ImGuiKey_Z, false))
                                              {
                                                  ed::NavigateToContent();
                                              }
                                          });

        m_shortcutManager->registerAction("edit.redo",
                                          "Redo",
                                          "Redo the last undone action",
                                          ShortcutContext::ModelEditor,
                                          ShortcutCombo(ImGuiKey_Y, true), // Ctrl+Y
                                          [this]()
                                          {
                                              // Handle this in the UI code since undo/redo are
                                              // private
                                              if (ImGui::GetIO().KeyCtrl &&
                                                  ImGui::IsKeyPressed(ImGuiKey_Y, false))
                                              {
                                                  ed::NavigateToContent();
                                              }
                                          });

        m_shortcutManager->registerAction("edit.compile",
                                          "Compile Model",
                                          "Compile the current model",
                                          ShortcutContext::ModelEditor,
                                          ShortcutCombo(ImGuiKey_F5), // F5
                                          [this]()
                                          {
                                              // The model editor will handle compilation based on
                                              // the F5 key press We'll let the native ImGui input
                                              // handling take care of this
                                          });

        // Add shortcut for showing settings dialog
        m_shortcutManager->registerAction("view.shortcuts",
                                          "Keyboard Shortcuts",
                                          "Show keyboard shortcuts dialog",
                                          ShortcutContext::Global,
                                          ShortcutCombo(ImGuiKey_K, true), // Ctrl+K
                                          [this]() { showShortcutSettings(); });

        m_shortcutManager->registerAction("view.uiScaleIncrease",
                                          "Increase UI Scale",
                                          "Increase UI scaling (Ctrl +)",
                                          ShortcutContext::Global,
                                          ShortcutCombo(ImGuiKey_Equal, true), // Ctrl +/=
                                          [this]() { m_mainView.adjustUserScale(1.10f); });

        m_shortcutManager->registerAction("view.uiScaleDecrease",
                                          "Decrease UI Scale",
                                          "Decrease UI scaling (Ctrl -)",
                                          ShortcutContext::Global,
                                          ShortcutCombo(ImGuiKey_Minus, true), // Ctrl -
                                          [this]() { m_mainView.adjustUserScale(1.0f / 1.10f); });

        m_shortcutManager->registerAction(
          "view.uiScaleReset",
          "Reset UI Scale",
          "Reset UI scaling (Ctrl+Shift+0)",
          ShortcutContext::Global,
          ShortcutCombo(ImGuiKey_0, true, false, true), // Ctrl+Shift+0
          [this]() { m_mainView.resetUserScale(); });

        // Model editor shortcuts - Compile implicit function
        m_shortcutManager->registerAction("model.compileImplicit",
                                          "Compile Implicit Function",
                                          "Manually compile the implicit function",
                                          ShortcutContext::ModelEditor,
                                          ShortcutCombo(ImGuiKey_F7), // F7
                                          [this]() { m_modelEditor.requestManualCompile(); });

        // Model editor: Copy / Paste
        m_shortcutManager->registerAction("model.copy",
                                          "Copy Nodes",
                                          "Copy selected nodes to clipboard",
                                          ShortcutContext::ModelEditor,
                                          ShortcutCombo(ImGuiKey_C, true), // Ctrl+C
                                          [this]()
                                          {
                                              if (m_modelEditor.isHovered())
                                              {
                                                  // Delegate to editor, which reads current
                                                  // selection Note: copySelectionToClipboard is
                                                  // private; use keyboard in editor
                                              }
                                          });
        m_shortcutManager->registerAction("model.paste",
                                          "Paste Nodes",
                                          "Paste nodes from clipboard",
                                          ShortcutContext::ModelEditor,
                                          ShortcutCombo(ImGuiKey_V, true), // Ctrl+V
                                          [this]()
                                          {
                                              if (m_modelEditor.isHovered())
                                              {
                                                  // Editor handles Ctrl+V; we keep registration for
                                                  // UI visibility
                                              }
                                          });

        // Model editor: History navigation (Back/Forward)
        m_shortcutManager->registerAction(
          "model.historyBack",
          "Navigate Back",
          "Go back to the previously viewed function",
          ShortcutContext::ModelEditor,
          ShortcutCombo(ImGuiKey_LeftArrow, false, true, false), // Alt+Left
          [this]()
          {
              if (m_modelEditor.isHovered())
              {
                  m_modelEditor.goBack();
              }
          });

        m_shortcutManager->registerAction(
          "model.historyForward",
          "Navigate Forward",
          "Go forward to the next viewed function",
          ShortcutContext::ModelEditor,
          ShortcutCombo(ImGuiKey_RightArrow, false, true, false), // Alt+Right
          [this]()
          {
              if (m_modelEditor.isHovered())
              {
                  m_modelEditor.goForward();
              }
          });

        // Standard CAD view shortcuts for RenderWindow
        // Based on industry standards (Blender, 3ds Max, Maya, AutoCAD, SolidWorks)

        // Basic view controls
        m_shortcutManager->registerAction(
          "camera.centerView",
          "Center View",
          "Center the camera view on the model",
          ShortcutContext::RenderWindow,
          ShortcutCombo(ImGuiKey_Period), // . (standard in many CAD apps)
          [this]() { m_renderWindow.centerView(); });

        m_shortcutManager->registerAction(
          "camera.togglePermanentCentering",
          "Toggle Permanent Centering",
          "Toggle automatic view centering when model or camera changes",
          ShortcutContext::RenderWindow,
          ShortcutCombo(ImGuiKey_Period, true), // Ctrl+. for permanent centering
          [this]() { m_renderWindow.togglePermanentCentering(); });

        m_shortcutManager->registerAction("camera.frameAll",
                                          "Frame All",
                                          "Frame all objects in view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Home), // Home key
                                          [this]() { m_renderWindow.frameAll(); });

        // Orthographic views (Numpad standard)
        m_shortcutManager->registerAction("camera.frontView",
                                          "Front View",
                                          "Set camera to front view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad1), // Numpad 1
                                          [this]() { m_renderWindow.setFrontView(); });

        m_shortcutManager->registerAction("camera.backView",
                                          "Back View",
                                          "Set camera to back view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad1, true), // Ctrl+Numpad 1
                                          [this]() { m_renderWindow.setBackView(); });

        m_shortcutManager->registerAction("camera.rightView",
                                          "Right View",
                                          "Set camera to right view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad3), // Numpad 3
                                          [this]() { m_renderWindow.setRightView(); });

        m_shortcutManager->registerAction("camera.leftView",
                                          "Left View",
                                          "Set camera to left view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad3, true), // Ctrl+Numpad 3
                                          [this]() { m_renderWindow.setLeftView(); });

        m_shortcutManager->registerAction("camera.topView",
                                          "Top View",
                                          "Set camera to top view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad7), // Numpad 7
                                          [this]() { m_renderWindow.setTopView(); });

        m_shortcutManager->registerAction("camera.bottomView",
                                          "Bottom View",
                                          "Set camera to bottom view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad7, true), // Ctrl+Numpad 7
                                          [this]() { m_renderWindow.setBottomView(); });

        // Isometric and perspective views
        m_shortcutManager->registerAction("camera.isoView",
                                          "Isometric View",
                                          "Set camera to isometric view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad0), // Numpad 0
                                          [this]() { m_renderWindow.setIsometricView(); });

        m_shortcutManager->registerAction("camera.perspectiveToggle",
                                          "Toggle Perspective/Orthographic",
                                          "Toggle between perspective and orthographic projection",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad5), // Numpad 5
                                          [this]() { m_renderWindow.togglePerspective(); });

        // Alternative view shortcuts for keyboards without numpad
        m_shortcutManager->registerAction("camera.frontViewAlt",
                                          "Front View (Alt)",
                                          "Set camera to front view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_1), // 1
                                          [this]() { m_renderWindow.setFrontView(); });

        m_shortcutManager->registerAction("camera.rightViewAlt",
                                          "Right View (Alt)",
                                          "Set camera to right view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_3), // 3
                                          [this]() { m_renderWindow.setRightView(); });

        m_shortcutManager->registerAction("camera.topViewAlt",
                                          "Top View (Alt)",
                                          "Set camera to top view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_7), // 7
                                          [this]() { m_renderWindow.setTopView(); });

        m_shortcutManager->registerAction("camera.backViewAlt",
                                          "Back View (Alt)",
                                          "Set camera to back view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_1, true), // Ctrl+1
                                          [this]() { m_renderWindow.setBackView(); });

        m_shortcutManager->registerAction("camera.leftViewAlt",
                                          "Left View (Alt)",
                                          "Set camera to left view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_3, true), // Ctrl+3
                                          [this]() { m_renderWindow.setLeftView(); });

        m_shortcutManager->registerAction("camera.bottomViewAlt",
                                          "Bottom View (Alt)",
                                          "Set camera to bottom view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_7, true), // Ctrl+7
                                          [this]() { m_renderWindow.setBottomView(); });

        // Camera movement shortcuts
        m_shortcutManager->registerAction("camera.panLeft",
                                          "Pan Left",
                                          "Pan camera to the left",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad4), // Numpad 4
                                          [this]() { m_renderWindow.panLeft(); });

        m_shortcutManager->registerAction("camera.panRight",
                                          "Pan Right",
                                          "Pan camera to the right",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad6), // Numpad 6
                                          [this]() { m_renderWindow.panRight(); });

        m_shortcutManager->registerAction("camera.panUp",
                                          "Pan Up",
                                          "Pan camera up",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad8), // Numpad 8
                                          [this]() { m_renderWindow.panUp(); });

        m_shortcutManager->registerAction("camera.panDown",
                                          "Pan Down",
                                          "Pan camera down",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Keypad2), // Numpad 2
                                          [this]() { m_renderWindow.panDown(); });

        // Rotation shortcuts
        m_shortcutManager->registerAction(
          "camera.rotateLeft",
          "Rotate Left",
          "Rotate camera to the left",
          ShortcutContext::RenderWindow,
          ShortcutCombo(ImGuiKey_Keypad4, false, true), // Shift+Numpad 4
          [this]() { m_renderWindow.rotateLeft(); });

        m_shortcutManager->registerAction(
          "camera.rotateRight",
          "Rotate Right",
          "Rotate camera to the right",
          ShortcutContext::RenderWindow,
          ShortcutCombo(ImGuiKey_Keypad6, false, true), // Shift+Numpad 6
          [this]() { m_renderWindow.rotateRight(); });

        m_shortcutManager->registerAction(
          "camera.rotateUp",
          "Rotate Up",
          "Rotate camera up",
          ShortcutContext::RenderWindow,
          ShortcutCombo(ImGuiKey_Keypad8, false, true), // Shift+Numpad 8
          [this]() { m_renderWindow.rotateUp(); });

        m_shortcutManager->registerAction(
          "camera.rotateDown",
          "Rotate Down",
          "Rotate camera down",
          ShortcutContext::RenderWindow,
          ShortcutCombo(ImGuiKey_Keypad2, false, true), // Shift+Numpad 2
          [this]() { m_renderWindow.rotateDown(); });

        // Zoom shortcuts for RenderWindow (CAD standard)
        m_shortcutManager->registerAction("camera.zoomIn",
                                          "Zoom In",
                                          "Zoom in the camera view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_KeypadAdd), // Numpad +
                                          [this]() { m_renderWindow.zoomIn(); });

        m_shortcutManager->registerAction("camera.zoomOut",
                                          "Zoom Out",
                                          "Zoom out the camera view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_KeypadSubtract), // Numpad -
                                          [this]() { m_renderWindow.zoomOut(); });

        m_shortcutManager->registerAction("camera.zoomInAlt",
                                          "Zoom In (Alt)",
                                          "Zoom in the camera view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Equal, false, true), // Alt+=
                                          [this]() { m_renderWindow.zoomIn(); });

        m_shortcutManager->registerAction("camera.zoomOutAlt",
                                          "Zoom Out (Alt)",
                                          "Zoom out the camera view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Minus, false, true), // Alt-
                                          [this]() { m_renderWindow.zoomOut(); });

        m_shortcutManager->registerAction("camera.zoomExtents",
                                          "Zoom Extents",
                                          "Zoom to fit all objects in view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_KeypadMultiply), // Numpad *
                                          [this]() { m_renderWindow.zoomExtents(); });

        m_shortcutManager->registerAction("camera.zoomSelected",
                                          "Zoom Selected",
                                          "Zoom to fit selected objects",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_KeypadDivide), // Numpad /
                                          [this]() { m_renderWindow.zoomSelected(); });

        m_shortcutManager->registerAction("camera.resetZoom",
                                          "Reset Zoom",
                                          "Reset the camera zoom level",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_0, true), // Ctrl+0
                                          [this]() { m_renderWindow.resetZoom(); });

        // CAD-specific view shortcuts
        m_shortcutManager->registerAction(
          "camera.previousView",
          "Previous View",
          "Go to previous view",
          ShortcutContext::RenderWindow,
          ShortcutCombo(ImGuiKey_LeftArrow, false, true), // Shift+Left Arrow
          [this]() { m_renderWindow.previousView(); });

        m_shortcutManager->registerAction(
          "camera.nextView",
          "Next View",
          "Go to next view",
          ShortcutContext::RenderWindow,
          ShortcutCombo(ImGuiKey_RightArrow, false, true), // Shift+Right Arrow
          [this]() { m_renderWindow.nextView(); });

        m_shortcutManager->registerAction("camera.saveView",
                                          "Save View",
                                          "Save current view",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_V, true), // Ctrl+V
                                          [this]() { m_renderWindow.saveCurrentView(); });

        m_shortcutManager->registerAction(
          "camera.restoreView",
          "Restore View",
          "Restore saved view",
          ShortcutContext::RenderWindow,
          ShortcutCombo(ImGuiKey_V, true, false, true), // Ctrl+Shift+V
          [this]() { m_renderWindow.restoreSavedView(); });

        // Mouse wheel zoom (handled via shortcut manager)
        m_shortcutManager->registerAction("camera.zoomInWheel",
                                          "Zoom In (Wheel)",
                                          "Zoom in using mouse wheel",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo::fromString("WheelUp"),
                                          [this]() { m_renderWindow.zoomIn(); });

        m_shortcutManager->registerAction("camera.zoomOutWheel",
                                          "Zoom Out (Wheel)",
                                          "Zoom out using mouse wheel",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo::fromString("WheelDown"),
                                          [this]() { m_renderWindow.zoomOut(); });

        // Fly/Walk mode shortcuts
        m_shortcutManager->registerAction("camera.flyMode",
                                          "Toggle Fly Mode",
                                          "Toggle fly/walk camera mode",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_F), // F
                                          [this]() { m_renderWindow.toggleFlyMode(); });

        // Additional professional CAD shortcuts
        m_shortcutManager->registerAction("camera.orbitMode",
                                          "Orbit Mode",
                                          "Enter orbit camera mode",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_O), // O
                                          [this]() { m_renderWindow.setOrbitMode(); });

        m_shortcutManager->registerAction("camera.panMode",
                                          "Pan Mode",
                                          "Enter pan camera mode",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_P), // P
                                          [this]() { m_renderWindow.setPanMode(); });

        m_shortcutManager->registerAction("camera.zoomMode",
                                          "Zoom Mode",
                                          "Enter zoom camera mode",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_Z), // Z
                                          [this]() { m_renderWindow.setZoomMode(); });

        m_shortcutManager->registerAction("camera.resetOrientation",
                                          "Reset Orientation",
                                          "Reset camera orientation only",
                                          ShortcutContext::RenderWindow,
                                          ShortcutCombo(ImGuiKey_R, true), // Ctrl+R
                                          [this]() { m_renderWindow.resetOrientation(); });

        // Model editor node shortcuts
        m_shortcutManager->registerAction("model.autoLayout",
                                          "Auto Layout",
                                          "Automatically arrange nodes in the editor",
                                          ShortcutContext::ModelEditor,
                                          ShortcutCombo(ImGuiKey_L, true), // Ctrl+L
                                          [this]()
                                          {
                                              m_modelEditor.autoLayoutNodes(
                                                200.0f); // Use default distance
                                          });

        m_shortcutManager->registerAction("model.createNode",
                                          "Create Node",
                                          "Open the create node menu",
                                          ShortcutContext::ModelEditor,
                                          ShortcutCombo(ImGuiKey_G, true), // Ctrl+G
                                          [this]() { m_modelEditor.showCreateNodePopup(); });

        // Slice preview shortcuts
        m_shortcutManager->registerAction("sliceview.zoomin",
                                          "Zoom In",
                                          "Zoom in slice view",
                                          ShortcutContext::SlicePreview,
                                          ShortcutCombo(ImGuiKey_Equal, false, true), // Alt+=
                                          [this]()
                                          {
                                              if (m_isSlicePreviewVisible)
                                              {
                                                  m_sliceView.zoomIn();
                                              }
                                          });

        m_shortcutManager->registerAction("sliceview.zoomout",
                                          "Zoom Out",
                                          "Zoom out slice view",
                                          ShortcutContext::SlicePreview,
                                          ShortcutCombo(ImGuiKey_Minus, false, true), // Alt-
                                          [this]()
                                          {
                                              if (m_isSlicePreviewVisible)
                                              {
                                                  m_sliceView.zoomOut();
                                              }
                                          });

        // Slice preview wheel zoom
        m_shortcutManager->registerAction("sliceview.zoominWheel",
                                          "Zoom In (Wheel)",
                                          "Zoom in slice view using mouse wheel",
                                          ShortcutContext::SlicePreview,
                                          ShortcutCombo::fromString("WheelUp"),
                                          [this]()
                                          {
                                              if (m_isSlicePreviewVisible)
                                              {
                                                  m_sliceView.zoomIn();
                                              }
                                          });

        m_shortcutManager->registerAction("sliceview.zoomoutWheel",
                                          "Zoom Out (Wheel)",
                                          "Zoom out slice view using mouse wheel",
                                          ShortcutContext::SlicePreview,
                                          ShortcutCombo::fromString("WheelDown"),
                                          [this]()
                                          {
                                              if (m_isSlicePreviewVisible)
                                              {
                                                  m_sliceView.zoomOut();
                                              }
                                          });

        // Global UI scaling via Ctrl + Mouse Wheel
        m_shortcutManager->registerAction("view.uiScaleIncreaseWheel",
                                          "Increase UI Scale (Ctrl+Wheel)",
                                          "Increase UI scale using Ctrl+Mouse Wheel",
                                          ShortcutContext::Global,
                                          ShortcutCombo::fromString("Ctrl+WheelUp"),
                                          [this]() { m_mainView.adjustUserScale(1.10f); });

        m_shortcutManager->registerAction("view.uiScaleDecreaseWheel",
                                          "Decrease UI Scale (Ctrl+Wheel)",
                                          "Decrease UI scale using Ctrl+Mouse Wheel",
                                          ShortcutContext::Global,
                                          ShortcutCombo::fromString("Ctrl+WheelDown"),
                                          [this]() { m_mainView.adjustUserScale(1.0f / 1.10f); });

        m_shortcutManager->registerAction("sliceview.reset",
                                          "Reset View",
                                          "Reset the slice view",
                                          ShortcutContext::SlicePreview,
                                          ShortcutCombo(ImGuiKey_R), // R
                                          [this]()
                                          {
                                              if (m_isSlicePreviewVisible)
                                              {
                                                  m_sliceView.resetView();
                                              }
                                          });
    }

    void MainWindow::processShortcuts(ShortcutContext activeContext)
    {
        if (m_shortcutManager)
        {
            m_shortcutManager->processInput(activeContext);
        }
    }

    void MainWindow::showShortcutSettings()
    {
        m_shortcutSettingsDialog.show();
    }

    void MainWindow::showWelcomeScreen()
    {
        m_overlayOpacity = 1.0f;
        m_welcomeScreen.show();
    }

    void MainWindow::hideWelcomeScreen()
    {
        m_welcomeScreen.hide();
    }

    void MainWindow::saveRenderSettings()
    {
        if (!m_configManager)
        {
            return;
        }

        // Save rendering settings if compute is available
        if (m_computeAvailable && m_core)
        {
            auto & renderSettings = m_core->getResourceContext()->getRenderingSettings();

            nlohmann::json renderJson;
            renderJson["quality"] = renderSettings.quality;
            renderJson["sdfVisEnabled"] = (renderSettings.flags & RF_SHOW_FIELD) != 0u;

            m_configManager->setValue("rendering", "settings", renderJson);
        }

        // Save UI settings (always available)
        m_configManager->setValue("ui", "userScale", m_mainView.getUserScale());
        m_configManager->setValue(
          "ui", "theme", std::string(themeIdToString(m_mainView.getCurrentTheme())));

        // Save shortcuts too
        if (m_shortcutManager)
        {
            m_shortcutManager->saveShortcuts();
        }

        // Write to disk
        m_configManager->save();

        // Log success
        m_logger->addEvent({"Settings saved", events::Severity::Info});
    }

    void MainWindow::loadRenderSettings()
    {
        if (!m_configManager)
        {
            return;
        }

        // Load UI settings that don't require compute
        float const userScale = m_configManager->getValue<float>("ui", "userScale", 1.0f);
        m_mainView.setUserScale(userScale);

        std::string const themeName =
          m_configManager->getValue<std::string>("ui", "theme", "Modern");
        m_mainView.setCurrentTheme(themeIdFromString(themeName));

        if (!m_computeAvailable || !m_core)
        {
            return;
        }

        // Get render settings from config (or use default empty object if not found)
        nlohmann::json renderJson = m_configManager->getValue<nlohmann::json>(
          "rendering", "settings", nlohmann::json::object());

        // Skip if there are no saved settings
        if (renderJson.empty())
        {
            return;
        }

        // Get current rendering settings to update
        auto & renderSettings = m_core->getResourceContext()->getRenderingSettings();

        // Update settings from config
        if (renderJson.contains("quality"))
        {
            renderSettings.quality = renderJson["quality"].get<float>();
        }

        if (renderJson.contains("sdfVisEnabled"))
        {
            bool const en = renderJson["sdfVisEnabled"].get<bool>();
            if (en)
                renderSettings.flags |= RF_SHOW_FIELD;
            else
                renderSettings.flags &= ~RF_SHOW_FIELD;
        }

        // Log success
        m_logger->addEvent({"Rendering settings loaded", events::Severity::Info});
    }
}
