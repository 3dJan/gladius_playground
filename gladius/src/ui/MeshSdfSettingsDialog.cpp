#include "MeshSdfSettingsDialog.h"

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

    namespace
    {
        char const * methodLabel(MeshSdfMethod m) noexcept
        {
            switch (m)
            {
            case MeshSdfMethod::PureBVH:          return "Pure BVH";
            case MeshSdfMethod::VoxelAccelerated: return "Voxel-accelerated BVH";
            case MeshSdfMethod::NanoVDB:          return "NanoVDB (not implemented)";
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
                               MeshSdfMethod::NanoVDB})
                {
                    bool const disabled = (m == MeshSdfMethod::NanoVDB);
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
                ImGui::DragFloat("  Weld epsilon",
                                 &m_repair.weldEpsilon,
                                 1e-6f,
                                 1e-7f,
                                 1e-2f,
                                 "%.6f");
                m_dirty = true;
            }

            if (ImGui::Checkbox("Remove degenerate triangles", &m_repair.removeDegenerate))
            {
                m_dirty = true;
            }
            if (m_repair.removeDegenerate)
            {
                ImGui::DragFloat("  Area epsilon",
                                 &m_repair.areaEpsilon,
                                 1e-11f,
                                 1e-14f,
                                 1e-4f,
                                 "%.4e");
                m_dirty = true;
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
                ImGui::DragFloat("  Max hole perimeter (mm)",
                                 &m_repair.maxHolePerimeter,
                                 0.01f,
                                 0.f,
                                 100.f,
                                 "%.3f");
                m_dirty = true;
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
            m_visible = false;
        }

        ImGui::End();
    }

} // namespace gladius::ui
