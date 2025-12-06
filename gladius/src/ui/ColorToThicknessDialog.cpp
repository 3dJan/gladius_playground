#include "ColorToThicknessDialog.h"

#include "Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace gladius::ui
{
    namespace
    {
        constexpr ImVec2 kSwatchSize{32.0F, 18.0F};

        float clamp01(float value)
        {
            return std::clamp(value, 0.0F, 1.0F);
        }

        void colorButtonWithTooltip(char const * label, ImVec4 color, char const * tooltip)
        {
            ImGui::ColorButton(label, color, ImGuiColorEditFlags_NoTooltip, kSwatchSize);
            if (ImGui::IsItemHovered() && tooltip != nullptr)
            {
                ImGui::SetTooltip("%s", tooltip);
            }
        }

        std::array<char, 64> makeNameBuffer(std::string const & name)
        {
            std::array<char, 64> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%s", name.c_str());
            return buffer;
        }
    } // namespace

    void ColorToThicknessDialog::open()
    {
        m_visible = true;
    }

    void ColorToThicknessDialog::setVisible(bool visible)
    {
        m_visible = visible;
    }

    bool ColorToThicknessDialog::isVisible() const
    {
        return m_visible;
    }

    void ColorToThicknessDialog::setPaletteColors(std::vector<Eigen::Vector3f> colors)
    {
        m_palette = std::move(colors);
    }

    void ColorToThicknessDialog::render()
    {
        if (!m_visible)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(780, 680), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Color → Shell Thickness", &m_visible))
        {
            ImGui::End();
            return;
        }

        handleFileDialogResult();

        renderMaterialsSection();
        ImGui::Spacing();
        renderPaletteSection();
        ImGui::Spacing();
        renderConstraintsSection();
        ImGui::Spacing();
        renderResultsSection();

        ImGui::End();
    }

    void ColorToThicknessDialog::renderMaterialsSection()
    {
        ImGui::Text("Materials");
        ImGui::Separator();

        ImGui::BeginDisabled(m_fileDialog.isActive());
        if (ImGui::Button("Add material"))
        {
            io::FilamentOpticalProperties filament;
            filament.name = "Material" + std::to_string(m_materials.size() + 1);
            filament.reflectanceColor = Eigen::Vector3f(1.0F, 1.0F, 1.0F);
            filament.opacity = 0.6F;
            filament.referenceThickness = 0.4F;
            filament.minThickness = 0.0F;
            filament.maxThickness = 5.0F;
            m_materials.push_back(filament);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load…"))
        {
            m_pendingFileMode = FileMode::Load;
            m_fileDialog.openFile({"*.json"}, m_materialsFile);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save…"))
        {
            m_pendingFileMode = FileMode::Save;
            m_fileDialog.saveFile({"*.json"}, m_materialsFile);
        }
        ImGui::EndDisabled();

        if (m_fileDialog.isActive())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Waiting for file dialog…");
        }

        if (m_materials.empty())
        {
            ImGui::TextDisabled("No materials defined. Add one to begin.");
            return;
        }

        if (ImGui::BeginTable("MaterialsTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0F);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 120.0F);
            ImGui::TableSetupColumn("Opacity", ImGuiTableColumnFlags_WidthFixed, 90.0F);
            ImGui::TableSetupColumn("Trans. dist (mm)", ImGuiTableColumnFlags_WidthFixed, 120.0F);
            ImGui::TableSetupColumn("Min/Max (mm)", ImGuiTableColumnFlags_WidthFixed, 140.0F);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 70.0F);
            ImGui::TableHeadersRow();

            for (std::size_t i = 0; i < m_materials.size(); ++i)
            {
                auto & mat = m_materials[i];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%zu", i + 1);

                ImGui::TableSetColumnIndex(1);
                auto nameBuffer = makeNameBuffer(mat.name);
                if (ImGui::InputText(("##name" + std::to_string(i)).c_str(),
                                      nameBuffer.data(),
                                      nameBuffer.size()))
                {
                    mat.name = nameBuffer.data();
                }

                ImGui::TableSetColumnIndex(2);
                float color[3]{mat.reflectanceColor.x(), mat.reflectanceColor.y(), mat.reflectanceColor.z()};
                if (ImGui::ColorEdit3(("##color" + std::to_string(i)).c_str(),
                                      color,
                                      ImGuiColorEditFlags_NoInputs))
                {
                    mat.reflectanceColor = Eigen::Vector3f{clamp01(color[0]), clamp01(color[1]), clamp01(color[2])};
                }

                ImGui::TableSetColumnIndex(3);
                ImGui::SetNextItemWidth(90.0F);
                if (ImGui::InputFloat(("##opacity" + std::to_string(i)).c_str(), &mat.opacity, 0.05F, 0.1F, "%.2f"))
                {
                    mat.opacity = clamp01(mat.opacity);
                }

                ImGui::TableSetColumnIndex(4);
                ImGui::SetNextItemWidth(120.0F);
                ImGui::InputFloat(("##refthick" + std::to_string(i)).c_str(), &mat.referenceThickness, 0.05F, 0.1F, "%.3f");

                ImGui::TableSetColumnIndex(5);
                ImGui::SetNextItemWidth(65.0F);
                ImGui::InputFloat(("##minth" + std::to_string(i)).c_str(), &mat.minThickness, 0.05F, 0.1F, "%.3f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(65.0F);
                ImGui::InputFloat(("##maxth" + std::to_string(i)).c_str(), &mat.maxThickness, 0.1F, 0.5F, "%.3f");
                if (mat.maxThickness < mat.minThickness)
                {
                    mat.maxThickness = mat.minThickness;
                }

                ImGui::TableSetColumnIndex(6);
                if (ImGui::Button(("Delete##" + std::to_string(i)).c_str()))
                {
                    m_materials.erase(m_materials.begin() + static_cast<std::ptrdiff_t>(i));
                    --i;
                }
            }

            ImGui::EndTable();
        }
    }

    void ColorToThicknessDialog::renderPaletteSection()
    {
        ImGui::Text("Palette (target colors)");
        ImGui::Separator();

        if (ImGui::Button("Add color"))
        {
            m_palette.push_back(Eigen::Vector3f(1.0F, 1.0F, 1.0F));
        }
        if (!m_palette.empty())
        {
            ImGui::SameLine();
            if (ImGui::Button("Clear palette"))
            {
                m_palette.clear();
                m_solutions.clear();
            }
        }

        if (m_palette.empty())
        {
            ImGui::TextDisabled("No palette colors. Add colors to compute thickness mapping.");
            return;
        }

        if (ImGui::BeginTable("PaletteTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0F);
            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 150.0F);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 80.0F);
            ImGui::TableHeadersRow();

            for (std::size_t i = 0; i < m_palette.size(); ++i)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%zu", i + 1);

                ImGui::TableSetColumnIndex(1);
                float color[3]{m_palette[i].x(), m_palette[i].y(), m_palette[i].z()};
                if (ImGui::ColorEdit3(("##pal" + std::to_string(i)).c_str(), color))
                {
                    m_palette[i] = Eigen::Vector3f{clamp01(color[0]), clamp01(color[1]), clamp01(color[2])};
                }

                ImGui::TableSetColumnIndex(2);
                if (ImGui::Button(("Remove##pal" + std::to_string(i)).c_str()))
                {
                    m_palette.erase(m_palette.begin() + static_cast<std::ptrdiff_t>(i));
                    m_solutions.clear();
                    --i;
                }
            }

            ImGui::EndTable();
        }
    }

    void ColorToThicknessDialog::renderConstraintsSection()
    {
        ImGui::Text("Thickness constraints");
        ImGui::Separator();

        ImGui::SetNextItemWidth(140.0F);
        if (ImGui::InputFloat("Min thickness (mm)", &m_constraints.minThickness, 0.05F, 0.1F, "%.3f"))
        {
            m_constraints.minThickness = std::max(0.0F, m_constraints.minThickness);
            if (m_constraints.maxThickness < m_constraints.minThickness)
            {
                m_constraints.maxThickness = m_constraints.minThickness;
            }
        }

        ImGui::SetNextItemWidth(140.0F);
        if (ImGui::InputFloat("Max thickness (mm)", &m_constraints.maxThickness, 0.1F, 0.5F, "%.3f"))
        {
            m_constraints.maxThickness = std::max(m_constraints.maxThickness, m_constraints.minThickness);
        }

        ImGui::SetNextItemWidth(140.0F);
        ImGui::InputFloat("Layer height (mm, optional)", &m_constraints.layerHeight, 0.01F, 0.05F, "%.3f");

        ImGui::SetNextItemWidth(140.0F);
        ImGui::InputFloat("Total max thickness (mm, optional)", &m_constraints.totalMaxThickness, 0.5F, 1.0F, "%.3f");

        bool const canCompute = !m_materials.empty() && !m_palette.empty();
        ImGui::BeginDisabled(!canCompute);
        if (ImGui::Button("Compute"))
        {
            computeThicknessMapping();
        }
        ImGui::EndDisabled();
        if (!canCompute)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Add materials and palette colors to enable computation.");
        }
    }

    void ColorToThicknessDialog::renderResultsSection()
    {
        ImGui::Text("Thickness results");
        ImGui::Separator();

        if (m_solutions.empty())
        {
            ImGui::TextDisabled("No results yet. Compute to see thickness mapping.");
            return;
        }

        int const columnCount = static_cast<int>(m_materials.size()) + 3;
        if (ImGui::BeginTable("ResultsTable", columnCount, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0F);
            ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 90.0F);
            for (auto const & mat : m_materials)
            {
                ImGui::TableSetupColumn(mat.name.c_str(), ImGuiTableColumnFlags_WidthFixed, 110.0F);
            }
            ImGui::TableSetupColumn("Error", ImGuiTableColumnFlags_WidthFixed, 80.0F);
            ImGui::TableHeadersRow();

            for (std::size_t row = 0; row < m_solutions.size(); ++row)
            {
                auto const & solution = m_solutions[row];

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%zu", row + 1);

                ImGui::TableSetColumnIndex(1);
                ImVec4 targetColor{solution.targetColor.x(), solution.targetColor.y(), solution.targetColor.z(), 1.0F};
                colorButtonWithTooltip(("##target" + std::to_string(row)).c_str(), targetColor, "Target color");
                ImGui::SameLine();
                ImVec4 achievedColor{solution.achievedColor.x(), solution.achievedColor.y(), solution.achievedColor.z(), 1.0F};
                colorButtonWithTooltip(("##achieved" + std::to_string(row)).c_str(), achievedColor, "Achieved color");

                for (std::size_t col = 0; col < m_materials.size(); ++col)
                {
                    ImGui::TableSetColumnIndex(static_cast<int>(2 + col));
                    float thickness = 0.0F;
                    if (col < solution.thicknesses.size())
                    {
                        thickness = solution.thicknesses[col];
                    }
                    ImGui::Text("%.3f mm", thickness);
                }

                ImGui::TableSetColumnIndex(columnCount - 1);
                ImGui::Text("%.4f", solution.colorError);
                if (!solution.converged)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(not converged)");
                }
            }

            ImGui::EndTable();
        }
    }

    void ColorToThicknessDialog::handleFileDialogResult()
    {
        if (m_pendingFileMode == FileMode::None)
        {
            return;
        }

        if (auto result = m_fileDialog.checkResult())
        {
            if (*result)
            {
                m_materialsFile = **result;
                if (m_pendingFileMode == FileMode::Load)
                {
                    loadMaterialsFromFile(**result);
                }
                else if (m_pendingFileMode == FileMode::Save)
                {
                    saveMaterialsToFile(**result);
                }
            }
            m_pendingFileMode = FileMode::None;
        }
    }

    void ColorToThicknessDialog::loadMaterialsFromFile(std::filesystem::path const & path)
    {
        std::ifstream stream(path);
        if (!stream)
        {
            return;
        }
        nlohmann::json json;
        stream >> json;
        deserializeMaterials(json);
    }

    void ColorToThicknessDialog::saveMaterialsToFile(std::filesystem::path const & path) const
    {
        nlohmann::json json = serializeMaterials();
        std::ofstream stream(path);
        stream << json.dump(2);
    }

    nlohmann::json ColorToThicknessDialog::serializeMaterials() const
    {
        nlohmann::json materials = nlohmann::json::array();
        for (auto const & mat : m_materials)
        {
            nlohmann::json entry;
            entry["name"] = mat.name;
            entry["reflectanceColor"] = {mat.reflectanceColor.x(), mat.reflectanceColor.y(), mat.reflectanceColor.z()};
            entry["opacity"] = mat.opacity;
            entry["referenceThickness"] = mat.referenceThickness;
            entry["minThickness"] = mat.minThickness;
            entry["maxThickness"] = mat.maxThickness;
            materials.push_back(entry);
        }
        nlohmann::json root;
        root["materials"] = materials;
        return root;
    }

    void ColorToThicknessDialog::deserializeMaterials(nlohmann::json const & json)
    {
        if (!json.contains("materials") || !json["materials"].is_array())
        {
            return;
        }

        m_materials.clear();
        for (auto const & entry : json["materials"])
        {
            io::FilamentOpticalProperties mat;
            mat.name = entry.value("name", std::string("Material"));
            auto colorArray = entry.value("reflectanceColor", std::vector<float>{1.0F, 1.0F, 1.0F});
            if (colorArray.size() == 3)
            {
                mat.reflectanceColor = Eigen::Vector3f{clamp01(colorArray[0]), clamp01(colorArray[1]), clamp01(colorArray[2])};
            }
            mat.opacity = clamp01(entry.value("opacity", 0.6F));
            mat.referenceThickness = entry.value("referenceThickness", 0.4F);
            mat.minThickness = entry.value("minThickness", 0.0F);
            mat.maxThickness = entry.value("maxThickness", 5.0F);
            if (mat.maxThickness < mat.minThickness)
            {
                mat.maxThickness = mat.minThickness;
            }
            m_materials.push_back(mat);
        }
    }

    void ColorToThicknessDialog::computeThicknessMapping()
    {
        if (m_materials.empty() || m_palette.empty())
        {
            return;
        }

        io::FilamentStack stack{m_materials};
        io::FrontlitThicknessSolver solver{stack, m_constraints};

        m_solutions.clear();
        m_solutions.reserve(m_palette.size());
        for (auto const & color : m_palette)
        {
            auto solution = solver.solve(color);
            m_solutions.push_back(std::move(solution));
        }
    }

} // namespace gladius::ui
