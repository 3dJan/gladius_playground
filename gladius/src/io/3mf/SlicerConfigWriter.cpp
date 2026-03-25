/**
 * @file SlicerConfigWriter.cpp
 * @brief Implementation of PrusaSlicer/OrcaSlicer model config generation
 */

#include "SlicerConfigWriter.h"

#include <fmt/format.h>
#include <sstream>

namespace gladius::io
{

    std::string SlicerConfigWriter::generate(int objectId, SlicerVolumeInfo const& volume)
    {
        std::ostringstream xml;

        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        xml << "<config>\n";
        xml << " <object id=\"" << objectId << "\" instances_count=\"1\">\n";
        xml << "  <metadata type=\"object\" key=\"name\" value=\""
            << volume.name << "\"/>\n";
        xml << "  <volume firstid=\"" << volume.firstTriangleId
            << "\" lastid=\"" << volume.lastTriangleId << "\">\n";
        xml << "   <metadata type=\"volume\" key=\"name\" value=\""
            << volume.name << "\"/>\n";
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
