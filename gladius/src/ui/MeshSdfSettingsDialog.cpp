#include "MeshSdfSettingsDialog.h"

#include <algorithm>
#include <imgui.h>

namespace gladius::ui
{
    void MeshSdfSettingsDialog::setSettings(MeshSdfSettings * settings)
    {
        m_settings = settings;
        syncFromSettings();
    }

    void MeshSdfSettingsDialog::setApplyCallback(ApplyCallback callback)
    {
        m_applyCallback = std::move(callback);
    }

    void MeshSdfSettingsDialog::show()
    {
        syncFromSettings();
        m_visible = true;
    }

    void MeshSdfSettingsDialog::hide()
    {
        m_visible = false;
    }

    bool MeshSdfSettingsDialog::isVisible() const noexcept
    {
        return m_visible;
    }

    void MeshSdfSettingsDialog::syncFromSettings()
    {
        if (m_settings == nullptr)
        {
            return;
        }
        m_eval = m_settings->evaluationConfig();
        m_repair = m_settings->repairConfig();
        m_dirty = false;
    }

    void MeshSdfSettingsDialog::setVdbSupported(bool supported, std::string const & reason) noexcept
    {
        m_vdbSupported = supported;
        m_vdbNotSupportedReason = reason;
    }

    namespace
    {
        char const * methodLabel(MeshSdfMethod m) noexcept
        {
            switch (m)
            {
            case MeshSdfMethod::PureBVH:           return "Pure BVH";
            case MeshSdfMethod::VoxelAccelerated:  return "Voxel-accelerated BVH";
            case MeshSdfMethod::NanoVDB:           return "NanoVDB";
            case MeshSdfMethod::FastWindingNumber: return "Fast winding number";
            }
            return "Pure BVH";
        }
    } // namespace

    void MeshSdfSettingsDialog::render()
    {
        if (!m_visible)
        {
            return;
        }

        if (!ImGui::Begin("Mesh SDF Settings", &m_visible, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return;
        }

        if (m_settings == nullptr)
        {
            ImGui::TextUnformatted("Settings backend not bound.");
            ImGui::End();
            return;
        }

        // ---- Evaluation method ------------------------------------------------
        if (ImGui::CollapsingHeader("Evaluation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            char const * preview = methodLabel(m_eval.method);
            if (ImGui::BeginCombo("Method", preview))
            {
                for (auto m : {MeshSdfMethod::PureBVH,
                               MeshSdfMethod::VoxelAccelerated,
                               MeshSdfMethod::FastWindingNumber,
                               MeshSdfMethod::NanoVDB})
                {
                    bool const disabled = (m == MeshSdfMethod::NanoVDB && !m_vdbSupported);
                    if (disabled)
                    {
                        ImGui::BeginDisabled();
                    }
                    bool const selected = (m == m_eval.method);
                    if (ImGui::Selectable(methodLabel(m), selected) && !disabled)
                    {
                        if (m_eval.method != m)
                        {
                            m_eval.method = m;
                            m_dirty = true;
                        }
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                    if (disabled)
                    {
                        ImGui::EndDisabled();
                        // Show why NanoVDB is unavailable when the user hovers the item.
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            char const * reason = m_vdbNotSupportedReason.empty()
                                                      ? "Not supported on this OpenCL device."
                                                      : m_vdbNotSupportedReason.c_str();
                            ImGui::SetTooltip("NanoVDB unavailable: %s", reason);
                        }
                    }
                }
                ImGui::EndCombo();
            }

            if (m_eval.method == MeshSdfMethod::VoxelAccelerated)
            {
                int resolution = m_eval.voxelGridResolution;
                if (ImGui::SliderInt("Voxel grid resolution", &resolution, 8, 256))
                {
                    m_eval.voxelGridResolution = resolution;
                    m_dirty = true;
                }
                ImGui::TextDisabled(
                    "Higher = finer cache, slower build, more memory. Default 32.");
            }

            if (m_eval.method == MeshSdfMethod::NanoVDB)
            {
                if (ImGui::DragFloat("Voxel size (mm)",
                                     &m_eval.nanovdbVoxelSize_mm,
                                     0.005f,
                                     0.01f,
                                     2.0f,
                                     "%.3f mm"))
                {
                    m_eval.nanovdbVoxelSize_mm =
                        std::max(m_eval.nanovdbVoxelSize_mm, 0.01f);
                    m_dirty = true;
                }
                ImGui::TextDisabled(
                    "Near-field SDF resolution. 0.1 mm recommended for L-PBF (200 \xC2\xB5m "
                    "walls). A coarse face-index grid (~5\xC3\x97) covers far-field distances. "
                    "Smaller values use more GPU memory.");
            }

            if (m_eval.method == MeshSdfMethod::FastWindingNumber)
            {
                if (ImGui::SliderFloat(
                        "Barnes-Hut \xCE\xB2", &m_eval.fwnBeta, 1.0f, 4.0f, "%.2f"))
                {
                    m_dirty = true;
                }
                ImGui::TextDisabled(
                    "Larger \xCE\xB2 \xE2\x86\x92 more accurate winding sign, slower. "
                    "Default 2.0 (Barill et al. 2018).");

                if (ImGui::SliderFloat("Far-field skip factor",
                                       &m_eval.fwnFarFieldFactor,
                                       0.0f,
                                       2.0f,
                                       "%.2f"))
                {
                    m_dirty = true;
                }
                ImGui::TextDisabled(
                    "Skip winding traversal beyond factor \xC3\x97 half-bbox-diagonal. "
                    "0 = exact (slow). 0.5 = recommended. >1 = aggressive (may show "
                    "sign artifacts on thin features).");

                if (ImGui::Checkbox("Use sign cache", &m_eval.fwnUseSignCache))
                {
                    m_dirty = true;
                }
                ImGui::TextDisabled(
                    "Pre-classifies cells far from the surface on the GPU to skip "
                    "winding traversal. Disable to diagnose sign speckles.");
            }

            if (ImGui::Checkbox("Use early-exit hint", &m_eval.useEarlyExit))
            {
                m_dirty = true;
            }
            ImGui::TextDisabled(
                "Lets the raymarcher cut mesh queries short for far-from-surface samples.");

            if (ImGui::DragFloat("Inflation distance (mm)",
                                 &m_eval.inflationDistance,
                                 0.001f,
                                 0.f,
                                 5.f,
                                 "%.4f"))
            {
                m_dirty = true;
            }
            ImGui::TextDisabled("Subtracted from every mesh-SDF reading. Closes pinholes "
                                "at the cost of rounding sharp features.");
        }

        // ---- Mesh repair ------------------------------------------------------
        if (ImGui::CollapsingHeader("Repair on import"))
        {
            ImGui::TextDisabled("Applied once when a 3MF mesh is loaded. "
                                "Toggling these affects the next import only.");

            if (ImGui::Checkbox("Weld duplicate vertices", &m_repair.weld))
            {
                m_dirty = true;
            }
            if (m_repair.weld)
            {
                if (ImGui::DragFloat("  Weld epsilon",
                                     &m_repair.weldEpsilon,
                                     1e-6f,
                                     1e-7f,
                                     1e-2f,
                                     "%.6f"))
                {
                    m_dirty = true;
                }
            }

            if (ImGui::Checkbox("Remove degenerate triangles", &m_repair.removeDegenerate))
            {
                m_dirty = true;
            }
            if (m_repair.removeDegenerate)
            {
                if (ImGui::DragFloat("  Area epsilon",
                                     &m_repair.areaEpsilon,
                                     1e-11f,
                                     1e-14f,
                                     1e-4f,
                                     "%.4e"))
                {
                    m_dirty = true;
                }
            }

            if (ImGui::Checkbox("Orient consistently", &m_repair.orientConsistently))
            {
                m_dirty = true;
            }

            if (ImGui::Checkbox("Fill small holes", &m_repair.fillHoles))
            {
                m_dirty = true;
            }
            if (m_repair.fillHoles)
            {
                if (ImGui::DragFloat("  Max hole perimeter (mm)",
                                     &m_repair.maxHolePerimeter,
                                     0.01f,
                                     0.f,
                                     100.f,
                                     "%.3f"))
                {
                    m_dirty = true;
                }
            }
        }

        ImGui::Separator();

        if (!m_dirty)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Apply"))
        {
            m_settings->setEvaluationConfig(m_eval);
            m_settings->setRepairConfig(m_repair);
            if (m_applyCallback)
            {
                m_applyCallback();
            }
            m_dirty = false;
        }
        if (!m_dirty)
        {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("Revert"))
        {
            syncFromSettings();
        }

        ImGui::SameLine();
        if (ImGui::Button("Close"))
        {
            // Discard pending edits so a subsequent show() starts from a
            // clean state regardless of how the user dismisses the window.
            syncFromSettings();
            m_visible = false;
        }

        ImGui::End();

        // The window's [X] button toggles m_visible directly via the Begin
        // pointer parameter. Treat that path the same as Close so we never
        // hold stale working copies across show/hide cycles.
        if (!m_visible && m_dirty)
        {
            syncFromSettings();
        }
    }

} // namespace gladius::ui
