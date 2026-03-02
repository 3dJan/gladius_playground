// Dual Contouring GPU Sampling Kernels
// Provides batched SDF evaluation for octree corner and Hermite sampling

// Corner batch sampling kernel
// Evaluates SDF at a batch of 3D positions
__kernel void sampleCorners(
    __global const float4* positions,    // Input: array of (x,y,z,_) positions
    __global float* values,              // Output: SDF values at positions
    const unsigned int count,            // Number of positions to sample
    PAYLOAD_ARGS,
    const float isoValue)
{
    const int gid = get_global_id(0);
    
    if (gid >= count)
    {
        return;
    }
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Evaluate SDF at this position using the model function
    const float4 sdfResult = model(worldPos, PASS_PAYLOAD_ARGS);
    const float distance = sdfResult.w;
    
    values[gid] = distance - isoValue;
}

// Hermite data sampling kernel with gradient computation
// Evaluates SDF and computes gradient (using central differences) at each position
__kernel void sampleHermite(
    __global const float4* positions,    // Input: array of (x,y,z,_) positions
    __global float* values,              // Output: SDF values
    __global float4* gradients,          // Output: normalized gradients (dx,dy,dz,_)
    const unsigned int count,            // Number of positions to sample
    PAYLOAD_ARGS,
    const float isoValue,
    const float epsilon)                 // Step size for gradient computation
{
    const int gid = get_global_id(0);
    
    if (gid >= count)
    {
        return;
    }
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Compute SDF value at center position
    const float4 centerResult = model(worldPos, PASS_PAYLOAD_ARGS);
    const float distance = centerResult.w - isoValue;
    values[gid] = distance;
    
    // Use 4-point central difference for more robust gradient estimation
    // This averages forward and backward differences to reduce noise
    const float h = epsilon;
    const float h2 = 2.0f * h;
    
    // Primary samples at +/- epsilon
    const float3 posXp = worldPos + (float3)(h, 0.0f, 0.0f);
    const float3 posXn = worldPos - (float3)(h, 0.0f, 0.0f);
    const float3 posYp = worldPos + (float3)(0.0f, h, 0.0f);
    const float3 posYn = worldPos - (float3)(0.0f, h, 0.0f);
    const float3 posZp = worldPos + (float3)(0.0f, 0.0f, h);
    const float3 posZn = worldPos - (float3)(0.0f, 0.0f, h);
    
    const float sdfXp = model(posXp, PASS_PAYLOAD_ARGS).w;
    const float sdfXn = model(posXn, PASS_PAYLOAD_ARGS).w;
    const float sdfYp = model(posYp, PASS_PAYLOAD_ARGS).w;
    const float sdfYn = model(posYn, PASS_PAYLOAD_ARGS).w;
    const float sdfZp = model(posZp, PASS_PAYLOAD_ARGS).w;
    const float sdfZn = model(posZn, PASS_PAYLOAD_ARGS).w;
    
    // Central difference formula: (f(x+h) - f(x-h)) / 2h
    float3 gradient;
    gradient.x = (sdfXp - sdfXn) / h2;
    gradient.y = (sdfYp - sdfYn) / h2;
    gradient.z = (sdfZp - sdfZn) / h2;
    
    // Check for degenerate gradient (numerical issues or flat region)
    // Use squared length to avoid sqrt
    const float gradLengthSq = dot(gradient, gradient);
    
    // More aggressive threshold for degeneracy detection
    if (gradLengthSq > 1e-8f)
    {
        // Normalize the gradient
        const float gradLength = sqrt(gradLengthSq);
        gradient /= gradLength;
    }
    else
    {
        // Degenerate gradient - try wider spacing for finite difference
        // Use 5× wider spacing instead of 10× for more accuracy
        const float h_wide = h * 5.0f;
        const float h2_wide = 2.0f * h_wide;
        
        const float3 posXp_wide = worldPos + (float3)(h_wide, 0.0f, 0.0f);
        const float3 posXn_wide = worldPos - (float3)(h_wide, 0.0f, 0.0f);
        const float3 posYp_wide = worldPos + (float3)(0.0f, h_wide, 0.0f);
        const float3 posYn_wide = worldPos - (float3)(0.0f, h_wide, 0.0f);
        const float3 posZp_wide = worldPos + (float3)(0.0f, 0.0f, h_wide);
        const float3 posZn_wide = worldPos - (float3)(0.0f, 0.0f, h_wide);
        
        const float sdfXp_wide = model(posXp_wide, PASS_PAYLOAD_ARGS).w;
        const float sdfXn_wide = model(posXn_wide, PASS_PAYLOAD_ARGS).w;
        const float sdfYp_wide = model(posYp_wide, PASS_PAYLOAD_ARGS).w;
        const float sdfYn_wide = model(posYn_wide, PASS_PAYLOAD_ARGS).w;
        const float sdfZp_wide = model(posZp_wide, PASS_PAYLOAD_ARGS).w;
        const float sdfZn_wide = model(posZn_wide, PASS_PAYLOAD_ARGS).w;
        
        gradient.x = (sdfXp_wide - sdfXn_wide) / h2_wide;
        gradient.y = (sdfYp_wide - sdfYn_wide) / h2_wide;
        gradient.z = (sdfZp_wide - sdfZn_wide) / h2_wide;
        
        const float gradLengthSq_wide = dot(gradient, gradient);
        if (gradLengthSq_wide > 1e-8f)
        {
            gradient /= sqrt(gradLengthSq_wide);
        }
        else
        {
            // Still degenerate - use fallback normal
            gradient = (float3)(0.0f, 1.0f, 0.0f);
        }
    }
    
    gradients[gid] = (float4)(gradient.x, gradient.y, gradient.z, 0.0f);
}

// Color sampling kernel
// Evaluates volumetric color at a batch of 3D positions
// model() returns float4: .xyz = RGB color (linear), .w = signed distance
__kernel void sampleColors(
    __global const float4* positions,    // Input: array of (x,y,z,_) positions
    __global float4* colors,             // Output: RGB colors at positions (linear sRGB)
    const unsigned int count,            // Number of positions to sample
    PAYLOAD_ARGS)
{
    const int gid = get_global_id(0);
    
    if (gid >= count)
    {
        return;
    }
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Evaluate model at this position - returns float4(color.rgb, distance)
    const float4 result = model(worldPos, PASS_PAYLOAD_ARGS);
    
    // Extract and clamp color to valid range (linear RGB)
    float3 color = clamp(result.xyz, 0.0f, 1.0f);
    
    // If color is nearly black (possibly invalid/undefined), try sampling slightly inside the surface
    // This handles cases where mesh vertices are slightly outside the true SDF surface
    const float colorMagnitude = color.x + color.y + color.z;
    if (colorMagnitude < 0.01f && result.w > -0.5f && result.w < 0.5f)
    {
        // Near the surface but got black color - try moving slightly inward (negative SDF direction)
        // Compute gradient to find surface normal direction
        const float eps = 0.01f;
        const float dx = model(worldPos + (float3)(eps, 0.0f, 0.0f), PASS_PAYLOAD_ARGS).w 
                       - model(worldPos - (float3)(eps, 0.0f, 0.0f), PASS_PAYLOAD_ARGS).w;
        const float dy = model(worldPos + (float3)(0.0f, eps, 0.0f), PASS_PAYLOAD_ARGS).w 
                       - model(worldPos - (float3)(0.0f, eps, 0.0f), PASS_PAYLOAD_ARGS).w;
        const float dz = model(worldPos + (float3)(0.0f, 0.0f, eps), PASS_PAYLOAD_ARGS).w 
                       - model(worldPos - (float3)(0.0f, 0.0f, eps), PASS_PAYLOAD_ARGS).w;
        
        float3 gradient = (float3)(dx, dy, dz);
        const float gradLen = length(gradient);
        if (gradLen > 0.001f)
        {
            gradient = gradient / gradLen;
            // Sample slightly inside the surface (move in negative gradient direction)
            const float3 insidePos = worldPos - gradient * 0.1f;
            const float4 insideResult = model(insidePos, PASS_PAYLOAD_ARGS);
            const float3 insideColor = clamp(insideResult.xyz, 0.0f, 1.0f);
            
            // Use inside color if it's more valid
            if ((insideColor.x + insideColor.y + insideColor.z) > colorMagnitude)
            {
                color = insideColor;
            }
        }
    }
    
    colors[gid] = (float4)(color.x, color.y, color.z, 1.0f);
}

// Zero-crossing refinement kernel
// Refines edge crossing positions using secant/bisection method
__kernel void refineZeroCrossings(
    __global const float4* edgeStarts,   // Input: edge start positions
    __global const float4* edgeEnds,     // Input: edge end positions
    __global const float* startValues,   // Input: SDF at start
    __global const float* endValues,     // Input: SDF at end
    __global float4* refinedPositions,   // Output: refined crossing positions
    const unsigned int count,            // Number of edges
    PAYLOAD_ARGS,
    const unsigned int maxIterations,    // Max refinement iterations
    const float tolerance,               // Convergence tolerance
    const float isoValue)
{
    const int gid = get_global_id(0);
    
    if (gid >= count)
    {
        return;
    }
    
    float3 start = (float3)(edgeStarts[gid].x, edgeStarts[gid].y, edgeStarts[gid].z);
    float3 end = (float3)(edgeEnds[gid].x, edgeEnds[gid].y, edgeEnds[gid].z);
    float valueStart = startValues[gid];
    float valueEnd = endValues[gid];
    
    // Secant/bisection method for zero-crossing refinement
    for (unsigned int iter = 0; iter < maxIterations; ++iter)
    {
        if (fabs(valueEnd - valueStart) < tolerance)
        {
            break;
        }
        
        // Interpolation parameter
        const float t = valueStart / (valueStart - valueEnd);
        const float tClamped = clamp(t, 0.0f, 1.0f);
        
        const float3 mid = start + (end - start) * tClamped;
        const float valueMid = model(mid, PASS_PAYLOAD_ARGS).w - isoValue;
        
        // Update interval
        if (valueMid * valueStart < 0.0f)
        {
            end = mid;
            valueEnd = valueMid;
        }
        else
        {
            start = mid;
            valueStart = valueMid;
        }
    }
    
    // Final interpolated position
    const float t = (fabs(valueEnd - valueStart) > tolerance) ? 
                    valueStart / (valueStart - valueEnd) : 0.5f;
    const float tClamped = clamp(t, 0.0f, 1.0f);
    const float3 result = start + (end - start) * tClamped;
    
    refinedPositions[gid] = (float4)(result.x, result.y, result.z, 0.0f);
}

// Variable thickness sampling kernel
// Evaluates SDF with thickness modulation from a 3D LUT
__kernel void sampleCornersVariableThickness(
    __global const float4* positions,    // Input: array of (x,y,z,_) positions
    __global float* values,              // Output: SDF values at positions
    const unsigned int count,            // Number of positions to sample
    PAYLOAD_ARGS,
    const float baseIsoValue,            // Base ISO value (usually 0 or base offset)
    __global const float* thicknessLUT,  // 3D LUT: RGB -> Thickness
    const int lutResolution              // Resolution of the 3D LUT (e.g. 32)
)
{
    const int gid = get_global_id(0);
    
    if (gid >= count)
    {
        return;
    }
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Evaluate SDF at this position using the model function
    const float4 sdfResult = model(worldPos, PASS_PAYLOAD_ARGS);
    const float distance = sdfResult.w;
    const float3 color = clamp(sdfResult.xyz, 0.0f, 1.0f);
    
    // Trilinear interpolation of thickness from LUT
    float3 uvw = color * (float)(lutResolution - 1);
    int3 i = convert_int3(floor(uvw));
    float3 f = uvw - convert_float3(i);
    
    // Clamp indices
    i = clamp(i, 0, lutResolution - 2);
    
    // Helper macro for LUT access
    #define LUT_IDX(x, y, z) (((x) * lutResolution + (y)) * lutResolution + (z))
    
    float c000 = thicknessLUT[LUT_IDX(i.x, i.y, i.z)];
    float c001 = thicknessLUT[LUT_IDX(i.x, i.y, i.z + 1)];
    float c010 = thicknessLUT[LUT_IDX(i.x, i.y + 1, i.z)];
    float c011 = thicknessLUT[LUT_IDX(i.x, i.y + 1, i.z + 1)];
    float c100 = thicknessLUT[LUT_IDX(i.x + 1, i.y, i.z)];
    float c101 = thicknessLUT[LUT_IDX(i.x + 1, i.y, i.z + 1)];
    float c110 = thicknessLUT[LUT_IDX(i.x + 1, i.y + 1, i.z)];
    float c111 = thicknessLUT[LUT_IDX(i.x + 1, i.y + 1, i.z + 1)];
    
    float c00 = mix(c000, c001, f.z);
    float c01 = mix(c010, c011, f.z);
    float c10 = mix(c100, c101, f.z);
    float c11 = mix(c110, c111, f.z);
    
    float c0 = mix(c00, c01, f.y);
    float c1 = mix(c10, c11, f.y);
    
    float thickness = mix(c0, c1, f.x);
    
    // We want the surface at SDF = -thickness.
    // Standard DC extracts at value = 0.
    // So value = SDF - (-thickness) = SDF + thickness.
    // We ignore baseIsoValue if the LUT contains the full cumulative thickness.
    // If baseIsoValue is used, it might be an additional offset.
    // Let's assume LUT contains the full thickness.
    
    values[gid] = distance + thickness + baseIsoValue;
}

// Shell volume sampling kernel
// Computes SDF for a material shell (band between two depth boundaries)
// The shell is the region where: outerThickness <= -distance < innerThickness
// SDF is negative inside the shell, positive outside
__kernel void sampleCornersShellVolume(
    __global const float4* positions,    // Input: array of (x,y,z,_) positions
    __global float* values,              // Output: SDF values at positions
    const unsigned int count,            // Number of positions to sample
    PAYLOAD_ARGS,
    __global const float* outerLUT,      // 3D LUT: RGB -> outer boundary thickness
    __global const float* innerLUT,      // 3D LUT: RGB -> inner boundary thickness
    const int lutResolution,             // Resolution of the 3D LUTs (e.g. 16)
    const int isInnermostLayer           // 1 if innermost layer (no inner boundary)
)
{
    const int gid = get_global_id(0);
    
    if (gid >= count)
    {
        return;
    }
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Evaluate SDF at this position using the model function
    const float4 sdfResult = model(worldPos, PASS_PAYLOAD_ARGS);
    const float distance = sdfResult.w;
    const float3 color = clamp(sdfResult.xyz, 0.0f, 1.0f);
    
    // Trilinear interpolation setup
    float3 uvw = color * (float)(lutResolution - 1);
    int3 idx = convert_int3(floor(uvw));
    float3 f = uvw - convert_float3(idx);
    
    // Clamp indices
    idx = clamp(idx, 0, lutResolution - 2);
    
    // Helper macro for LUT access
    #define SHELL_LUT_IDX(x, y, z) (((x) * lutResolution + (y)) * lutResolution + (z))
    
    // Sample outer LUT with trilinear interpolation
    float o000 = outerLUT[SHELL_LUT_IDX(idx.x, idx.y, idx.z)];
    float o001 = outerLUT[SHELL_LUT_IDX(idx.x, idx.y, idx.z + 1)];
    float o010 = outerLUT[SHELL_LUT_IDX(idx.x, idx.y + 1, idx.z)];
    float o011 = outerLUT[SHELL_LUT_IDX(idx.x, idx.y + 1, idx.z + 1)];
    float o100 = outerLUT[SHELL_LUT_IDX(idx.x + 1, idx.y, idx.z)];
    float o101 = outerLUT[SHELL_LUT_IDX(idx.x + 1, idx.y, idx.z + 1)];
    float o110 = outerLUT[SHELL_LUT_IDX(idx.x + 1, idx.y + 1, idx.z)];
    float o111 = outerLUT[SHELL_LUT_IDX(idx.x + 1, idx.y + 1, idx.z + 1)];
    
    float o00 = mix(o000, o001, f.z);
    float o01 = mix(o010, o011, f.z);
    float o10 = mix(o100, o101, f.z);
    float o11 = mix(o110, o111, f.z);
    
    float o0 = mix(o00, o01, f.y);
    float o1 = mix(o10, o11, f.y);
    
    float outerThickness = mix(o0, o1, f.x);
    
    if (isInnermostLayer)
    {
        // Innermost layer: solid core from outer boundary inward
        // SDF is negative inside (distance < -outerThickness)
        values[gid] = distance + outerThickness;
    }
    else
    {
        // Sample inner LUT with trilinear interpolation
        float i000 = innerLUT[SHELL_LUT_IDX(idx.x, idx.y, idx.z)];
        float i001 = innerLUT[SHELL_LUT_IDX(idx.x, idx.y, idx.z + 1)];
        float i010 = innerLUT[SHELL_LUT_IDX(idx.x, idx.y + 1, idx.z)];
        float i011 = innerLUT[SHELL_LUT_IDX(idx.x, idx.y + 1, idx.z + 1)];
        float i100 = innerLUT[SHELL_LUT_IDX(idx.x + 1, idx.y, idx.z)];
        float i101 = innerLUT[SHELL_LUT_IDX(idx.x + 1, idx.y, idx.z + 1)];
        float i110 = innerLUT[SHELL_LUT_IDX(idx.x + 1, idx.y + 1, idx.z)];
        float i111 = innerLUT[SHELL_LUT_IDX(idx.x + 1, idx.y + 1, idx.z + 1)];
        
        float i00 = mix(i000, i001, f.z);
        float i01 = mix(i010, i011, f.z);
        float i10 = mix(i100, i101, f.z);
        float i11 = mix(i110, i111, f.z);
        
        float ii0 = mix(i00, i01, f.y);
        float ii1 = mix(i10, i11, f.y);
        
        float innerThickness = mix(ii0, ii1, f.x);
        
        // Shell volume using fabs() for closed surface
        // Shell centerline is at offset = (outer + inner) / 2
        // Shell half-thickness is (inner - outer) / 2
        // The fabs() naturally creates both inner and outer surfaces in one mesh!
        float shellCenter = (outerThickness + innerThickness) * 0.5f;
        float halfThickness = (innerThickness - outerThickness) * 0.5f;
        
        // Shell SDF: |distance_to_centerline| - halfThickness
        // Negative inside shell volume, positive outside
        // Iso-surface at 0 = closed shell boundary (both inner and outer faces)
        values[gid] = fabs(distance + shellCenter) - halfThickness;
    }
    
    #undef SHELL_LUT_IDX
}

// ============================================================================
// Surface-aligned thickness field sampling kernels
// These sample from a precomputed 3D thickness field instead of color→LUT
// ============================================================================

/// Helper: Transform world position to field grid coordinates and sample
inline float sampleThicknessField(
    __global const float* thicknessField,  // 3D grid of precomputed thickness values
    const int fieldResolution,              // Resolution of the 3D grid (e.g., 128)
    const float16 worldToGrid,              // 4x4 transform: world → grid coordinates
    const float3 worldPos)
{
    // Transform to grid coordinates using 4x4 matrix
    // worldToGrid is packed as: row0(0-3), row1(4-7), row2(8-11), row3(12-15)
    float3 gridPos;
    gridPos.x = worldToGrid.s0 * worldPos.x + worldToGrid.s1 * worldPos.y + 
                worldToGrid.s2 * worldPos.z + worldToGrid.s3;
    gridPos.y = worldToGrid.s4 * worldPos.x + worldToGrid.s5 * worldPos.y + 
                worldToGrid.s6 * worldPos.z + worldToGrid.s7;
    gridPos.z = worldToGrid.s8 * worldPos.x + worldToGrid.s9 * worldPos.y + 
                worldToGrid.sa * worldPos.z + worldToGrid.sb;
    
    // Trilinear interpolation in grid space
    float3 uvw = gridPos;
    int3 idx = convert_int3(floor(uvw));
    float3 f = uvw - convert_float3(idx);
    
    // Clamp to valid range
    idx = clamp(idx, 0, fieldResolution - 2);
    f = clamp(f, 0.0f, 1.0f);
    
    // Helper for 3D grid access (z-major order to match CPU: (z * res + y) * res + x)
    #define FIELD_IDX(x, y, z) (((z) * fieldResolution + (y)) * fieldResolution + (x))
    
    float c000 = thicknessField[FIELD_IDX(idx.x, idx.y, idx.z)];
    float c001 = thicknessField[FIELD_IDX(idx.x, idx.y, idx.z + 1)];
    float c010 = thicknessField[FIELD_IDX(idx.x, idx.y + 1, idx.z)];
    float c011 = thicknessField[FIELD_IDX(idx.x, idx.y + 1, idx.z + 1)];
    float c100 = thicknessField[FIELD_IDX(idx.x + 1, idx.y, idx.z)];
    float c101 = thicknessField[FIELD_IDX(idx.x + 1, idx.y, idx.z + 1)];
    float c110 = thicknessField[FIELD_IDX(idx.x + 1, idx.y + 1, idx.z)];
    float c111 = thicknessField[FIELD_IDX(idx.x + 1, idx.y + 1, idx.z + 1)];
    
    float c00 = mix(c000, c001, f.z);
    float c01 = mix(c010, c011, f.z);
    float c10 = mix(c100, c101, f.z);
    float c11 = mix(c110, c111, f.z);
    
    float c0 = mix(c00, c01, f.y);
    float c1 = mix(c10, c11, f.y);
    
    float thickness = mix(c0, c1, f.x);
    
    #undef FIELD_IDX
    
    return thickness;
}

/// Sample corners using precomputed surface-aligned thickness field (single boundary)
/// This is the surface-color-corrected version of sampleCornersVariableThickness
__kernel void sampleCornersWithThicknessField(
    __global const float4* positions,       // Input: world positions
    __global float* values,                 // Output: SDF values
    const unsigned int count,
    PAYLOAD_ARGS,
    const float baseIsoValue,               // Base ISO offset
    __global const float* thicknessField,   // 3D precomputed thickness grid
    const int fieldResolution,              // Grid resolution (e.g., 128)
    const float16 worldToGrid               // 4x4 transform matrix
)
{
    const int gid = get_global_id(0);
    if (gid >= count) return;
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Evaluate base SDF
    const float4 sdfResult = model(worldPos, PASS_PAYLOAD_ARGS);
    const float distance = sdfResult.w;
    
    // Sample thickness from precomputed field (surface colors propagated inward)
    float thickness = sampleThicknessField(
        thicknessField, fieldResolution, worldToGrid, worldPos);
    
    // Surface at SDF = -thickness
    values[gid] = distance + thickness + baseIsoValue;
}

/// Sample corners for shell volume using two precomputed thickness fields
/// This is the surface-color-corrected version of sampleCornersShellVolume
__kernel void sampleCornersShellVolumeWithField(
    __global const float4* positions,       // Input: world positions
    __global float* values,                 // Output: SDF values
    const unsigned int count,
    PAYLOAD_ARGS,
    __global const float* outerField,       // 3D precomputed outer boundary thickness
    __global const float* innerField,       // 3D precomputed inner boundary thickness
    const int fieldResolution,              // Grid resolution (e.g., 128)
    const float16 worldToGrid,              // 4x4 transform matrix
    const int isInnermostLayer              // 1 if innermost (no inner boundary)
)
{
    const int gid = get_global_id(0);
    if (gid >= count) return;
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Evaluate base SDF
    const float4 sdfResult = model(worldPos, PASS_PAYLOAD_ARGS);
    const float distance = sdfResult.w;
    
    // Sample outer thickness from precomputed field
    float outerThickness = sampleThicknessField(
        outerField, fieldResolution, worldToGrid, worldPos);
    
    if (isInnermostLayer)
    {
        // Innermost layer: solid core from outer boundary inward
        values[gid] = distance + outerThickness;
    }
    else
    {
        // Sample inner thickness from precomputed field
        float innerThickness = sampleThicknessField(
            innerField, fieldResolution, worldToGrid, worldPos);
        
        // Shell volume: CSG difference of outer and inner offset surfaces
        // Outer boundary at SDF = -outerThickness, inner at SDF = -innerThickness
        // Shell is intersection of (outside outer) and (inside inner)
        float outerSDF = distance + outerThickness;  // negative inside outer boundary
        float innerSDF = distance + innerThickness;  // negative inside inner boundary
        
        // max(outer, -inner) gives the shell between the two boundaries
        values[gid] = max(outerSDF, -innerSDF);
    }
}

// ============================================================================
// Shell-aware Hermite sampling with thickness field
// Computes both SDF value and gradient for the shell volume SDF
// ============================================================================

/// Helper: Compute shell SDF value at a position using thickness fields
inline float computeShellSdf(
    __global const float* outerField,
    __global const float* innerField,
    const int fieldResolution,
    const float16 worldToGrid,
    const float3 worldPos,
    const int isInnermostLayer,
    PAYLOAD_ARGS)
{
    // Evaluate base model SDF
    const float4 sdfResult = model(worldPos, PASS_PAYLOAD_ARGS);
    const float distance = sdfResult.w;
    
    // Sample outer thickness from precomputed field
    float outerThickness = sampleThicknessField(
        outerField, fieldResolution, worldToGrid, worldPos);
    
    if (isInnermostLayer)
    {
        // Innermost layer: solid core from outer boundary inward
        return distance + outerThickness;
    }
    else
    {
        // Sample inner thickness from precomputed field
        float innerThickness = sampleThicknessField(
            innerField, fieldResolution, worldToGrid, worldPos);
        
        // Shell volume: CSG difference of outer and inner offset surfaces
        float outerSDF = distance + outerThickness;
        float innerSDF = distance + innerThickness;
        
        return max(outerSDF, -innerSDF);
    }
}

/// Hermite data sampling for shell volumes using thickness fields
/// Computes shell SDF value and gradient at each position
__kernel void sampleHermiteShellVolumeWithField(
    __global const float4* positions,       // Input: world positions
    __global float* values,                 // Output: SDF values
    __global float4* gradients,             // Output: normalized gradients
    const unsigned int count,
    PAYLOAD_ARGS,
    __global const float* outerField,       // 3D precomputed outer boundary thickness
    __global const float* innerField,       // 3D precomputed inner boundary thickness
    const int fieldResolution,              // Grid resolution (e.g., 128)
    const float16 worldToGrid,              // 4x4 transform matrix
    const int isInnermostLayer,             // 1 if innermost (no inner boundary)
    const float epsilon)                    // Step size for gradient computation
{
    const int gid = get_global_id(0);
    if (gid >= count) return;
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Compute shell SDF value at center
    const float centerValue = computeShellSdf(
        outerField, innerField, fieldResolution, worldToGrid, 
        worldPos, isInnermostLayer, PASS_PAYLOAD_ARGS);
    values[gid] = centerValue;
    
    // Compute gradient using central differences on the shell SDF
    const float h = epsilon;
    const float h2 = 2.0f * h;
    
    const float3 posXp = worldPos + (float3)(h, 0.0f, 0.0f);
    const float3 posXn = worldPos - (float3)(h, 0.0f, 0.0f);
    const float3 posYp = worldPos + (float3)(0.0f, h, 0.0f);
    const float3 posYn = worldPos - (float3)(0.0f, h, 0.0f);
    const float3 posZp = worldPos + (float3)(0.0f, 0.0f, h);
    const float3 posZn = worldPos - (float3)(0.0f, 0.0f, h);
    
    const float sdfXp = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posXp, isInnermostLayer, PASS_PAYLOAD_ARGS);
    const float sdfXn = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posXn, isInnermostLayer, PASS_PAYLOAD_ARGS);
    const float sdfYp = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posYp, isInnermostLayer, PASS_PAYLOAD_ARGS);
    const float sdfYn = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posYn, isInnermostLayer, PASS_PAYLOAD_ARGS);
    const float sdfZp = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posZp, isInnermostLayer, PASS_PAYLOAD_ARGS);
    const float sdfZn = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posZn, isInnermostLayer, PASS_PAYLOAD_ARGS);
    
    float3 gradient;
    gradient.x = (sdfXp - sdfXn) / h2;
    gradient.y = (sdfYp - sdfYn) / h2;
    gradient.z = (sdfZp - sdfZn) / h2;
    
    // Normalize gradient
    const float gradLengthSq = dot(gradient, gradient);
    
    if (gradLengthSq > 1e-8f)
    {
        const float gradLength = sqrt(gradLengthSq);
        gradient /= gradLength;
    }
    else
    {
        // Degenerate gradient - try wider spacing
        const float h_wide = h * 5.0f;
        const float h2_wide = 2.0f * h_wide;
        
        const float3 posXp_wide = worldPos + (float3)(h_wide, 0.0f, 0.0f);
        const float3 posXn_wide = worldPos - (float3)(h_wide, 0.0f, 0.0f);
        const float3 posYp_wide = worldPos + (float3)(0.0f, h_wide, 0.0f);
        const float3 posYn_wide = worldPos - (float3)(0.0f, h_wide, 0.0f);
        const float3 posZp_wide = worldPos + (float3)(0.0f, 0.0f, h_wide);
        const float3 posZn_wide = worldPos - (float3)(0.0f, 0.0f, h_wide);
        
        const float sdfXp_wide = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posXp_wide, isInnermostLayer, PASS_PAYLOAD_ARGS);
        const float sdfXn_wide = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posXn_wide, isInnermostLayer, PASS_PAYLOAD_ARGS);
        const float sdfYp_wide = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posYp_wide, isInnermostLayer, PASS_PAYLOAD_ARGS);
        const float sdfYn_wide = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posYn_wide, isInnermostLayer, PASS_PAYLOAD_ARGS);
        const float sdfZp_wide = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posZp_wide, isInnermostLayer, PASS_PAYLOAD_ARGS);
        const float sdfZn_wide = computeShellSdf(outerField, innerField, fieldResolution, worldToGrid, posZn_wide, isInnermostLayer, PASS_PAYLOAD_ARGS);
        
        gradient.x = (sdfXp_wide - sdfXn_wide) / h2_wide;
        gradient.y = (sdfYp_wide - sdfYn_wide) / h2_wide;
        gradient.z = (sdfZp_wide - sdfZn_wide) / h2_wide;
        
        const float gradLengthSq_wide = dot(gradient, gradient);
        if (gradLengthSq_wide > 1e-10f)
        {
            gradient /= sqrt(gradLengthSq_wide);
        }
        else
        {
            // Last resort: use Z-up
            gradient = (float3)(0.0f, 0.0f, 1.0f);
        }
    }
    
    gradients[gid] = (float4)(gradient.x, gradient.y, gradient.z, 0.0f);
}
