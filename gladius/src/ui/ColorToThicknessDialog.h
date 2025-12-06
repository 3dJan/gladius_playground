#pragma once

#include "FileDialogService.h"
#include "io/3mf/FilamentOpticalProperties.h"
#include "io/3mf/FrontlitThicknessSolver.h"

#include <eigen3/Eigen/Core>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace gladius::ui
{
    /**
     * @brief Dialog for experimenting with color → shell thickness mapping (HueForge-style)
     */
    class ColorToThicknessDialog
    {
      public:
        /// Open the dialog (will show on next render call)
        void open();

        /// Explicitly show/hide the dialog
        void setVisible(bool visible);

        /// Check if the dialog is visible
        [[nodiscard]] bool isVisible() const;

        /// Set the palette colors to display (linear RGB [0,1])
        void setPaletteColors(std::vector<Eigen::Vector3f> colors);

        /// Provide a callback to request palette derivation (async)
        void setPaletteRequestHandler(std::function<void()> handler);

        /// Notify dialog that palette derivation started
        void notifyPaletteDeriveStarted();

        /// Notify dialog that palette derivation failed
        void notifyPaletteDeriveFailed(std::string message);

        /// Notify dialog that palette derivation succeeded
        void notifyPaletteDeriveSucceeded(std::vector<Eigen::Vector3f> colors);

        /// Render the dialog contents (call each frame)
        void render();

      private:
        enum class FileMode
        {
            None,
            Load,
            Save
        };

        void renderMaterialsSection();
        void renderPaletteSection();
        void renderConstraintsSection();
        void renderResultsSection();
        void handleFileDialogResult();

        void loadMaterialsFromFile(std::filesystem::path const & path);
        void saveMaterialsToFile(std::filesystem::path const & path) const;

        [[nodiscard]] nlohmann::json serializeMaterials() const;
        void deserializeMaterials(nlohmann::json const & json);

        void computeThicknessMapping();

        bool m_visible = false;
        AsyncFileDialog m_fileDialog;
        FileMode m_pendingFileMode = FileMode::None;
        std::filesystem::path m_materialsFile;

        std::function<void()> m_paletteRequestHandler;
        bool m_paletteBusy = false;
        std::string m_paletteStatus;

        std::vector<io::FilamentOpticalProperties> m_materials;
        std::vector<Eigen::Vector3f> m_palette;
        std::vector<io::ThicknessSolution> m_solutions;
        io::ThicknessConstraints m_constraints{0.2F, 5.0F, 0.0F, 0.0F};
    };

} // namespace gladius::ui
