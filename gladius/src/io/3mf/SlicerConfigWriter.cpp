/**
 * @file SlicerConfigWriter.cpp
 * @brief Implementation of PrusaSlicer/OrcaSlicer model config generation
 */

#include "SlicerConfigWriter.h"

#include <fmt/format.h>
#include <sstream>

namespace gladius::io
{

    namespace
    {
        std::string escapeXmlAttribute(std::string const& input)
        {
            std::string result;
            result.reserve(input.size());
            for (char c : input)
            {
                switch (c)
                {
                case '&':  result += "&amp;";  break;
                case '<':  result += "&lt;";   break;
                case '>':  result += "&gt;";   break;
                case '"': result += "&quot;"; break;
                case '\'': result += "&apos;"; break;
                default:   result += c;         break;
                }
            }
            return result;
        }
    } // anonymous namespace

    std::string SlicerConfigWriter::generate(int objectId, SlicerVolumeInfo const& volume)
    {
        auto const safeName = escapeXmlAttribute(volume.name);
        std::ostringstream xml;

        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        xml << "<config>\n";
        xml << " <object id=\"" << objectId << "\" instances_count=\"1\">\n";
        xml << "  <metadata type=\"object\" key=\"name\" value=\""
            << safeName << "\"/>\n";
        xml << "  <volume firstid=\"" << volume.firstTriangleId
            << "\" lastid=\"" << volume.lastTriangleId << "\">\n";
        xml << "   <metadata type=\"volume\" key=\"name\" value=\""
            << safeName << "\"/>\n";
        xml << "   <metadata type=\"volume\" key=\"volume_type\" value=\"ModelPart\"/>\n";
        xml << "   <metadata type=\"volume\" key=\"extruder\" value=\""
            << volume.defaultExtruder << "\"/>\n";
        xml << "   <metadata type=\"volume\" key=\"matrix\" value=\""
            << "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\"/>\n";
        xml << "  </volume>\n";
        xml << " </object>\n";
        xml << "</config>\n";

        return xml.str();
    }

} // namespace gladius::io
