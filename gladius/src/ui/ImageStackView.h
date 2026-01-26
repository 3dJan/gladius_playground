#pragma once

#include "ViewStates.h"
#include "imgui.h"

namespace gladius::io
{
    class ImageStack;
}

namespace gladius::ui
{
    /// View for displaying and navigating ImageStack layers
    class ImageStackView
    {
      public:
        ImageStackView();
        ~ImageStackView();

        /// Set the ImageStack to display
        /// @param stack Pointer to ImageStack, can be nullptr to clear
        void setImageStack(io::ImageStack const * stack);

        /// Get currently selected layer index
        /// @return 0-based layer index
        int getCurrentLayerIndex() const;

        /// Set current layer index
        /// @param index 0-based index, clamped to valid range
        void setCurrentLayerIndex(int index);

        /// Render the view
        /// @return true if any changes were made
        bool render();

        /// Check if view is hovered
        bool isHovered() const;

      private:
        void uploadLayerTexture();
        void cleanupTexture();

        ImageStackViewState m_state;
        io::ImageStack const * m_imageStack = nullptr;
        unsigned int m_layerTexture = 0;
        bool m_hovered = false;
    };
}
