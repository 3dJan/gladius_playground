// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

//
// OpenCL wrapper for PNanoVDB - Part 1: Buffer definitions
// This header provides OpenCL-compatible buffer access for PNanoVDB.
// It MUST be included BEFORE PNanoVDB.h in the OpenCL kernel source list.
//
// The helper functions (pnanovdb_make_coord, pnanovdb_read_float_value, etc.)
// are defined in PNanoVDB_OpenCL_Helpers.h which MUST be included AFTER PNanoVDB.h.
//

#ifndef PNANOVDB_OPENCL_H_HAS_BEEN_INCLUDED
#define PNANOVDB_OPENCL_H_HAS_BEEN_INCLUDED

// OpenCL-specific type definitions (must be defined before including PNanoVDB.h)
#ifdef __OPENCL_VERSION__

// Gladius only needs FLOAT and INT32 grid types for distance fields and triangle lookups.
// This significantly reduces compilation time and constant memory usage.
#define PNANOVDB_GLADIUS_MINIMAL

// Use C mode with custom buffer for OpenCL
#define PNANOVDB_C
#define PNANOVDB_BUF_CUSTOM
#define PNANOVDB_ADDRESS_32

// OpenCL basic types
typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

// Custom buffer type for OpenCL global memory
typedef struct pnanovdb_buf_t
{
    __global uint32_t* data;
} pnanovdb_buf_t;

// Buffer creation helper
static inline pnanovdb_buf_t pnanovdb_make_buf(__global uint32_t* data, uint64_t size_in_words)
{
    pnanovdb_buf_t ret;
    ret.data = data;
    return ret;
}

// Buffer read/write functions for 32-bit addressing
static inline uint32_t pnanovdb_buf_read_uint32(pnanovdb_buf_t buf, uint32_t byte_offset)
{
    uint32_t wordaddress = (byte_offset >> 2u);
    return buf.data[wordaddress];
}

static inline uint64_t pnanovdb_buf_read_uint64(pnanovdb_buf_t buf, uint32_t byte_offset)
{
    __global uint64_t* data64 = (__global uint64_t*) buf.data;
    uint32_t wordaddress64 = (byte_offset >> 3u);
    return data64[wordaddress64];
}

static inline void
pnanovdb_buf_write_uint32(pnanovdb_buf_t buf, uint32_t byte_offset, uint32_t value)
{
    uint32_t wordaddress = (byte_offset >> 2u);
    buf.data[wordaddress] = value;
}

static inline void
pnanovdb_buf_write_uint64(pnanovdb_buf_t buf, uint32_t byte_offset, uint64_t value)
{
    __global uint64_t* data64 = (__global uint64_t*) buf.data;
    uint32_t wordaddress64 = (byte_offset >> 3u);
    data64[wordaddress64] = value;
}

// Grid type for OpenCL
typedef uint32_t pnanovdb_grid_type_t;
#define PNANOVDB_GRID_TYPE_GET(grid_typeIn, nameIn) pnanovdb_grid_type_constants[grid_typeIn].nameIn

// Force inline for OpenCL
#define PNANOVDB_BUF_FORCE_INLINE static inline

// OpenCL requires program-scope constants to be in __constant address space
#define PNANOVDB_STATIC_CONST __constant

// memcpy replacement for OpenCL - uses private address space pointers
static inline void pnanovdb_memcpy(void* dst, const void* src, size_t n)
{
    char* d = (char*) dst;
    const char* s = (const char*) src;
    for (size_t i = 0; i < n; ++i)
    {
        d[i] = s[i];
    }
}
#define PNANOVDB_MEMCPY_CUSTOM

#endif // __OPENCL_VERSION__

#endif // PNANOVDB_OPENCL_H_HAS_BEEN_INCLUDED
