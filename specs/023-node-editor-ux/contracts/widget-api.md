# Contracts: Node Editor UX Improvements (023)

This feature is a desktop C++ GUI application using ImGui — there are no REST/GraphQL APIs. Contracts are defined as C++ interface/class signatures for the new components.

## 1. NumericWidget API

```cpp
namespace gladius::ui
{
    enum class WidgetLayoutMode
    {
        DialPlusDragFloat, ///< Orbital dial paired with drag-float (default)
        Slider             ///< Linear slider with value label
    };

    struct NumericWidgetParams
    {
        float * value = nullptr;
        ContentType contentType = ContentType::Length;
        WidgetLayoutMode layoutMode = WidgetLayoutMode::DialPlusDragFloat;
        std::optional<float> minValue;
        std::optional<float> maxValue;
        float dragSensitivity = 1.0f; ///< Base sensitivity multiplier
    };

    /// Renders an enhanced numeric widget (dial+drag-float or slider).
    /// Returns true if the value was changed.
    bool numericWidget(char const * label, NumericWidgetParams & params);

    /// Renders an orbital dial knob.
    /// Returns true if the value was changed by dial interaction.
    bool orbitalDial(char const * label,
                     float * value,
                     float radius,
                     std::optional<float> minValue = std::nullopt,
                     std::optional<float> maxValue = std::nullopt);

    /// Renders an enhanced drag-float with adaptive sensitivity.
    /// Supports Shift (fine), Ctrl (coarse) modifier keys.
    /// Returns true if the value was changed.
    bool adaptiveDragFloat(char const * label,
                           float * value,
                           ContentType contentType = ContentType::Length);
}
```

## 2. LinkDragState API

```cpp
namespace gladius::ui
{
    struct LinkDragState
    {
        bool isDragging = false;
        nodes::PortId sourcePortId{0};
        std::type_index sourcePortType{typeid(void)};
        bool sourceIsOutput = false;
        std::unordered_set<int64_t> compatiblePorts;

        /// Recompute compatible ports from the model for the current source port.
        void computeCompatibility(nodes::Model const & model);

        /// Check if a specific port/parameter is compatible with the current drag source.
        [[nodiscard]] bool isCompatible(int64_t portOrParamId) const;

        /// Reset to idle state.
        void reset();
    };
}
```

## 3. Node Rendering Extensions

```cpp
namespace gladius::ui
{
    struct NodeRenderConfig
    {
        float borderWidth = 4.0f;   ///< Category ring thickness
        float rounding = 20.0f;     ///< Corner rounding radius
        bool showCategoryIcon = false;
    };

    /// Push enhanced node styling (rounding, border width, category color).
    /// Must be paired with popNodeStyle().
    void pushNodeStyle(NodeRenderConfig const & config, ImVec4 categoryColor);
    void popNodeStyle();

    /// Compute the minimum node width that avoids clipping content.
    float computeMinNodeWidth(nodes::NodeBase const & node, float uiScale);
}
```

## 4. Parameter Throttle API

```cpp
namespace gladius::ui
{
    class ParameterThrottle
    {
      public:
        explicit ParameterThrottle(std::chrono::milliseconds debounceInterval = std::chrono::milliseconds(100));

        /// Called when a parameter value changes. Returns true if a recompile should be triggered now.
        bool onParameterChanged();

        /// Called each frame. Returns true if debounce expired and a pending recompile should fire.
        bool shouldRecompile();

        /// Reset the throttle state.
        void reset();

      private:
        std::chrono::steady_clock::time_point m_lastChangeTime;
        std::chrono::milliseconds m_debounceInterval;
        bool m_pendingRecompile = false;
    };
}
```

## 5. Begin/End Node Argument Management API

```cpp
namespace gladius::ui
{
    /// Renders the enhanced begin/end node argument table with
    /// inline editing, reordering, and removal with confirmation.
    /// Returns true if arguments were modified.
    bool renderArgumentTable(nodes::Begin & beginNode,
                             nodes::Assembly & assembly,
                             float uiScale);

    bool renderOutputTable(nodes::End & endNode,
                           nodes::Assembly & assembly,
                           float uiScale);
}
```

## 6. Port Compatibility Highlighting API

```cpp
namespace gladius::ui
{
    /// Render a port pin with link-drag-aware highlighting.
    /// When linkDrag is active, compatible ports get glow/highlight,
    /// incompatible ports are dimmed.
    void renderPortPin(nodes::PortId portId,
                       std::type_index portType,
                       ed::PinKind kind,
                       LinkDragState const & linkDrag,
                       float uiScale);
}
```
