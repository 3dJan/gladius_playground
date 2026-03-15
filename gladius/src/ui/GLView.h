#pragma once

#include "../gpgpu.h"
#include "Theme.h"

#include <filesystem>
#include <functional>
#include <future>
#include <mutex>

#include "imgui.h"

struct GLFWwindow;
struct ImGuiTestEngine;

namespace gladius
{
    enum class FullscreenMode
    {
        Windowed = 0,
        SingleMonitor = 1,
        SpanAllSameHeight = 2
    };
    using ViewCallBack = std::function<void()>;
    using RequestCloseCallBack = std::function<void()>;
    using FileDropCallBack = std::function<void(const std::filesystem::path &)>;

    struct WindowSettings
    {
        int width = 1280;
        int height = 720;
        int x = 50;
        int y = 50;
        FullscreenMode fullscreenMode{FullscreenMode::Windowed};
    };

    class GLView
    {
      public:
        GLView();

        ~GLView();

        // Ensure a GL context and ImGui are initialized without entering the main loop
        void ensureInitialized();

        void startMainLoop();

        void render();

        void setRenderCallback(const ViewCallBack & func);

        void setFileDropCallback(const FileDropCallBack & func);

#ifdef ENABLE_UI_TESTING
        ImGuiTestEngine* getTestEngine() { return m_testEngine; }
#endif


        void applyFullscreenMode();

        [[nodiscard]] size_t getWidth() const;
        [[nodiscard]] size_t getHeight() const;

        /**
         * @brief Get the current UI scale factor
         * @return The UI scale factor
         */
        [[nodiscard]] float getUiScale() const
        {
            return m_uiScale;
        }

        /**
         * @brief Set the total UI scale factor (base * user)
         * @param scale The total UI scale factor to set
         */
        void setUiScale(float scale)
        {
            m_uiScale = scale;
        }

        /**
         * @brief Get the OS/monitor-provided base (HiDPI) scale
         */
        [[nodiscard]] float getBaseScale() const
        {
            return m_baseScale;
        }

        /**
         * @brief Get the user-defined additional UI scale factor
         */
        [[nodiscard]] float getUserScale() const
        {
            return m_userScale;
        }

        /**
         * @brief Set the user-defined additional UI scale factor
         * This will be multiplied with the base (HiDPI) scale.
         */
        void setUserScale(float scale);

        /**
         * @brief Adjust user-defined scale by a multiplicative factor and clamp
         * @param factor Multiplicative change (e.g., 1.1 to increase by 10%)
         */
        void adjustUserScale(float factor);

        /**
         * @brief Reset user-defined scale to 1.0
         */
        void resetUserScale();

        void storeWindowSettings();

        void addViewCallBack(const ViewCallBack & func);
        void clearViewCallback();
        void setRequestCloseCallBack(const ViewCallBack & func);

        bool isViewSettingsVisible();
        void setViewSettingsVisible(bool visible);

        [[nodiscard]] bool isFullScreen() const;
        void setFullScreen(bool enableFullscreen);

        // New tri-state fullscreen API
        [[nodiscard]] FullscreenMode getFullscreenMode() const
        {
            return m_windowSettings.fullscreenMode;
        }

        void setFullscreenMode(FullscreenMode mode);
        
        // Check if spanning across multiple monitors is available
        [[nodiscard]] bool isSpanModeAvailable() const;
        
        void startAnimationMode();
        void stopAnimationMode();

        // [[nodiscard]]
        // HWND getNativeWindowHandle();

        // Request a screenshot to be saved at the end of the next rendered frame
        std::future<bool> requestScreenshot(const std::string& outputPath);

        /// Get the current theme
        [[nodiscard]] ThemeId getCurrentTheme() const
        {
            return m_currentTheme;
        }

        /// Change the active theme (takes effect immediately)
        void setCurrentTheme(ThemeId theme)
        {
            m_currentTheme = theme;
            applyTheme(m_currentTheme);
            m_originalStyle = ImGui::GetStyle();
        }

      private:
        void init();
        void setGladiusTheme(ImGuiIO & io);
        void initImgUI();

        void displayUI();
        void determineUiScale();
        void recomputeTotalScale();
        void processPendingScreenshot();

        static void noOp()
        {
        }

        static void noOpFileDrop(const std::filesystem::path &)
        {
        }

        void handleDropCallback(GLFWwindow *, int count, const char ** paths);
        ViewCallBack m_render = noOp;
        RequestCloseCallBack m_close = noOp;

        FileDropCallBack m_fileDrop = noOpFileDrop;
        std::string m_screenshotPath;
        std::shared_ptr<std::promise<bool>> m_screenshotPromise;
        std::mutex m_screenshotMutex;
        // applied tri-state fullscreen mode (current native window state)
        FullscreenMode m_appliedFullscreenMode{FullscreenMode::Windowed};
        WindowSettings m_windowSettings;

        std::vector<ViewCallBack> m_viewCallBacks;
        bool m_show_demo_window{false};
        bool m_showViewSettings{false};

        std::filesystem::path m_gladiusImgUiFilename{};
        std::string m_iniFileNameStorage{}; // Stores the string for ImGui ini filename
        GLFWwindow * m_window{nullptr};
        bool m_isAnimationRunning = false;
        bool m_stateCloseRequested = false;
        bool m_initialized = false; // true once a window and ImGui context have been created

        ImGuiStyle m_originalStyle;
        float m_uiScale = 1.0f;   // total = base * user
        float m_baseScale = 1.0f; // detected from OS/monitor/DPI
        float m_userScale = 1.0f; // user preference, persisted
        ThemeId m_currentTheme{ThemeId::Modern}; // active theme
        
#ifdef ENABLE_UI_TESTING
        ImGuiTestEngine* m_testEngine = nullptr;
#endif
    };
}
