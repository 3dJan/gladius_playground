#pragma once

#include "../nodes/nodesfwd.h"
#include "../nodes/types.h"

#include <string>
#include <unordered_map>

namespace gladius::nodes
{
    class Model;
    class Assembly;
} // namespace gladius::nodes

namespace gladius::ui
{
    /// Per-function editor buffer state for the Code tab.
    struct CodeBuffer
    {
        std::string buffer;     ///< Current editor text
        std::string syncedText; ///< Text at last successful sync (for dirty detection)
        bool generated{false};  ///< Whether code has been generated for this function
    };

    /// Code editor widget: displays a GLSL-like snippet for a function graph
    /// and allows editing + syncing back to the graph.
    class CodeView
    {
      public:
        /// Set the current function to display/edit.
        void setFunction(nodes::ResourceId functionId,
                         nodes::Model * model,
                         nodes::Assembly * assembly);

        /// Render the code editor and sync button. Returns true if a sync was performed.
        bool render();

        /// Check if there are unsaved changes for the current function.
        [[nodiscard]] bool hasUnsavedChanges() const;

        /// Discard unsaved changes (revert to last synced text).
        void discardChanges();

      private:
        bool syncToGraph(CodeBuffer & buf);

        std::unordered_map<nodes::ResourceId, CodeBuffer> m_buffers;
        nodes::ResourceId m_currentFunctionId{0};
        nodes::Model * m_currentModel{nullptr};
        nodes::Assembly * m_currentAssembly{nullptr};
        std::string m_lastError;
    };

} // namespace gladius::ui
