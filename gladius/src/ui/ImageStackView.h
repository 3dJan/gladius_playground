#pragma once

#include "ViewStates.h"
#include "imgui.h"

#include <functional>

namespace gladius::io
{
    class ImageStack;
}

namespace gladius::ui
{
    /// Transform operation types for ImageStack
    enum class ImageStackTransform
    {
        FlipHorizontal,
        FlipVertical,
        Rotate90CW,
        Rotate90CCW
    };

    /// Callback invoked when user requests a transform
    /// @param transform The transform operation to apply
    using TransformCallback = std::function<void(ImageStackTransform)>;

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

        /// Set callback for transform requests
        /// The callback should handle undo, apply the transform, and call invalidateTexture()
        void setTransformCallback(TransformCallback callback);

        /// Render the view
        /// @return true if any changes were made
        bool render();

        /// Check if view is hovered
        bool isHovered() const;

        /// Mark texture as needing refresh (T061)
        void invalidateTexture();

      private:
        void uploadLayerTexture();
        void cleanupTexture();
        void renderTransformButtons();

        ImageStackViewState m_state;
        io::ImageStack const * m_imageStack = nullptr;
        unsigned int m_layerTexture = 0;
        bool m_hovered = false;
        TransformCallback m_transformCallback;
    };
}
