#pragma once

#include "ContourExtractor.h"
#include "compute/ComputeCore.h"

#include <filesystem>
#include <fstream>

namespace gladius
{
    /// SVG bounding box for 2D export (X/Y dimensions in mm)
    struct SvgBounds
    {
        float minX = 0.f;
        float minY = 0.f;
        float maxX = 0.f;
        float maxY = 0.f;

        [[nodiscard]] float width() const
        {
            return maxX - minX;
        }
        [[nodiscard]] float height() const
        {
            return maxY - minY;
        }
    };

    /// Exports contours to SVG format with real-world units (mm)
    class SvgWriter
    {
      public:
        /// Exports the current layer contours to an SVG file
        /// @param fileName Output file path
        /// @param generator ComputeCore instance to extract contours and bounding box from
        void saveCurrentLayer(std::filesystem::path const & fileName, ComputeCore & generator);

        /// Exports contours directly to an SVG file (for testing without ComputeCore)
        /// @param fileName Output file path
        /// @param polyLines Contours to export
        /// @param bounds Bounding box for the SVG viewBox
        void saveContours(std::filesystem::path const & fileName,
                          PolyLines const & polyLines,
                          SvgBounds const & bounds);

        /// Writes contours to an already-open stream (for testing)
        /// @param stream Output stream
        /// @param polyLines Contours to export
        /// @param bounds Bounding box for the SVG viewBox
        void writeToStream(std::ostream & stream,
                           PolyLines const & polyLines,
                           SvgBounds const & bounds);

      private:
        void writeHeader(std::ostream & stream, SvgBounds const & bounds);
        void writeLayer(std::ostream & stream, PolyLines const & polyLines);
        void writePolyLine(std::ostream & stream, PolyLine const & polyLine);
        void writeFooter(std::ostream & stream);
    };
}
