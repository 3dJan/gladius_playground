#pragma once

// use 32 bit indices for imgui

#include <atomic>
#include <filesystem>
#include <future>

#include "../ConfigManager.h"
#include "../Document.h"
#include "AboutDialog.h"
#include "CliExportDialog.h"
#include "ExportState.h"
#include "FileDialogService.h"
#include "GLView.h"
#include "LibraryExportDialog.h"
#include "LogView.h"
#include "MeshExportDialog.h"
#include "ModelEditor.h"
#include "Outline.h"
#include "ParameterThrottle.h"
#include "RecentFilesManager.h"
#include "RenderWindow.h"
#include "SliceView.h"
#include "WelcomeScreen.h"

// includes for the shortcut system
#include "ShortcutManager.h"
#include "ShortcutSettingsDialog.h"
#include "MeshSdfSettingsDialog.h"
#include "GamepadSettingsDialog.h"
#include "GamepadQuickRef.h"

#include <chrono>

namespace ed = ax::NodeEditor;

namespace gladius::ui
{
    enum class PendingFileOperation
    {
        None,
        NewModel,
        OpenFile
    };

    /// @brief Identifies which async file dialog operation is pending
    enum class AsyncDialogOperation
    {
        None,
        ExportCliCurrentLayer,
        ExportCliSliced,
        ExportSvgCurrentLayer,
        ExportVdb,
        ExportNvdb,
        Import,
        Open,
        Merge,
        SaveAs,
        SaveCurrentFunction,
        ImportImageStack,
        OpenAfterSavePrompt  ///< Open dialog triggered from "save before opening" popup
    };

    class MainWindow
    {
      public:
        MainWindow();

        /**
         * @brief Set the ConfigManager reference
         * @param configManager Reference to the ConfigManager
         */
        void setConfigManager(ConfigManager & configManager)
        {
            m_configManager = &configManager;
            m_recentFilesManager = std::make_unique<RecentFilesManager>(m_configManager);
        }

        void setup(std::shared_ptr<ComputeCore> core,
                   std::shared_ptr<Document> doc,
                   events::SharedLogger logger);
        void dragParameter(const std::string & label, float * valuePtr, float minVal, float maxVal);
        void renderSettingsDialog();
        void open(const std::filesystem::path & filename);
        void setStartupFile(std::filesystem::path filename);
        void startMainLoop();
        void setup();

        // Enable or disable verbose OpenCL debug checks/output for any contexts created here
        void setOpenCLDebugEnabled(bool enabled)
        {
            m_openclDebugEnabled = enabled;
        }

        /**
         * @brief Minimal setup for headless operation (no UI/GL windows).
         * Initializes ComputeCore and Document so document operations work in headless mode.
         * Does not register any UI callbacks and keeps Document in non-UI mode to avoid backups.
         */
        void setupHeadless(events::SharedLogger logger);

        /**
         * @brief Returns whether compute/rendering is available.
         */
        bool isComputeAvailable() const
        {
            return m_computeAvailable;
        }

        GLView & getGLView()
        {
            return m_mainView;
        }

        /**
         * @brief Initialize the shortcut system
         * Registers standard keyboard shortcuts for the application
         */
        void initializeShortcuts();

        /**
         * @brief Process keyboard shortcuts based on the active context
         * @param activeContext The currently active context
         */
        void processShortcuts(ShortcutContext activeContext);

        /**
         * @brief Show the shortcut settings dialog
         */
        void showShortcutSettings();

        /**
         * @brief Show the mesh SDF settings dialog
         */
        void showMeshSdfSettings();

        /**
         * @brief Show the gamepad settings dialog
         */
        void showGamepadSettings();

        /**
         * @brief Show the gamepad quick reference overlay
         */
        void showGamepadQuickReference();

        /**
         * @brief Bind the persistent mesh SDF settings + apply hook used by the
         *        "Mesh SDF Settings" menu entry.
         */
        void setMeshSdfSettings(MeshSdfSettings * settings,
                                MeshSdfSettingsDialog::ApplyCallback applyCallback);

        /// Inform the mesh SDF settings dialog whether NanoVDB is supported
        /// on the active OpenCL device. Grays out the NanoVDB combo entry when false.
        /// @p reason is forwarded to the dialog as a tooltip (may be empty).
        void setVdbSupported(bool supported, std::string const & reason = {});

        /// Register a callback that is invoked once when the async OpenCL compute
        /// initialisation has finished (success or failure). Use this to react to
        /// device capabilities that are only known after init completes.
        void setOnComputeReadyCallback(ViewCallBack callback);

        /**
         * @brief Show the welcome screen and reset overlay opacity
         */
        void showWelcomeScreen();

        /**
         * @brief Hide the welcome screen
         */
        void hideWelcomeScreen();

        /**
         * @brief Create a new model
         */
        void newModel();

        /**
         * @brief Get the current document
         * @return Shared pointer to the current document
         */
        std::shared_ptr<Document> getCurrentDocument() const
        {
            return m_doc;
        }

        /**
         * @brief Get the export state for checking if export is in progress
         * @return Reference to the export state
         */
        ExportState & getExportState()
        {
            return m_exportState;
        }

        /**
         * @brief Get the export state (const version)
         * @return Const reference to the export state
         */
        ExportState const & getExportState() const
        {
            return m_exportState;
        }

      private:
        void render();
        void nodeEditor();
        void mainWindowDockingArea();

        void renderWindow();
        void mainMenu();
        void sliceWindow();
        void meshExportDialog();
        void cliExportDialog();
        void showExitPopUp();
        void showExportInProgressWarning();
        void showSaveBeforeFileOperationPopUp();
        void showSaveAsOverwriteConfirmation();
        void logViewer();
        void renderStatusBar();
        void renderComputeErrorModal();

        /// Renders a fullscreen semi-transparent overlay when export is in progress
        void renderExportOverlay();

        void refreshModel();

        void markFileAsChanged();
        void import();
        void updateContours();
        void close();
        void open();
        void merge();
        void resetEditorState();
        void save();
        void updateModel();
        void saveAs(std::filesystem::path defaultPath = {});
        void executeSaveAs(std::filesystem::path const & savePath);
        void saveCurrentFunction();
        void importImageStack();

        /**
         * @brief Helper to load a file asynchronously and defer editor reset
         * @param filename Path to the file to load
         */
        void loadFileDeferred(const std::filesystem::path & filename);

        /**
         * @brief Save rendering settings to configuration
         */
        void saveRenderSettings();

        /**
         * @brief Load rendering settings from configuration
         */
        void loadRenderSettings();

        void onPreviewProgramSwap();

        /**
         * @brief Add a file to the recent files list
         * @param filePath Path to the file
         */
        void addToRecentFiles(std::filesystem::path const & filePath);

        /**
         * @brief Get the list of recent files
         * @param maxCount Maximum number of files to return
         * @return List of pairs containing file paths and timestamps
         */
        std::vector<std::pair<std::filesystem::path, std::time_t>>
        getRecentFiles(size_t maxCount = 100) const;

        /// Recent files manager
        std::unique_ptr<RecentFilesManager> m_recentFilesManager;

        GLView m_mainView;

        ModelEditor m_modelEditor;

        std::filesystem::path m_modelFileName;
        std::optional<std::filesystem::path> m_currentAssemblyFileName;
        std::shared_ptr<ComputeCore> m_core;
        bool m_fileChanged{false};
        std::atomic<bool> m_dirty{true};
        std::atomic<bool> m_parameterDirty{false};
        std::atomic<bool> m_contoursDirty{false};
        ParameterThrottle m_parameterThrottle;

        ViewCallBack m_renderCallback;

        bool m_showStyleEditor{false};

        bool m_showMainMenu{false};
        bool m_isSlicePreviewVisible{false};
        bool m_showSaveBeforeExit{false};
        bool m_showSaveBeforeFileOperation{false};
        bool m_showExportInProgressWarning{false};
        PendingFileOperation m_pendingFileOperation{PendingFileOperation::None};
        std::optional<std::filesystem::path> m_pendingOpenFilename;
        std::optional<std::filesystem::path> m_startupFile;

        float m_mainMenuPosX{-400.f}; // used for the move in animation

        bool m_moving = false;

        bool m_showAuthoringTools{true};
        MeshExportDialog m_meshExporterDialog;
        CliExportDialog m_cliExportDialog;
        LibraryExportDialog m_libraryExportDialog;
        SliceView m_sliceView;
        LogView m_logView;
        RenderWindow m_renderWindow;
        AboutDialog m_about;
        WelcomeScreen m_welcomeScreen;

        std::shared_ptr<Document> m_doc;
        events::SharedLogger m_logger;

        /// @brief State for async file loading coordination
        enum class AsyncLoadState
        {
            Idle,
            Loading,
            LoadingWithReset
        };
        AsyncLoadState m_asyncLoadState{AsyncLoadState::Idle};

        // Flag to remember if library browser was visible
        bool m_isLibraryBrowserVisible = false;

        double m_maxTimeSliceOptimization_s = 60.f;

        bool m_initialized = false;

        bool m_showSettings = true;

        size_t m_lastEventCount{};

        size_t m_lastWarningCount{};

        std::chrono::time_point<std::chrono::steady_clock> m_lastUpateTime;

        Outline m_outline;

        float m_uiScale = 1.f;

        /// Opacity value for the welcome screen overlay (0.0-1.0)
        float m_overlayOpacity = 1.0f;

        /// Flag indicating if welcome screen overlay fadeout is in progress
        bool m_overlayFadeoutActive = false;

        ConfigManager * m_configManager = nullptr; // Pointer to the Application's ConfigManager

        bool m_wasWelcomeScreenVisible = false;

        // Shortcut system
        std::shared_ptr<ShortcutManager> m_shortcutManager;
        ShortcutSettingsDialog m_shortcutSettingsDialog;
        MeshSdfSettingsDialog m_meshSdfSettingsDialog;

        // Gamepad system
        GamepadSettingsDialog m_gamepadSettingsDialog;
        GamepadQuickRef m_gamepadQuickRef;
        /// Mirror of the dialog's Apply callback so we can invoke it
        /// programmatically after a freshly loaded document — otherwise the
        /// persisted mesh-SDF method (e.g. FastWindingNumber) is never pushed
        /// into the renderer until the user opens the dialog and clicks Apply.
        MeshSdfSettingsDialog::ApplyCallback m_meshSdfApplyCallback;
        ViewCallBack m_onComputeReadyCallback;

        // Compute availability flag. If false, UI runs in a limited mode without rendering.
        bool m_computeAvailable{true};
        // Optional message why compute is disabled.
        std::string m_computeErrorMessage;
        // Controls visibility of the compute error details modal
        bool m_showComputeErrorModal{false};

        // Instance-level flag to propagate OpenCL debug verbosity to contexts we create
        bool m_openclDebugEnabled{false};

        // Async file dialog for non-blocking file/directory selection
        AsyncFileDialog m_asyncFileDialog;
        AsyncDialogOperation m_asyncDialogOp{AsyncDialogOperation::None};

        /// @brief Path selected by Save As that already exists and waits for overwrite confirmation
        std::optional<std::filesystem::path> m_pendingSaveAsPath;
        /// @brief Whether to show the Save As overwrite confirmation modal
        bool m_showSaveAsOverwriteConfirmation{false};

        /// @brief Process async file dialog results and execute pending operations
        void processAsyncFileDialog();

        // Export state for blocking UI modifications during mesh export
        ExportState m_exportState;

        // --- Deferred OpenCL initialization ---
        /// @brief State of the async compute initialization
        enum class ComputeInitState
        {
            NotStarted,   ///< Not yet started
            InProgress,   ///< Async device enumeration running
            Completed,    ///< Enumeration completed, needs finalization on main thread
            Finalized     ///< Fully initialized
        };
        ComputeInitState m_computeInitState{ComputeInitState::NotStarted};

        /// @brief Result of async device enumeration
        struct ComputeEnumResult
        {
            AcceleratorList accelerators;
            bool success = false;
            std::string errorMessage;
        };

        /// @brief Future for async compute initialization
        std::future<ComputeEnumResult> m_computeInitFuture;

        /// @brief Start async compute initialization
        void startAsyncComputeInit();

        /// @brief Poll and finalize async compute initialization (called from render loop)
        void pollComputeInit();
    };
}
