#pragma once

#include "Document.h"
#include "ExportState.h"
#include "FileDialogService.h"
#include <memory>

namespace Lib3MF
{
    class CModel;
    typedef std::shared_ptr<CModel> PModel;
}

namespace gladius::ui
{
    /**
     * @brief Class for displaying and managing beam lattice resources
     */
    class BeamLatticeView
    {
      public:
        /**
         * @brief Renders the beam lattice properties in the outline view
         *
         * @param document The document containing the beam lattice resources
         *
         * @return true if the beam lattice properties were modified
         */
        bool render(SharedDocument document);

        /// @brief Set the export state for blocking UI modifications during export
        void setExportState(ExportState * state)
        {
            m_exportState = state;
        }

      private:
        void renderImportDialog(SharedDocument document, bool & propertiesChanged);

        /**
         * @brief Gets the name of a beam lattice resource
         *
         * @param key The resource key to get the name for
         *
         * @return std::string The display name of the beam lattice
         */
        static std::string getBeamLatticeName(const ResourceKey & key);

        // Dialog state for importing STL as beam lattice
        bool m_showImportDialog = false;
        std::string m_filename;
        float m_beamDiameter = 2.0f;

        // Async file dialog for browsing STL files
        AsyncFileDialog m_asyncFileDialog;

        // Export state for blocking UI modifications
        ExportState * m_exportState{nullptr};
    };
}
