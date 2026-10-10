#include "IndexedTriangleMeshWriter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace gladius::io
{
    namespace
    {
        std::uint32_t getValidatedTriangleCount(
            IndexedTriangleMesh const & mesh,
            IndexedTriangleMeshValidationPolicy policy)
        {
            if (auto const error = mesh.validationError(policy))
            {
                throw std::invalid_argument("Cannot export invalid mesh as STL: " + *error);
            }

            std::size_t const triangleCount = mesh.indices.size() / 3U;
            if (triangleCount > std::numeric_limits<std::uint32_t>::max())
            {
                throw std::invalid_argument("Mesh exceeds the binary STL triangle count limit");
            }
            return static_cast<std::uint32_t>(triangleCount);
        }

        void writeUInt16LittleEndian(std::ostream & output, std::uint16_t value)
        {
                        std::array<unsigned char, 2> const bytes{
                            static_cast<unsigned char>(value & 0xffU),
                            static_cast<unsigned char>((value >> 8U) & 0xffU)};
                        output.write(reinterpret_cast<char const *>(bytes.data()),
                                                 static_cast<std::streamsize>(bytes.size()));
        }

        void writeUInt32LittleEndian(std::ostream & output, std::uint32_t value)
        {
                        std::array<unsigned char, 4> const bytes{
                            static_cast<unsigned char>(value & 0xffU),
                            static_cast<unsigned char>((value >> 8U) & 0xffU),
                            static_cast<unsigned char>((value >> 16U) & 0xffU),
                            static_cast<unsigned char>((value >> 24U) & 0xffU)};
                        output.write(reinterpret_cast<char const *>(bytes.data()),
                                                 static_cast<std::streamsize>(bytes.size()));
        }

        void writeFloatLittleEndian(std::ostream & output, float value)
        {
            static_assert(sizeof(float) == sizeof(std::uint32_t));
            writeUInt32LittleEndian(output, std::bit_cast<std::uint32_t>(value));
        }

        void writeValidatedBinaryStl(std::ostream & output,
                                     IndexedTriangleMesh const & mesh,
                                     std::uint32_t triangleCount)
        {
            std::array<char, 80> header{};
            constexpr std::string_view headerText{"Gladius indexed triangle mesh"};
            std::copy(headerText.begin(), headerText.end(), header.begin());
            output.write(header.data(), static_cast<std::streamsize>(header.size()));
            writeUInt32LittleEndian(output, triangleCount);

            for (std::size_t offset = 0; offset < mesh.indices.size(); offset += 3U)
            {
                auto const & a = mesh.positions[mesh.indices[offset]];
                auto const & b = mesh.positions[mesh.indices[offset + 1U]];
                auto const & c = mesh.positions[mesh.indices[offset + 2U]];

                double const abx = static_cast<double>(b.x()) - a.x();
                double const aby = static_cast<double>(b.y()) - a.y();
                double const abz = static_cast<double>(b.z()) - a.z();
                double const acx = static_cast<double>(c.x()) - a.x();
                double const acy = static_cast<double>(c.y()) - a.y();
                double const acz = static_cast<double>(c.z()) - a.z();

                double const normalX = aby * acz - abz * acy;
                double const normalY = abz * acx - abx * acz;
                double const normalZ = abx * acy - aby * acx;
                double const normalLength =
                  std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);

                if (normalLength <= 1.0e-6)
                {
                    writeFloatLittleEndian(output, 0.0F);
                    writeFloatLittleEndian(output, 0.0F);
                    writeFloatLittleEndian(output, 1.0F);
                }
                else
                {
                    writeFloatLittleEndian(output, static_cast<float>(normalX / normalLength));
                    writeFloatLittleEndian(output, static_cast<float>(normalY / normalLength));
                    writeFloatLittleEndian(output, static_cast<float>(normalZ / normalLength));
                }

                for (auto const * vertex : {&a, &b, &c})
                {
                    writeFloatLittleEndian(output, vertex->x());
                    writeFloatLittleEndian(output, vertex->y());
                    writeFloatLittleEndian(output, vertex->z());
                }

                writeUInt16LittleEndian(output, 0U);
            }

            if (!output)
            {
                throw std::runtime_error("Failed while writing binary STL data");
            }
        }
    } // namespace

    void writeIndexedMeshToBinaryStl(
        std::ostream & output,
        IndexedTriangleMesh const & mesh,
        IndexedTriangleMeshValidationPolicy policy)
    {
        auto const triangleCount = getValidatedTriangleCount(mesh, policy);
        writeValidatedBinaryStl(output, mesh, triangleCount);
    }

    void writeIndexedMeshToBinaryStl(
        std::filesystem::path const & filePath,
        IndexedTriangleMesh const & mesh,
        IndexedTriangleMeshValidationPolicy policy)
    {
        auto const triangleCount = getValidatedTriangleCount(mesh, policy);
        auto temporaryPath = filePath;
        temporaryPath += ".tmp." + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Could not open temporary STL output file: " +
                                     temporaryPath.string());
        }

        try
        {
            writeValidatedBinaryStl(output, mesh, triangleCount);
            output.flush();
            if (!output)
            {
                throw std::runtime_error("Failed to flush temporary STL output file: " +
                                         temporaryPath.string());
            }

            output.close();
            if (!output)
            {
                throw std::runtime_error("Failed to close temporary STL output file: " +
                                         temporaryPath.string());
            }
            std::error_code error;
            std::filesystem::rename(temporaryPath, filePath, error);
            if (error)
            {
                throw std::runtime_error("Could not finalize STL output file: " + error.message());
            }
        }
        catch (...)
        {
            if (output.is_open())
            {
                output.close();
            }
            std::error_code error;
            std::filesystem::remove(temporaryPath, error);
            throw;
        }
    }
} // namespace gladius::io