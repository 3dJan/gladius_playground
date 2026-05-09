#pragma once

/// @file MeshSdfSettingsDialog.h
/// @brief ImGui dialog for editing the persistent @ref gladius::MeshSdfSettings.

#include "MeshSdfSettings.h"

#include <functional>
#include <string>

namespace gladius::ui
{
    /**
     * @brief Modal-less dialog allowing the user to pick the mesh SDF
     *        evaluation strategy and the optional load-time mesh repair steps.
     *
     * The dialog edits a copy of the current @ref MeshSdfSettings; an explicit
     * **Apply** button writes the changes back and invokes the supplied
     * apply-callback (which is expected to push the new settings into the
     * active @ref Document and rebuild affected mesh resources).
     */
    class MeshSdfSettingsDialog
    {
      public:
        using ApplyCallback = std::function<void()>;

        MeshSdfSettingsDialog() = default;
        ~MeshSdfSettingsDialog() = default;

        MeshSdfSettingsDialog(MeshSdfSettingsDialog const &) = delete;
        MeshSdfSettingsDialog & operator=(MeshSdfSettingsDialog const &) = delete;

        /// Bind the persistent settings instance and the apply hook. Must be
        /// called once during application setup.
        void setSettings(MeshSdfSettings * settings);

        /// Provide the callback invoked when the user clicks **Apply**.
        /// Typically wired to @c Application::applyMeshSdfSettingsToCurrentDocument.
        void setApplyCallback(ApplyCallback callback);

        void show();
        void hide();
        [[nodiscard]] bool isVisible() const noexcept;

        /// Render the dialog. Safe to call every frame; no-op when hidden.
        void render();

        /// Set whether the NanoVDB method is available on the active OpenCL device.
        /// When @p supported is false the NanoVDB entry in the method combo is grayed out.
        /// @p reason is shown as a tooltip on the disabled entry (may be empty).
        void setVdbSupported(bool supported, std::string const & reason = {}) noexcept;

      private:
        void syncFromSettings();

        MeshSdfSettings * m_settings = nullptr;
        ApplyCallback m_applyCallback;

        bool m_visible = false;
        bool m_dirty = false;
        bool m_vdbSupported = false;
        std::string m_vdbNotSupportedReason;

        // Working copies edited by the UI; flushed on Apply.
        MeshSdfEvaluationConfig m_eval{};
        mesh_repair::MeshRepairConfig m_repair{};
    };

} // namespace gladius::ui
