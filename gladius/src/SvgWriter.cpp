#include "SvgWriter.h"
#include "ContourExtractor.h"

#include <iomanip>
#include <sstream>

#include "gpgpu.h"

namespace gladius
{
    void SvgWriter::saveCurrentLayer(std::filesystem::path const & fileName,
                                     ComputeCore & generator)
    {
        // Request contour update for current slice
        auto sliceParameter = contourOnlyParameter();
        sliceParameter.zHeight_mm = generator.getSliceHeight();
        generator.requestContourUpdate(sliceParameter);

        // Determine bounds from model bounding box
        SvgBounds bounds;
        auto const bbox = generator.getBoundingBox();
        if (bbox.has_value())
        {
            bounds.minX = bbox->min.x;
            bounds.minY = bbox->min.y;
            bounds.maxX = bbox->max.x;
            bounds.maxY = bbox->max.y;
        }
        else
        {
            // Fallback: compute bounds from contour vertices
            auto const & contours = generator.getContour()->getContour();
            bounds.minX = std::numeric_limits<float>::max();
            bounds.minY = std::numeric_limits<float>::max();
            bounds.maxX = std::numeric_limits<float>::lowest();
            bounds.maxY = std::numeric_limits<float>::lowest();

            for (auto const & polyLine : contours)
            {
                for (auto const & vertex : polyLine.vertices)
                {
                    bounds.minX = std::min(bounds.minX, vertex.x());
                    bounds.minY = std::min(bounds.minY, vertex.y());
                    bounds.maxX = std::max(bounds.maxX, vertex.x());
                    bounds.maxY = std::max(bounds.maxY, vertex.y());
                }
            }

            // Handle empty contours
            if (bounds.minX > bounds.maxX)
            {
                bounds = {0.f, 0.f, 100.f, 100.f};
            }
        }

        saveContours(fileName, generator.getContour()->getContour(), bounds);
    }

    void SvgWriter::saveContours(std::filesystem::path const & fileName,
                                 PolyLines const & polyLines,
                                 SvgBounds const & bounds)
    {
        std::ofstream file(fileName);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open file for SVG export: " + fileName.string());
        }

        writeToStream(file, polyLines, bounds);
        file.close();
    }

    void SvgWriter::writeToStream(std::ostream & stream,
                                  PolyLines const & polyLines,
                                  SvgBounds const & bounds)
    {
        writeHeader(stream, bounds);
        writeLayer(stream, polyLines);
        writeFooter(stream);
    }

    void SvgWriter::writeHeader(std::ostream & stream, SvgBounds const & bounds)
    {
        // Use fixed precision for consistent output
        stream << std::fixed << std::setprecision(6);

        stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        stream << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
               << "xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\" baseProfile=\"full\" "
               << "width=\"" << bounds.width() << "mm\" "
               << "height=\"" << bounds.height() << "mm\" "
               << "viewBox=\"" << bounds.minX << " " << bounds.minY << " " << bounds.width() << " "
               << bounds.height() << "\">\n";

        // Add transform group to flip Y-axis (SVG Y goes down, CAD Y goes up)
        // Transform: translate to move origin, then scale -1 on Y to flip
        stream << "<g transform=\"translate(0," << (bounds.minY + bounds.maxY)
               << ") scale(1,-1)\">\n";
    }

    void SvgWriter::writeLayer(std::ostream & stream, PolyLines const & polyLines)
    {
        stream << "<path fill=\"none\" stroke=\"black\" stroke-width=\"0.1\" d=\"";
        for (auto const & polyLine : polyLines)
        {
            writePolyLine(stream, polyLine);
        }
        stream << "\"/>\n";
    }

    void SvgWriter::writePolyLine(std::ostream & stream, PolyLine const & polyLine)
    {
        // Don't write contour if in exclude mode
        if (polyLine.contourMode == ContourMode::ExcludeFromSlice)
        {
            return;
        }

        if (!polyLine.vertices.empty())
        {
            // Move to first vertex
            stream << "M " << polyLine.vertices[0].x() << "," << polyLine.vertices[0].y() << " ";

            // Line to subsequent vertices
            for (size_t i = 1; i < polyLine.vertices.size(); ++i)
            {
                stream << "L " << polyLine.vertices[i].x() << "," << polyLine.vertices[i].y()
                       << " ";
            }

            // Close path if this is a closed contour
            if (polyLine.isClosed)
            {
                stream << "Z ";
            }
        }
    }

    void SvgWriter::writeFooter(std::ostream & stream)
    {
        stream << "</g>\n";
        stream << "</svg>\n";
    }
}