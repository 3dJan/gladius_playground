#include "ColorToThicknessDialog.h"

#include "Widgets.h"
#include "io/3mf/FaceThicknessMapper.h"

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

    void ColorToThicknessDialog::setPaletteRequestHandler(std::function<void()> handler)
    {
        m_paletteRequestHandler = std::move(handler);
    }

    void ColorToThicknessDialog::notifyPaletteDeriveStarted()
    {
        m_paletteBusy = true;
        m_paletteStatus = "Deriving palette from mesh...";
    }

    void ColorToThicknessDialog::notifyPaletteDeriveFailed(std::string message)
    {
        m_paletteBusy = false;
        m_paletteStatus = std::move(message);
    }

    void ColorToThicknessDialog::notifyPaletteDeriveSucceeded(std::vector<Eigen::Vector3f> colors)
    {
        m_paletteBusy = false;
        m_paletteStatus = "Palette derived from mesh.";
        setPaletteColors(std::move(colors));
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
        if (!ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }
        ImGui::Separator();

        ImGui::BeginDisabled(m_fileDialog.isActive());
        if (ImGui::Button("Add material"))
        {
            io::FilamentOpticalProperties filament;
            filament.name = "Material" + std::to_string(m_materials.size() + 1);
            filament.reflectanceColor = Eigen::Vector3f(1.0F, 1.0F, 1.0F);
            filament.transmissionDistance = Eigen::Vector3f(1.0F, 1.0F, 1.0F);
            m_materials.push_back(filament);
            invalidateComputedData();
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

        if (ImGui::BeginTable("MaterialsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0F);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 120.0F);
            ImGui::TableSetupColumn("Trans. dist (mm)", ImGuiTableColumnFlags_WidthFixed, 160.0F);
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
                    invalidateComputedData();
                }

                ImGui::TableSetColumnIndex(3);
                ImGui::SetNextItemWidth(120.0F);
                float tdValue = mat.transmissionDistance.x();
                if (ImGui::InputFloat(("##td" + std::to_string(i)).c_str(), &tdValue, 0.05F, 0.1F, "%.3f"))
                {
                    tdValue = std::max(0.0F, tdValue);
                    mat.transmissionDistance = Eigen::Vector3f{tdValue, tdValue, tdValue};
                    invalidateComputedData();
                }

                ImGui::TableSetColumnIndex(4);
                if (ImGui::Button(("Delete##" + std::to_string(i)).c_str()))
                {
                    m_materials.erase(m_materials.begin() + static_cast<std::ptrdiff_t>(i));
                    if (m_backgroundIndex >= m_materials.size())
                    {
                        m_backgroundIndex = m_materials.empty() ? 0 : m_materials.size() - 1;
                    }
                    invalidateComputedData();
                    --i;
                }
            }

            ImGui::EndTable();
        }

        if (!m_materials.empty())
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Background material (infill/base):");
            ImGui::SameLine();

            if (m_backgroundIndex >= m_materials.size())
            {
                m_backgroundIndex = 0;
            }

            std::vector<const char*> names;
            names.reserve(m_materials.size());
            for (auto const& mat : m_materials)
            {
                names.push_back(mat.name.c_str());
            }

            int selected = static_cast<int>(m_backgroundIndex);
            if (ImGui::Combo("##background", &selected, names.data(), static_cast<int>(names.size())))
            {
                m_backgroundIndex = static_cast<std::size_t>(selected);
                invalidateComputedData();
            }
        }
    }

    void ColorToThicknessDialog::renderPaletteSection()
    {
        if (!ImGui::CollapsingHeader("Palette (target colors)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }
        ImGui::Separator();

        if (m_paletteRequestHandler)
        {
            ImGui::BeginDisabled(m_paletteBusy);
            if (ImGui::Button("Derive from mesh"))
            {
                m_paletteBusy = true;
                m_paletteStatus.clear();
                m_paletteRequestHandler();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (m_paletteBusy)
            {
                ImGui::TextDisabled("Running extraction...");
            }
            else if (!m_paletteStatus.empty())
            {
                ImGui::TextUnformatted(m_paletteStatus.c_str());
            }
        }

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
                invalidateComputedData();
            }
        }

        if (m_palette.empty())
        {
            ImGui::TextDisabled("No palette colors. Add colors to compute thickness mapping.");
            return;
        }

        if (ImGui::BeginTable("PaletteTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 130.0F);
            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 250.0F);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 180.0F);
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
                    m_solutions.clear();
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
        if (!ImGui::CollapsingHeader("Thickness constraints", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }
        ImGui::Separator();

        ImGui::SetNextItemWidth(180.0F);
        static char const * MODE_LABELS[] = {"Frontlit", "Backlit"};
        int modeIndex = (m_illuminationMode == io::IlluminationMode::Frontlit) ? 0 : 1;
        if (ImGui::Combo("Illumination mode", &modeIndex, MODE_LABELS, IM_ARRAYSIZE(MODE_LABELS)))
        {
            m_illuminationMode = (modeIndex == 0) ? io::IlluminationMode::Frontlit : io::IlluminationMode::Backlit;
            invalidateComputedData();
        }

        ImGui::SetNextItemWidth(140.0F);
        if (ImGui::InputFloat("Min thickness (mm)", &m_constraints.minThickness, 0.05F, 0.1F, "%.3f"))
        {
            m_constraints.minThickness = std::max(0.0F, m_constraints.minThickness);
            if (m_constraints.maxThickness < m_constraints.minThickness)
            {
                m_constraints.maxThickness = m_constraints.minThickness;
            }
            invalidateComputedData();
        }

        ImGui::SetNextItemWidth(140.0F);
        if (ImGui::InputFloat("Max thickness (mm)", &m_constraints.maxThickness, 0.1F, 0.5F, "%.3f"))
        {
            m_constraints.maxThickness = std::max(m_constraints.maxThickness, m_constraints.minThickness);
            invalidateComputedData();
        }

        ImGui::SetNextItemWidth(140.0F);
        if (ImGui::InputFloat("Layer height (mm, optional)", &m_constraints.layerHeight, 0.01F, 0.05F, "%.3f"))
        {
            invalidateComputedData();
        }

        ImGui::SetNextItemWidth(140.0F);
        if (ImGui::InputFloat("Total max thickness (mm, optional)", &m_constraints.totalMaxThickness, 0.5F, 1.0F, "%.3f"))
        {
            invalidateComputedData();
        }

        ImGui::SetNextItemWidth(140.0F);
        if (ImGui::InputInt("LUT resolution (per axis)", &m_lutResolution))
        {
            m_lutResolution = std::clamp(m_lutResolution, 2, 64); // keep sane bounds
            invalidateComputedData();
        }

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

        ImGui::Separator();
        bool const canBuildLut = !m_materials.empty();
        ImGui::BeginDisabled(!canBuildLut);
        if (ImGui::Button("Generate thickness LUT"))
        {
            computePrecomputedLuts();
        }
        ImGui::EndDisabled();
        if (!canBuildLut)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Define materials first.");
        }
        if (!m_lutStatus.empty())
        {
            ImGui::TextDisabled("%s", m_lutStatus.c_str());
        }
    }

    void ColorToThicknessDialog::renderResultsSection()
    {
        if (!ImGui::CollapsingHeader("Thickness results", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }
        ImGui::Separator();

        if (m_solutions.empty())
        {
            ImGui::TextDisabled("No results yet. Compute to see thickness mapping.");
            return;
        }

        auto const orderedMaterials = getOrderedShellMaterials();

        int const columnCount = static_cast<int>(orderedMaterials.stack.size()) + 3;
        if (ImGui::BeginTable("ResultsTable", columnCount, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 130.0F);
            ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 190.0F);
            for (std::size_t orderedIndex = 0; orderedIndex < orderedMaterials.stack.size(); ++orderedIndex)
            {
                auto const & mat = orderedMaterials.stack[orderedIndex];
                std::string columnLabel = mat.name;
                if (orderedIndex == 0U)
                {
                    columnLabel += " (inner)";
                }
                else if (orderedIndex + 1U == orderedMaterials.stack.size())
                {
                    columnLabel += " (outer)";
                }
                ImGui::TableSetupColumn(columnLabel.c_str(), ImGuiTableColumnFlags_WidthFixed, 200.0F);
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
                Eigen::Vector3f const targetDisplay = io::linearToSrgb(
                    solution.targetColor.cwiseMax(0.0F).cwiseMin(1.0F));
                ImVec4 targetColor{targetDisplay.x(), targetDisplay.y(), targetDisplay.z(), 1.0F};
                colorButtonWithTooltip(("##target" + std::to_string(row)).c_str(), targetColor, "Target color");
                ImGui::SameLine();
                Eigen::Vector3f const achievedDisplay = io::linearToSrgb(
                    solution.achievedColor.cwiseMax(0.0F).cwiseMin(1.0F));
                ImVec4 achievedColor{achievedDisplay.x(), achievedDisplay.y(), achievedDisplay.z(), 1.0F};
                colorButtonWithTooltip(("##achieved" + std::to_string(row)).c_str(), achievedColor, "Achieved color");

                for (std::size_t orderedIndex = 0; orderedIndex < orderedMaterials.stack.size(); ++orderedIndex)
                {
                    ImGui::TableSetColumnIndex(static_cast<int>(2 + orderedIndex));
                    float thickness = 0.0F;
                    if (orderedIndex < orderedMaterials.orderedToOriginal.size())
                    {
                        std::size_t const originalIndex = orderedMaterials.orderedToOriginal[orderedIndex];
                        if (originalIndex < solution.thicknesses.size())
                        {
                            thickness = solution.thicknesses[originalIndex];
                        }
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

    std::string const & ColorToThicknessDialog::getLutStatus() const
    {
        return m_lutStatus;
    }

    io::FilamentStack ColorToThicknessDialog::getFilamentStack() const
    {
        return getOrderedShellMaterials().stack;
    }

    bool ColorToThicknessDialog::ensurePrecomputedLuts()
    {
        if (!m_precomputedLuts.empty())
        {
            return true;
        }

        computePrecomputedLuts();
        return !m_precomputedLuts.empty();
    }

    void ColorToThicknessDialog::computePrecomputedLuts()
    {
        m_lutStatus.clear();

        if (m_materials.empty() || m_lutResolution < 2)
        {
            m_lutStatus = "Cannot build LUT: need materials and resolution >= 2.";
            return;
        }

        auto const orderedMaterials = getOrderedShellMaterials();
        io::FrontlitThicknessSolver solver{
            orderedMaterials.stack,
            m_constraints,
            m_illuminationMode,
            orderedMaterials.backgroundIndex};

        std::size_t const layerCount = orderedMaterials.stack.size();
        std::size_t const lutSize = static_cast<std::size_t>(m_lutResolution) *
                                   static_cast<std::size_t>(m_lutResolution) *
                                   static_cast<std::size_t>(m_lutResolution);

                    std::vector<std::vector<float>> newLuts;
                    newLuts.resize(layerCount);

        float const denom = static_cast<float>(m_lutResolution - 1);

        // Build one cumulative-thickness LUT per layer (bottom-to-top indexing)
        for (std::size_t startLayer = 0; startLayer < layerCount; ++startLayer)
        {
            auto & lut = newLuts[startLayer];
            lut.resize(lutSize, 0.0F);

            auto lutIndex = [this](int r, int g, int b) -> std::size_t
            {
                return (static_cast<std::size_t>(r) * static_cast<std::size_t>(m_lutResolution) +
                        static_cast<std::size_t>(g)) * static_cast<std::size_t>(m_lutResolution) +
                       static_cast<std::size_t>(b);
            };

            for (int r = 0; r < m_lutResolution; ++r)
            {
                for (int g = 0; g < m_lutResolution; ++g)
                {
                    for (int b = 0; b < m_lutResolution; ++b)
                    {
                        Eigen::Vector3f const color{
                            static_cast<float>(r) / denom,
                            static_cast<float>(g) / denom,
                            static_cast<float>(b) / denom};

                        io::ThicknessSolution const solution = solver.solve(color);

                        float cumulative = 0.0F;
                        for (std::size_t layer = startLayer;
                             layer < solution.thicknesses.size();
                             ++layer)
                        {
                            cumulative += solution.thicknesses[layer];
                        }

                        lut[lutIndex(r, g, b)] = cumulative;
                    }
                }
            }
        }

        m_precomputedLuts = std::move(newLuts);
        m_lutStatus = "Thickness LUT generated.";
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
            entry["transmissionDistance"] = mat.transmissionDistance.x();
            materials.push_back(entry);
        }
        nlohmann::json root;
        root["materials"] = materials;
        root["illuminationMode"] = (m_illuminationMode == io::IlluminationMode::Frontlit) ? "frontlit" : "backlit";
        root["backgroundIndex"] = m_backgroundIndex;
        root["lutResolution"] = m_lutResolution;
        if (!m_precomputedLuts.empty())
        {
            root["precomputedLuts"] = m_precomputedLuts;
        }
        return root;
    }

    void ColorToThicknessDialog::deserializeMaterials(nlohmann::json const & json)
    {
        m_solutions.clear();
        m_lutStatus.clear();

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
            if (entry.contains("transmissionDistance"))
            {
                if (entry["transmissionDistance"].is_number_float() || entry["transmissionDistance"].is_number_integer())
                {
                    float td = std::max(0.0F, entry["transmissionDistance"].get<float>());
                    mat.transmissionDistance = Eigen::Vector3f{td, td, td};
                }
                else if (entry["transmissionDistance"].is_array())
                {
                    auto tdArray = entry.value("transmissionDistance", std::vector<float>{0.0F, 0.0F, 0.0F});
                    if (tdArray.size() == 3)
                    {
                        float td = std::max(0.0F, tdArray[0]);
                        mat.transmissionDistance = Eigen::Vector3f{td, td, td};
                    }
                }
            }
            m_materials.push_back(mat);
        }

        std::string const modeString = json.value("illuminationMode", std::string("frontlit"));
        if (modeString == "backlit")
        {
            m_illuminationMode = io::IlluminationMode::Backlit;
        }
        else
        {
            m_illuminationMode = io::IlluminationMode::Frontlit;
        }

        m_backgroundIndex = json.value("backgroundIndex", 0u);
        if (m_materials.empty())
        {
            m_backgroundIndex = 0;
        }
        else if (m_backgroundIndex >= m_materials.size())
        {
            m_backgroundIndex = m_materials.size() - 1;
        }

        // Optional: restore LUT resolution and precomputed LUTs
        m_lutResolution = std::clamp(json.value("lutResolution", m_lutResolution), 2, 64);
        if (json.contains("precomputedLuts") && json["precomputedLuts"].is_array())
        {
            try
            {
                m_precomputedLuts = json["precomputedLuts"].get<std::vector<std::vector<float>>>();
            }
            catch (...)
            {
                // If parsing fails, leave LUTs empty
                m_precomputedLuts.clear();
            }
        }
    }

    void ColorToThicknessDialog::computeThicknessMapping()
    {
        if (m_materials.empty() || m_palette.empty())
        {
            return;
        }

        auto const orderedMaterials = getOrderedShellMaterials();
        io::FrontlitThicknessSolver solver{
            orderedMaterials.stack,
            m_constraints,
            m_illuminationMode,
            orderedMaterials.backgroundIndex};

        m_solutions.clear();
        m_solutions.reserve(m_palette.size());
        for (auto const & color : m_palette)
        {
            auto solution = solver.solve(io::srgbToLinear(color.cwiseMax(0.0F).cwiseMin(1.0F)));
            solution.thicknesses = remapThicknessesToUiOrder(
                solution.thicknesses,
                orderedMaterials.orderedToOriginal,
                m_materials.size());
            m_solutions.push_back(std::move(solution));
        }
    }

    io::OrderedShellMaterials ColorToThicknessDialog::getOrderedShellMaterials() const
    {
        io::FilamentStack stack{m_materials};

        // For small frontlit stacks with a known palette, prefer the palette-optimized
        // global order over the generic translucency heuristic. The exhaustive search is
        // cheap for small material counts and the benchmark showed it meaningfully
        // reduces reproduction error for typical CMY-style stacks.
        if (m_illuminationMode == io::IlluminationMode::Frontlit
            && !m_palette.empty()
            && m_materials.size() <= 6U)
        {
            return io::ShellMaterialOrdering::optimizeGlobalOrderForPalette(
                stack,
                m_backgroundIndex,
                m_illuminationMode,
                m_constraints,
                m_palette);
        }

        return io::ShellMaterialOrdering::reorderForShells(stack, m_backgroundIndex, m_illuminationMode);
    }

    std::vector<float> ColorToThicknessDialog::remapThicknessesToUiOrder(
        std::vector<float> const& orderedThicknesses,
        std::vector<std::size_t> const& orderedToOriginal,
        std::size_t materialCount)
    {
        std::vector<float> remapped(materialCount, 0.0F);
        for (std::size_t orderedIndex = 0; orderedIndex < orderedThicknesses.size() &&
                                           orderedIndex < orderedToOriginal.size();
             ++orderedIndex)
        {
            std::size_t const originalIndex = orderedToOriginal[orderedIndex];
            if (originalIndex < remapped.size())
            {
                remapped[originalIndex] = orderedThicknesses[orderedIndex];
            }
        }
        return remapped;
    }

    void ColorToThicknessDialog::invalidateComputedData(bool clearPaletteStatus)
    {
        m_solutions.clear();
        m_precomputedLuts.clear();
        m_lutStatus.clear();
        if (clearPaletteStatus)
        {
            m_paletteStatus.clear();
        }
    }

} // namespace gladius::ui
