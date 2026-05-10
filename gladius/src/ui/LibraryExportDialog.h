#pragma once

#include "../Document.h"
#include "../EventLogger.h"

#include <filesystem>
#include <string>
#include <vector>

namespace gladius::ui
{
    /// @brief Modal dialog for exporting a function to the library as a .3mf file
    ///        with gladius metadata (library-functions + library-description).
    class LibraryExportDialog
    {
      public:
        LibraryExportDialog() = default;

        /// @brief Open the dialog for the given document and library root directory.
        /// @param doc The current document (must be non-null).
        /// @param libraryRoot Absolute path to the library root directory.
        void open(SharedDocument doc, std::filesystem::path libraryRoot);

        /// @brief Render the modal dialog. Call once per frame.
        void render();

        /// @brief Returns true exactly once after a successful export.
        [[nodiscard]] bool wasExportCompleted();

        /// @brief Returns true exactly once after an export error occurred.
        [[nodiscard]] bool hadError();

      private:
        void performExport();
        void populateFunctionList();
        void populateCategoryList();

        SharedDocument m_doc;
        events::SharedLogger m_logger;
        std::filesystem::path m_libraryRoot;

        bool m_isOpen = false;
        bool m_exportCompleted = false;
        bool m_exportError = false;

        /// Cached list of exportable functions.
        struct FunctionEntry
        {
            std::string displayName;
            nodes::SharedModel model;
            ResourceId resourceId = 0;
        };
        std::vector<FunctionEntry> m_functions;
        int m_selectedFunctionIndex = 0;

        /// Available category sub-directories.
        std::vector<std::string> m_categories;
        int m_selectedCategoryIndex = 0;

        /// Input buffers for ImGui text inputs.
        static constexpr size_t DESCRIPTION_BUF_SIZE = 256;
        static constexpr size_t FILENAME_BUF_SIZE = 128;
        static constexpr size_t CATEGORY_BUF_SIZE = 128;

        char m_descriptionBuf[DESCRIPTION_BUF_SIZE] = {};
        char m_fileNameBuf[FILENAME_BUF_SIZE] = {};
        char m_newCategoryBuf[CATEGORY_BUF_SIZE] = {};
        bool m_useNewCategory = false;

        /// Overwrite-confirmation state.
        bool m_showOverwriteConfirm = false;
        std::filesystem::path m_targetPath;
    };

} // namespace gladius::ui
