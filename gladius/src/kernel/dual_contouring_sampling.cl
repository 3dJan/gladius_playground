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
    
    // Compute gradient using central differences (7-point stencil)
    // Sample at 6 offset positions: +/- epsilon in each axis
    const float3 posXp = worldPos + (float3)(epsilon, 0.0f, 0.0f);
    const float3 posXn = worldPos - (float3)(epsilon, 0.0f, 0.0f);
    const float3 posYp = worldPos + (float3)(0.0f, epsilon, 0.0f);
    const float3 posYn = worldPos - (float3)(0.0f, epsilon, 0.0f);
    const float3 posZp = worldPos + (float3)(0.0f, 0.0f, epsilon);
    const float3 posZn = worldPos - (float3)(0.0f, 0.0f, epsilon);
    
    const float sdfXp = model(posXp, PASS_PAYLOAD_ARGS).w;
    const float sdfXn = model(posXn, PASS_PAYLOAD_ARGS).w;
    const float sdfYp = model(posYp, PASS_PAYLOAD_ARGS).w;
    const float sdfYn = model(posYn, PASS_PAYLOAD_ARGS).w;
    const float sdfZp = model(posZp, PASS_PAYLOAD_ARGS).w;
    const float sdfZn = model(posZn, PASS_PAYLOAD_ARGS).w;
    
    // Central difference formula: (f(x+h) - f(x-h)) / 2h
    float3 gradient;
    gradient.x = (sdfXp - sdfXn) / (2.0f * epsilon);
    gradient.y = (sdfYp - sdfYn) / (2.0f * epsilon);
    gradient.z = (sdfZp - sdfZn) / (2.0f * epsilon);
    
    // Normalize the gradient
    const float gradLength = length(gradient);
    if (gradLength > 1e-7f)
    {
        gradient /= gradLength;
    }
    else
    {
        // Fallback for degenerate gradients
        gradient = (float3)(0.0f, 1.0f, 0.0f);
    }
    
    gradients[gid] = (float4)(gradient.x, gradient.y, gradient.z, 0.0f);
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
