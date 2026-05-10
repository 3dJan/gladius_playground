/**
 * @file MmuSegmentationWriter.cpp
 * @brief Implementation of PrusaSlicer/OrcaSlicer MMU segmentation encoding
 */

#include "MmuSegmentationWriter.h"

#include <cassert>
#include <stdexcept>

namespace gladius::io
{

    std::string MmuSegmentationWriter::encodeExtruderState(int extruderIndex)
    {
        if (extruderIndex <= 0)
        {
            return {};
        }

        if (extruderIndex > MAX_EXTRUDERS)
        {
            throw std::runtime_error("MMU segmentation supports at most 16 extruders");
        }

        // The bitstream encodes an unsplit (leaf) triangle as follows:
        // Bits are stored LSB-first in groups of 4 (nibbles), then each nibble is
        // written as a hex char. The hex string itself is reversed (most significant
        // nibble first) in the final output.
        //
        // For an unsplit triangle:
        //   4 bits: yy xx  where yy = split_sides (0 for leaf), xx = state (2 bits)
        //
        // For state 0..2:  nibble = (state << 2) | 0  →  0x0, 0x4, 0x8
        //   but then reversed hex: nibble 0x4 -> char '4', etc.
        //   Wait - let me re-read the OrcaSlicer serialization more carefully.
        //
        // Looking at TriangleSelector::serialize():
        //   - First 2 bits: split_sides (0 for unsplit leaf)
        //   - Next 2 bits: state (for state 0..2) OR 0b11 prefix for extended state
        //
        // The bits are pushed into a bitstream LSB first.
        // Then get_triangle_as_string() reads 4 bits at a time from the bitstream,
        // converts to hex, and PREPENDS each hex char (so the string is reversed).
        //
        // For extruder 1 (state=1), unsplit:
        //   bitstream: [0,0, 1,0]  (split=0, state=1 in 2 bits: bit0=1, bit1=0)
        //   4 bits read: bit0..bit3 = 0,0,1,0
        //   nibble = 0*1 + 0*2 + 1*4 + 0*8 = 4
        //   hex char = '4'
        //   Result: "4"
        //
        // For extruder 2 (state=2), unsplit:
        //   bitstream: [0,0, 0,1]  (split=0, state=2: bit0=0, bit1=1)
        //   nibble = 0 + 0 + 0 + 1*8 = 8
        //   hex char = '8'
        //   Result: "8"
        //
        // For extruder 3 (state=3), unsplit:
        //   bitstream: [0,0, 1,1,  0,0,0,0]  (split=0, prefix=11, then (3-3)=0 in 4 bits)
        //   First nibble: bits 0..3 = 0,0,1,1 → 0+0+4+8 = 12 = 0xC
        //   Second nibble: bits 4..7 = 0,0,0,0 → 0
        //   String (prepend order): "0" then prepend "C" → "C0"
        //   Result: "C0"
        //
        // For extruder N (state=N, N>=3), unsplit:
        //   First nibble is always 0xC (split=0, prefix=11)
        //   Then (N-3) encoded in base-15 nibbles (0xF means "continue")

        // Build the bitstream directly
        std::vector<bool> bits;

        // split_sides = 0 (unsplit leaf)
        bits.push_back(false); // bit 0 of split_sides
        bits.push_back(false); // bit 1 of split_sides

        if (extruderIndex <= 2)
        {
            // Simple 2-bit state
            bits.push_back(static_cast<bool>(extruderIndex & 1));
            bits.push_back(static_cast<bool>(extruderIndex & 2));
        }
        else
        {
            // Extended encoding: prefix 0b11, then (state-3) in base-15 nibbles
            bits.push_back(true);  // prefix bit 0
            bits.push_back(true);  // prefix bit 1

            int remaining = extruderIndex - 3;
            while (remaining >= 15)
            {
                // continuation nibble: 0xF
                for (int i = 0; i < 4; ++i)
                {
                    bits.push_back(static_cast<bool>(0xF & (1 << i)));
                }
                remaining -= 15;
            }
            // final nibble
            for (int i = 0; i < 4; ++i)
            {
                bits.push_back(static_cast<bool>(remaining & (1 << i)));
            }
        }

        // Convert bitstream to hex string (same logic as get_triangle_as_string)
        // Read 4 bits at a time, convert to hex, prepend to result
        std::string result;
        for (std::size_t offset = 0; offset < bits.size(); offset += 4)
        {
            int nibble = 0;
            for (int i = 3; i >= 0; --i)
            {
                nibble <<= 1;
                if (offset + static_cast<std::size_t>(i) < bits.size())
                {
                    nibble |= static_cast<int>(bits[offset + static_cast<std::size_t>(i)]);
                }
            }
            assert(nibble >= 0 && nibble <= 15);
            char digit = nibble < 10 ? static_cast<char>('0' + nibble)
                                     : static_cast<char>('A' + nibble - 10);
            result.insert(result.begin(), digit);
        }

        return result;
    }

    std::vector<std::string> MmuSegmentationWriter::encodeFaceExtruders(QuantizedPalette const& palette)
    {
        std::vector<std::string> result;
        result.reserve(palette.sourceToPaletteMap.size());

        for (auto const paletteIdx : palette.sourceToPaletteMap)
        {
            // Palette index is 0-based, extruder is 1-based
            int const extruderIndex = static_cast<int>(paletteIdx) + 1;
            result.push_back(encodeExtruderState(extruderIndex));
        }

        return result;
    }

} // namespace gladius::io
