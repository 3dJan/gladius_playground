#pragma once

#include "3mf/ColorCompatibilityPlanner.h"
#include "3mf/ShellExportOptions.h"
#include "SurfaceExtractionOptions.h"

#include <filesystem>
#include <optional>

namespace gladius::io
{
    enum class MeshExportFormat
    {
        Stl,
        ThreeMf
    };

    /// Backend-neutral description of a mesh export, including optional color and shell settings.
    struct MeshExportRequest
    {
        std::filesystem::path filename;
        MeshExportFormat format{MeshExportFormat::ThreeMf};
        StlExportOptions extraction{};
        MeshColorExportSettings color{};
        std::optional<ShellExportConfig> shell;
    };
} // namespace gladius::io
