#pragma once

#include "nodes/types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace gladius::io
{
    /// @brief A constant value extracted from a library .3mf example function.
    ///
    /// Represents a ConstantScalar or ConstantVector node that feeds into
    /// a FunctionCall argument in the library file's embedded example.
    struct ExampleConstantValue
    {
        enum class Kind
        {
            Scalar,
            Vector,
            Matrix
        };

        Kind kind = Kind::Scalar;
        std::string parameterName; ///< The FunctionCall argument name this value feeds into
        float scalarValue = 0.0f;
        nodes::float3 vectorValue = {};
        nodes::Matrix4x4 matrixValue = {};
    };

    /// @brief Extract example constant values for a tagged library function from a .3mf file.
    ///
    /// Loads the file, finds the assembly/example function that calls the tagged function
    /// (matched by display name), and returns the constant node values wired to its
    /// FunctionCall arguments.  Used to pre-populate constant nodes when the user drops a
    /// library item onto the node editor canvas.
    ///
    /// @param filePath                  Path to the .3mf library file.
    /// @param taggedFunctionDisplayName Display name of the tagged library function.
    /// @return Example constants (empty if none found, or on any error).
    [[nodiscard]] std::vector<ExampleConstantValue>
    extractExampleConstants(std::filesystem::path const & filePath,
                            std::string const & taggedFunctionDisplayName);

} // namespace gladius::io
