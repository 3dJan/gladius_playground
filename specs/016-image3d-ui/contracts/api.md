# API Contracts: Image3D & FunctionFromImage3D UI

**Date**: 2026-01-26  
**Branch**: `016-image3d-ui`

## Internal C++ Interfaces

### ImageStackView

```cpp
namespace gladius::ui
{
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
        ImageStackViewState m_state;
        io::ImageStack const * m_imageStack = nullptr;
        GLuint m_layerTexture = 0;
    };
}
```

### FunctionFromImage3DView

```cpp
namespace gladius::ui
{
    class FunctionFromImage3DView
    {
    public:
        FunctionFromImage3DView();
        ~FunctionFromImage3DView();
        
        /// Set the function model to configure
        /// @param model Function model containing ImageSampler
        /// @param assembly Assembly containing resources
        void setFunction(nodes::Model * model, nodes::Assembly * assembly);
        
        /// Set ModelEditor for undo integration
        void setModelEditor(ModelEditor * editor);
        
        /// Render the configuration panel
        /// @return true if any settings were changed
        bool render();
        
        /// Force preview update
        void invalidatePreview();
        
    private:
        // Configuration accessors
        SamplingFilter getFilter() const;
        void setFilter(SamplingFilter filter);
        
        TextureTileStyle getTileStyleU() const;
        TextureTileStyle getTileStyleV() const;
        TextureTileStyle getTileStyleW() const;
        void setTileStyle(int axis, TextureTileStyle style);
        
        float getOffset() const;
        void setOffset(float offset);
        
        float getScale() const;
        void setScale(float scale);
        
        ResourceId getImageStackId() const;
        void setImageStackId(ResourceId id);
        
        // Internal helpers
        nodes::ImageSampler * findImageSampler();
        void renderPreview();
        void updatePreviewTexture();
        
        FunctionFromImage3DViewState m_state;
        nodes::Model * m_function = nullptr;
        nodes::Assembly * m_assembly = nullptr;
        ModelEditor * m_modelEditor = nullptr;
    };
}
```

### ImageStack Transform Methods

```cpp
namespace gladius::io
{
    class ImageStack
    {
    public:
        // ... existing methods ...
        
        /// Flip all layers horizontally (mirror along X axis)
        void flipHorizontal();
        
        /// Flip all layers vertically (mirror along Y axis)
        void flipVertical();
        
        /// Rotate all layers 90° clockwise
        /// @note Updates width/height if images are rectangular
        void rotate90CW();
        
        /// Rotate all layers 90° counter-clockwise
        /// @note Updates width/height if images are rectangular
        void rotate90CCW();
        
        /// Get layer by index
        /// @param index 0-based layer index
        /// @throws std::out_of_range if index >= size()
        Image const & at(size_t index) const;
        Image & at(size_t index);
    };
}
```

### Image Transform Methods

```cpp
namespace gladius::io
{
    class Image
    {
    public:
        // ... existing methods ...
        
        /// Flip image horizontally (mirror along X axis)
        void flipHorizontal();
        
        /// Flip image vertically (mirror along Y axis)
        void flipVertical();
        
        /// Rotate image 90° clockwise
        /// @note Swaps width and height
        void rotate90CW();
        
        /// Rotate image 90° counter-clockwise
        /// @note Swaps width and height
        void rotate90CCW();
        
        /// Pad image to new dimensions
        /// @param newWidth Target width (must be >= current width)
        /// @param newHeight Target height (must be >= current height)
        /// @param padValue Pixel value for padding (default: 0)
        void padTo(unsigned int newWidth, unsigned int newHeight, unsigned char padValue = 0);
    };
}
```

### ModelEditor Extensions

```cpp
namespace gladius::ui
{
    class ModelEditor
    {
    public:
        // ... existing methods ...
        
        /// Check if current model is a FunctionFromImage3D
        /// @return true if contains ImageSampler node structure
        bool isFunctionFromImage3D() const;
        
        /// Get current tab mode
        enum class TabMode { Properties, Graph };
        TabMode getTabMode() const;
        
        /// Set current tab mode
        void setTabMode(TabMode mode);
    };
}
```

## Event/Callback Contracts

### Import Progress Callback

```cpp
/// Callback signature for import progress
/// @param current Current file being processed (1-based)
/// @param total Total number of files
/// @param filename Current filename being processed
/// @return false to cancel import
using ImportProgressCallback = std::function<bool(int current, int total, std::string const& filename)>;

class ImageStackCreator
{
public:
    /// Import with progress reporting
    Lib3MF::PImageStack addImageStackFromDirectory(
        Lib3MF::PModel model,
        std::filesystem::path const & path,
        ImportProgressCallback progress = nullptr);
};
```

### Configuration Change Notification

```cpp
/// Signal emitted when FunctionFromImage3D configuration changes
/// Existing pattern: uses ModelEditor::markModelAsModified()
/// and triggers recompile via m_parameterDirty flag
```

## UI Widget Specifications

### Layer Slider

- **Type**: ImGui::SliderInt
- **Range**: 1 to layerCount (1-based display)
- **Format**: "Layer %d of %d"
- **Behavior**: Clamp, no wrap-around

### Filter Dropdown

- **Type**: ImGui::Combo
- **Options**: ["Linear", "Nearest"]
- **Default**: Linear (index 1)

### Tile Style Dropdown (per axis)

- **Type**: ImGui::Combo  
- **Options**: ["Wrap", "Mirror", "Clamp"]
- **Default**: Wrap (index 0)
- **Labels**: "Tile Style U", "Tile Style V", "Tile Style W"

### Offset/Scale Inputs

- **Type**: ImGui::DragFloat
- **Speed**: 0.01f
- **Format**: "%.3f"
- **Range**: No explicit limits (allows negative offset)
