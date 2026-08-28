#pragma once

#include "Contour.h"
#include "compute/RenderContracts.h"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cstdint>
#include <future>
#include <optional>

namespace gladius
{
    class ComputeCore;
    class Document;
    class GLView;
}

namespace gladius::ui
{

    struct DistanceMeasurement
    {
        std::optional<Vector2> start;
        std::optional<Vector2> end;
        bool measurementInProgress = false;
    };

    class SliceView
    {
      public:
                ~SliceView();

        void show();
        void hide();
        bool isVisible() const;
        /// \returns Returns true, if the window was rendered
        [[nodiscard]] bool render(ComputeCore & core, GLView & view);

        /// Backend-neutral variant for builds/drivers without an OpenCL core.
        /// Contours are generated through Document::generateContourWebGpu.
        /// \returns Returns true, if the window was rendered
        [[nodiscard]] bool render(Document & doc, GLView & view);

        /// Set the slice height in mm used by the document-based (WebGPU) path
        void setSliceHeight(float zHeight_mm);

        /// Set the authoritative bounds used to limit WebGPU contour sampling.
        void setModelBounds(std::optional<compute::RenderBounds> modelBounds);

        /// Drop cached contours so they are regenerated on the next frame
        void invalidateContours();

        /**
         * @brief Check if mouse is hovering over the slice view
         * @return true if the slice view is being hovered
         */
        bool isHovered() const;

        /// @brief Return whether an asynchronous WebGPU contour request is still running.
        [[nodiscard]] bool isSlicingInProgress() const;

        /// @brief Wait for an asynchronous contour request before dependent state is destroyed.
        void waitForPendingContourGeneration();

        /**
         * @brief Zoom in the slice view
         */
        void zoomIn();

        /**
         * @brief Zoom out the slice view
         */
        void zoomOut();

        /**
         * @brief Reset the slice view to default position and zoom
         */
        void resetView();

        /**
         * @brief Center the view on the current contour and zoom to fit
         */
        void centerView();

      private:
        bool m_visible{false};

        /// Common render implementation shared by both backends. When @p core is
        /// nullptr, contours are generated via the document (WebGPU) instead.
        [[nodiscard]] bool renderImpl(ComputeCore * core, GLView & view);

        /// Slice height used by the document-based (WebGPU) contour path
        float m_sliceZ_mm{10.f};
        /// True while the cached contours match the last WebGPU generation parameters
        bool m_webGpuCacheValid{false};
        /// Time of the last WebGPU contour generation (throttles slider dragging)
        std::chrono::steady_clock::time_point m_lastWebGpuGeneration{};
        /// Input values used for the cached WebGPU contours
        float m_webGpuCachedSliceZ_mm = 0.0f;
        float m_webGpuCachedMinFeatureSize_mm = 0.0f;
        bool m_webGpuCachedAdaptiveContour = true;
        uint64_t m_webGpuCachedStructuralEditEpoch = 0;
        float m_webGpuPendingSliceZ_mm = 0.0f;
        float m_webGpuPendingMinFeatureSize_mm = 0.0f;
        bool m_webGpuPendingAdaptiveContour = true;
        uint64_t m_webGpuPendingStructuralEditEpoch = 0;
        uint64_t m_webGpuPendingBoundsGeneration = 0;
        std::future<PolyLines> m_webGpuContourFuture;
        /// Document used by the document-based (WebGPU) contour path
        gladius::Document * m_document{nullptr};
        std::optional<compute::RenderBounds> m_modelBounds;
        uint64_t m_modelBoundsGeneration = 0;

        float m_zoom = 4.f;
        float m_zoomTarget = 4.f;
        ImVec2 m_scrolling = {0.0f, 250.0f};
        ImVec2 m_origin = {};

        DistanceMeasurement m_distanceMeasurement;

        bool m_renderNormals = false;
        bool m_renderSourceVertices = false;
        bool m_showJumps = false;
        bool m_showSelfIntersections = false;
        bool m_hideDeveloperTools = false;
        bool m_useAdaptiveContour = true;        ///< Toggle for quadtree-based contour extraction
        float m_minFeatureSize_mm = 0.2f;        ///< Min feature size for adaptive contour (mm)

        std::optional<PolyLines> m_contours;
        bool m_contoursNeedRefetch = false;

        /// Current canvas size in pixels
        ImVec2 m_canvasSize = {800.0f, 600.0f};

        /// Bounding rectangle of the current contour in world coordinates
        struct BoundingRect
        {
            float minX = FLT_MAX;
            float minY = FLT_MAX;
            float maxX = -FLT_MAX;
            float maxY = -FLT_MAX;
            bool isValid = false;

            void reset()
            {
                minX = FLT_MAX;
                minY = FLT_MAX;
                maxX = -FLT_MAX;
                maxY = -FLT_MAX;
                isValid = false;
            }

            void expand(Vector2 const & point)
            {
                minX = std::min(minX, point.x());
                minY = std::min(minY, point.y());
                maxX = std::max(maxX, point.x());
                maxY = std::max(maxY, point.y());
                isValid = true;
            }

            float width() const
            {
                return maxX - minX;
            }
            float height() const
            {
                return maxY - minY;
            }
            Vector2 center() const
            {
                return Vector2{(minX + maxX) * 0.5f, (minY + maxY) * 0.5f};
            }
        };

        BoundingRect m_contourBounds;

        /// Track if contours were empty in the previous frame for auto-centering
        bool m_contourWasEmpty = true;

        [[nodiscard]] ImVec2 worldToCanvasPos(ImVec2 WorldPos) const;
        [[nodiscard]] ImVec2 worldToCanvasPos(gladius::Vector2 WorldPos) const;
        [[nodiscard]] ImVec2 screenToWorldPos(ImVec2 screenPos) const;

        /// Calculate bounding rectangle from current contour data
        void calculateContourBounds();

        /// Render screen rulers with marks in canvas coordinates
        void renderScreenRulers(ImDrawList * drawList, ImVec2 canvasStart, ImVec2 canvasSize) const;
    };
}
