// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

//
// OpenCL wrapper for PNanoVDB - Part 2: Helper functions
// This header provides convenience functions for PNanoVDB access in OpenCL.
// It MUST be included AFTER PNanoVDB.h in the OpenCL kernel source list.
//
// Source file order should be:
//   1. PNanoVDB_OpenCL.h (buffer definitions)
//   2. PNanoVDB.h (main PNanoVDB implementation)
//   3. PNanoVDB_OpenCL_Helpers.h (this file - helper functions)
//

#ifndef PNANOVDB_OPENCL_HELPERS_H_HAS_BEEN_INCLUDED
#define PNANOVDB_OPENCL_HELPERS_H_HAS_BEEN_INCLUDED

#ifdef __OPENCL_VERSION__

// ================================================
// OpenCL convenience functions for VDB access
// These provide a simpler API similar to CNanoVDB
// ================================================

// Helper to create a grid handle from a buffer offset
static inline pnanovdb_grid_handle_t pnanovdb_make_grid_handle(uint32_t byte_offset)
{
    pnanovdb_grid_handle_t grid;
    grid.address.byte_offset = byte_offset;
    return grid;
}

// Read a float value from the grid at the given coordinate
static inline float
pnanovdb_read_float_value(pnanovdb_buf_t buf, pnanovdb_readaccessor_t* acc, pnanovdb_coord_t ijk)
{
    pnanovdb_address_t address = pnanovdb_readaccessor_get_value_address(
      PNANOVDB_GRID_TYPE_FLOAT, buf, acc, PNANOVDB_REF(ijk));
    return pnanovdb_read_float(buf, address);
}

// Read an int32 value from the grid at the given coordinate
static inline int32_t
pnanovdb_read_int32_value(pnanovdb_buf_t buf, pnanovdb_readaccessor_t* acc, pnanovdb_coord_t ijk)
{
    pnanovdb_address_t address = pnanovdb_readaccessor_get_value_address(
      PNANOVDB_GRID_TYPE_INT32, buf, acc, PNANOVDB_REF(ijk));
    return pnanovdb_read_int32(buf, address);
}

// Initialize read accessor for a float grid
static inline void pnanovdb_readaccessor_init_float(pnanovdb_readaccessor_t* acc,
                                                    pnanovdb_buf_t buf,
                                                    pnanovdb_grid_handle_t grid)
{
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_readaccessor_init(acc, root);
}

// Initialize read accessor for an int32 grid
static inline void pnanovdb_readaccessor_init_int32(pnanovdb_readaccessor_t* acc,
                                                    pnanovdb_buf_t buf,
                                                    pnanovdb_grid_handle_t grid)
{
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_readaccessor_init(acc, root);
}

// Create a coordinate from x, y, z values
static inline pnanovdb_coord_t pnanovdb_make_coord(int32_t x, int32_t y, int32_t z)
{
    pnanovdb_coord_t coord;
    coord.x = x;
    coord.y = y;
    coord.z = z;
    return coord;
}

#endif // __OPENCL_VERSION__

#endif // PNANOVDB_OPENCL_HELPERS_H_HAS_BEEN_INCLUDED
