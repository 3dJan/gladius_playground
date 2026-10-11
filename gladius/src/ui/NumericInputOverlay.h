#pragma once

#include <imgui.h>

#include <optional>
#include <string>
#include <string_view>

namespace gladius::ui
{
    enum class NumericConstant
    {
        Pi,
        Euler,
        SqrtTwo,
        Tau,
        GoldenRatio,
        SqrtThree,
        LnTwo,
        LnTen
    };

      enum class NumericInputOverlayAction
      {
        None,
        Apply,
        Cancel
      };

      struct NumericInputOverlayUpdate
      {
        /// Live value to write to the target, or the original value when cancelling.
        std::optional<float> value;
        NumericInputOverlayAction action{NumericInputOverlayAction::None};
      };

    namespace numeric_input_detail
    {
        /// Parse a complete finite floating-point value.
        [[nodiscard]] std::optional<float> parseFloat(std::string_view text);

        /// Format a value compactly while retaining enough precision for float edits.
        [[nodiscard]] std::string formatFloat(float value);

        /// Choose an initial minor ruler step by value magnitude, favoring powers of ten.
        [[nodiscard]] float initialStep(float value);

        /// Align a ruler's major ticks to the nearest absolute multiple of ten minor steps.
        [[nodiscard]] double rulerOffsetForValue(float value, float step);

        /// Scale a finite value by a number of decimal decades, clamping at float limits.
        [[nodiscard]] float scaleValueByDecade(float value, int decadeDelta);

        /// Change a positive ruler step by a number of decimal decades.
        [[nodiscard]] float scaleStepByDecade(float step, int decadeDelta);

        /// Apply one signed nudge without overflowing the finite float range.
        [[nodiscard]] float nudgeValue(float value, float step, int direction);

        /// Return a constant in display units; angle values are displayed in degrees.
        [[nodiscard]] float constantValue(NumericConstant constant, bool angleInDegrees);

        [[nodiscard]] float degreesToRadians(float degrees);
        [[nodiscard]] float radiansToDegrees(float radians);
    } // namespace numeric_input_detail

    /// A transient, touch-friendly numeric editor that reports valid changes immediately.
    class NumericInputOverlay
    {
      public:
        void open(std::string label,
                  float value,
                  bool angleInDegrees,
                  ImVec2 anchorPosition,
                  float uiScale);

        /// Draw the overlay and return a changed value in display units, if edited this frame.
        [[nodiscard]] NumericInputOverlayUpdate draw(bool targetAvailable);

        [[nodiscard]] bool isActive() const;
        void close();

      private:
        void setValue(float value);
        void setRulerValue(float value, double offsetTicks);
        void appendDigit(char digit);
        void appendDecimal();
        void toggleSign();
        void eraseLastCharacter();
        void refreshValueFromText();

        bool m_isActive{false};
        bool m_angleInDegrees{false};
        bool m_hasValidValue{false};
        bool m_valueChangedThisFrame{false};
        bool m_focusInputOnOpen{false};
        bool m_moreMenuOpen{false};
        std::string m_label;
        std::string m_inputText;
        float m_initialValue{0.f};
        float m_value{0.f};
        float m_step{0.01f};
        float m_uiScale{1.f};
        ImVec2 m_anchorPosition{};
        float m_rulerDragStartMouseX{0.f};
        float m_rulerDragStartValue{0.f};
        float m_rulerDragStartStep{0.01f};
        double m_rulerOffsetTicks{0.0};
        double m_rulerDragStartOffsetTicks{0.0};
        std::optional<float> m_rulerTapValue;
        bool m_rulerWasDragged{false};
    };
} // namespace gladius::ui
