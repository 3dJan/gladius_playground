#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#if defined(GLADIUS_ENABLE_OPENCL)
#include "gpgpu.h"
#else

using cl_char = std::int8_t;
using cl_float = float;
using cl_int = std::int32_t;
using cl_uint = std::uint32_t;
using cl_ulong = std::uint64_t;

/// Host-side vector types used by backend-neutral geometry and camera code.
/// The pure WebGPU build must not include the OpenCL headers just to provide
/// the layout-compatible scalar/vector types shared with the kernels.
struct cl_float2
{
    union
    {
        struct
        {
            float x;
            float y;
        };
        float s[2];
    };

    constexpr cl_float2()
        : s{}
    {
    }

    constexpr cl_float2(float xValue, float yValue)
        : s{xValue, yValue}
    {
    }

    cl_float2(std::initializer_list<float> values)
        : s{}
    {
        std::size_t index = 0;
        for (float value : values)
        {
            if (index == 2)
            {
                break;
            }
            s[index++] = value;
        }
    }
};

struct cl_float4
{
    union
    {
        struct
        {
            float x;
            float y;
            float z;
            float w;
        };
        float s[4];
    };

    constexpr cl_float4()
        : s{}
    {
    }

    constexpr cl_float4(float xValue, float yValue, float zValue, float wValue)
        : s{xValue, yValue, zValue, wValue}
    {
    }

    cl_float4(std::initializer_list<float> values)
        : s{}
    {
        std::size_t index = 0;
        for (float value : values)
        {
            if (index == 4)
            {
                break;
            }
            s[index++] = value;
        }
    }
};

// OpenCL defines cl_float3 as a four-component vector for ABI alignment. Keep
// the same relationship in the host-only implementation so code accepting a
// cl_float4 also accepts a cl_float3, just as it does with the OpenCL headers.
using cl_float3 = cl_float4;

struct cl_float8
{
    union
    {
        struct
        {
            float s0;
            float s1;
            float s2;
            float s3;
            float s4;
            float s5;
            float s6;
            float s7;
        };
        float s[8];
    };

    constexpr cl_float8()
        : s{}
    {
    }
};

struct cl_float16
{
    union
    {
        struct
        {
            float s0;
            float s1;
            float s2;
            float s3;
            float s4;
            float s5;
            float s6;
            float s7;
            float s8;
            float s9;
            float sa;
            float sb;
            float sc;
            float sd;
            float se;
            float sf;
        };
        float s[16];
    };

    constexpr cl_float16()
        : s{}
    {
    }
};

struct cl_int2
{
    union
    {
        struct
        {
            cl_int x;
            cl_int y;
        };
        cl_int s[2];
    };

    constexpr cl_int2()
        : s{}
    {
    }

    constexpr cl_int2(cl_int xValue, cl_int yValue)
        : s{xValue, yValue}
    {
    }
};

struct cl_char4
{
    union
    {
        struct
        {
            cl_char x;
            cl_char y;
            cl_char z;
            cl_char w;
        };
        cl_char s[4];
    };

    constexpr cl_char4()
        : s{}
    {
    }
};

#endif