#include "SvgWriter.h"

#include <gmock/gmock.h>
#include <regex>
#include <sstream>

namespace gladius::tests
{
    using namespace gladius;
    using ::testing::HasSubstr;
    using ::testing::MatchesRegex;

    class SvgWriterTest : public ::testing::Test
    {
      protected:
        SvgWriter m_writer;
        std::stringstream m_output;

        /// Helper to create a simple rectangular contour
        PolyLine createRectangle(float x1, float y1, float x2, float y2)
        {
            PolyLine rect;
            rect.contourMode = ContourMode::Outer;
            rect.isClosed = true;
            rect.vertices = {{x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}};
            return rect;
        }

        /// Helper to extract attribute value from SVG
        std::string extractAttribute(std::string const & svg,
                                     std::string const & element,
                                     std::string const & attr)
        {
            std::regex pattern("<" + element + "[^>]*\\s" + attr + "=\"([^\"]*)\"");
            std::smatch match;
            if (std::regex_search(svg, match, pattern))
            {
                return match[1].str();
            }
            return "";
        }
    };

    TEST_F(SvgWriterTest, WriteHeader_UsesCorrectUnits_mm)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 80.f};
        PolyLines empty;

        m_writer.writeToStream(m_output, empty, bounds);
        std::string svg = m_output.str();

        EXPECT_THAT(svg, HasSubstr("width=\"100.000000mm\""));
        EXPECT_THAT(svg, HasSubstr("height=\"80.000000mm\""));
    }

    TEST_F(SvgWriterTest, WriteHeader_ViewBoxMatchesBounds)
    {
        SvgBounds bounds{10.f, 20.f, 110.f, 120.f};
        PolyLines empty;

        m_writer.writeToStream(m_output, empty, bounds);
        std::string svg = m_output.str();

        // viewBox should be "minX minY width height"
        EXPECT_THAT(svg, HasSubstr("viewBox=\"10.000000 20.000000 100.000000 100.000000\""));
    }

    TEST_F(SvgWriterTest, WriteHeader_ContainsYAxisTransform)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 80.f};
        PolyLines empty;

        m_writer.writeToStream(m_output, empty, bounds);
        std::string svg = m_output.str();

        // Should have transform group for Y-axis flip
        EXPECT_THAT(svg, HasSubstr("transform=\"translate(0,80.000000) scale(1,-1)\""));
    }

    TEST_F(SvgWriterTest, WriteHeader_NegativeBoundsHandledCorrectly)
    {
        SvgBounds bounds{-50.f, -30.f, 50.f, 30.f};
        PolyLines empty;

        m_writer.writeToStream(m_output, empty, bounds);
        std::string svg = m_output.str();

        EXPECT_THAT(svg, HasSubstr("width=\"100.000000mm\""));
        EXPECT_THAT(svg, HasSubstr("height=\"60.000000mm\""));
        EXPECT_THAT(svg, HasSubstr("viewBox=\"-50.000000 -30.000000 100.000000 60.000000\""));
    }

    TEST_F(SvgWriterTest, WritePolyLine_ClosedContour_UsesZCommand)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 100.f};
        PolyLines contours;
        contours.push_back(createRectangle(10.f, 20.f, 30.f, 40.f));

        m_writer.writeToStream(m_output, contours, bounds);
        std::string svg = m_output.str();

        // Path should end with Z for closed contours
        EXPECT_THAT(svg, HasSubstr("Z "));
    }

    TEST_F(SvgWriterTest, WritePolyLine_OpenContour_NoZCommand)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 100.f};
        PolyLines contours;

        PolyLine openLine;
        openLine.contourMode = ContourMode::OpenLine;
        openLine.isClosed = false;
        openLine.vertices = {{0.f, 0.f}, {50.f, 50.f}, {100.f, 0.f}};
        contours.push_back(openLine);

        m_writer.writeToStream(m_output, contours, bounds);
        std::string svg = m_output.str();

        // Open paths should not have Z command
        EXPECT_THAT(svg, ::testing::Not(HasSubstr("Z ")));
    }

    TEST_F(SvgWriterTest, WritePolyLine_ExcludedContour_IsSkipped)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 100.f};
        PolyLines contours;

        PolyLine excluded;
        excluded.contourMode = ContourMode::ExcludeFromSlice;
        excluded.vertices = {{0.f, 0.f}, {100.f, 100.f}};
        contours.push_back(excluded);

        m_writer.writeToStream(m_output, contours, bounds);
        std::string svg = m_output.str();

        // Path data should be empty (just the path element with no coordinates)
        EXPECT_THAT(svg, HasSubstr("d=\"\""));
    }

    TEST_F(SvgWriterTest, WritePolyLine_CoordinatesAreCorrect)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 100.f};
        PolyLines contours;

        PolyLine line;
        line.contourMode = ContourMode::Outer;
        line.isClosed = false;
        line.vertices = {{10.5f, 20.25f}, {30.75f, 40.125f}};
        contours.push_back(line);

        m_writer.writeToStream(m_output, contours, bounds);
        std::string svg = m_output.str();

        // Coordinates should be written as-is (Y transform is via SVG transform group)
        EXPECT_THAT(svg, HasSubstr("M 10.500000,20.250000"));
        EXPECT_THAT(svg, HasSubstr("L 30.750000,40.125000"));
    }

    TEST_F(SvgWriterTest, WriteLayer_MultipleContours_AllWritten)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 100.f};
        PolyLines contours;
        contours.push_back(createRectangle(10.f, 10.f, 20.f, 20.f));
        contours.push_back(createRectangle(50.f, 50.f, 60.f, 60.f));

        m_writer.writeToStream(m_output, contours, bounds);
        std::string svg = m_output.str();

        // Both rectangles should be in the output
        EXPECT_THAT(svg, HasSubstr("M 10.000000,10.000000"));
        EXPECT_THAT(svg, HasSubstr("M 50.000000,50.000000"));
    }

    TEST_F(SvgWriterTest, ValidSvgStructure_HasXmlDeclaration)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 100.f};
        PolyLines empty;

        m_writer.writeToStream(m_output, empty, bounds);
        std::string svg = m_output.str();

        EXPECT_THAT(svg, HasSubstr("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
    }

    TEST_F(SvgWriterTest, ValidSvgStructure_HasSvgNamespace)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 100.f};
        PolyLines empty;

        m_writer.writeToStream(m_output, empty, bounds);
        std::string svg = m_output.str();

        EXPECT_THAT(svg, HasSubstr("xmlns=\"http://www.w3.org/2000/svg\""));
    }

    TEST_F(SvgWriterTest, ValidSvgStructure_HasClosingTags)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 100.f};
        PolyLines empty;

        m_writer.writeToStream(m_output, empty, bounds);
        std::string svg = m_output.str();

        EXPECT_THAT(svg, HasSubstr("</g>"));
        EXPECT_THAT(svg, HasSubstr("</svg>"));
    }

    TEST_F(SvgWriterTest, PathStyle_HasStrokeProperties)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 100.f};
        PolyLines contours;
        contours.push_back(createRectangle(10.f, 10.f, 20.f, 20.f));

        m_writer.writeToStream(m_output, contours, bounds);
        std::string svg = m_output.str();

        EXPECT_THAT(svg, HasSubstr("fill=\"none\""));
        EXPECT_THAT(svg, HasSubstr("stroke=\"black\""));
        EXPECT_THAT(svg, HasSubstr("stroke-width=\"0.1\""));
    }

    TEST_F(SvgWriterTest, SvgBounds_WidthAndHeight_Calculated)
    {
        SvgBounds bounds{10.f, 20.f, 50.f, 80.f};

        EXPECT_FLOAT_EQ(bounds.width(), 40.f);
        EXPECT_FLOAT_EQ(bounds.height(), 60.f);
    }

    TEST_F(SvgWriterTest, EmptyContour_IsHandledGracefully)
    {
        SvgBounds bounds{0.f, 0.f, 100.f, 100.f};
        PolyLines contours;

        PolyLine emptyLine;
        emptyLine.contourMode = ContourMode::Outer;
        emptyLine.vertices = {}; // Empty vertices
        contours.push_back(emptyLine);

        // Should not throw
        EXPECT_NO_THROW(m_writer.writeToStream(m_output, contours, bounds));
    }

} // namespace gladius::tests
