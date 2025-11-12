// Dual Contouring GPU Sampling Kernels
// Provides batched SDF evaluation for octree corner and Hermite sampling

// Corner batch sampling kernel
// Evaluates SDF at a batch of 3D positions
__kernel void sampleCorners(
    __global const float4* positions,    // Input: array of (x,y,z,_) positions
    __global float* values,              // Output: SDF values at positions
    const unsigned int count,            // Number of positions to sample
    __global const float16* transformationMatrices,
    __global const uchar* opcodes,
    __global const float* dataBuffer,
    __global const uint* indexBuffer,
    const uint transformationMatrixCount,
    const uint opcodeCount,
    const float isoValue)
{
    const int gid = get_global_id(0);
    
    if (gid >= count)
    {
        return;
    }
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Evaluate SDF at this position using the existing SDF evaluation logic
    // This calls into the main SDF evaluation function that's already compiled
    const float distance = evaluateSdf(worldPos, 
                                       transformationMatrices,
                                       opcodes,
                                       dataBuffer,
                                       indexBuffer,
                                       transformationMatrixCount,
                                       opcodeCount);
    
    values[gid] = distance - isoValue;
}

// Hermite sampling kernel with gradient evaluation
// Computes both SDF value and gradient at each position
__kernel void sampleHermite(
    __global const float4* positions,    // Input: array of (x,y,z,_) positions
    __global float* values,              // Output: SDF values
    __global float4* gradients,          // Output: gradient vectors (x,y,z,_)
    const unsigned int count,            // Number of positions to sample
    const float gradientEpsilon,         // Finite difference step size
    __global const float16* transformationMatrices,
    __global const uchar* opcodes,
    __global const float* dataBuffer,
    __global const uint* indexBuffer,
    const uint transformationMatrixCount,
    const uint opcodeCount,
    const float isoValue)
{
    const int gid = get_global_id(0);
    
    if (gid >= count)
    {
        return;
    }
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Evaluate SDF at center position
    const float centerValue = evaluateSdf(worldPos,
                                          transformationMatrices,
                                          opcodes,
                                          dataBuffer,
                                          indexBuffer,
                                          transformationMatrixCount,
                                          opcodeCount);
    
    values[gid] = centerValue - isoValue;
    
    // Compute gradient using central differences
    const float3 epsilonVec = (float3)(gradientEpsilon, gradientEpsilon, gradientEpsilon);
    
    // X gradient
    const float3 posXp = worldPos + (float3)(epsilonVec.x, 0.0f, 0.0f);
    const float3 posXn = worldPos - (float3)(epsilonVec.x, 0.0f, 0.0f);
    const float sdfXp = evaluateSdf(posXp, transformationMatrices, opcodes, dataBuffer, 
                                    indexBuffer, transformationMatrixCount, opcodeCount);
    const float sdfXn = evaluateSdf(posXn, transformationMatrices, opcodes, dataBuffer,
                                    indexBuffer, transformationMatrixCount, opcodeCount);
    
    // Y gradient
    const float3 posYp = worldPos + (float3)(0.0f, epsilonVec.y, 0.0f);
    const float3 posYn = worldPos - (float3)(0.0f, epsilonVec.y, 0.0f);
    const float sdfYp = evaluateSdf(posYp, transformationMatrices, opcodes, dataBuffer,
                                    indexBuffer, transformationMatrixCount, opcodeCount);
    const float sdfYn = evaluateSdf(posYn, transformationMatrices, opcodes, dataBuffer,
                                    indexBuffer, transformationMatrixCount, opcodeCount);
    
    // Z gradient
    const float3 posZp = worldPos + (float3)(0.0f, 0.0f, epsilonVec.z);
    const float3 posZn = worldPos - (float3)(0.0f, 0.0f, epsilonVec.z);
    const float sdfZp = evaluateSdf(posZp, transformationMatrices, opcodes, dataBuffer,
                                    indexBuffer, transformationMatrixCount, opcodeCount);
    const float sdfZn = evaluateSdf(posZn, transformationMatrices, opcodes, dataBuffer,
                                    indexBuffer, transformationMatrixCount, opcodeCount);
    
    // Store gradient (normalized in host code if needed)
    const float3 gradient = (float3)(
        (sdfXp - sdfXn) / (2.0f * epsilonVec.x),
        (sdfYp - sdfYn) / (2.0f * epsilonVec.y),
        (sdfZp - sdfZn) / (2.0f * epsilonVec.z)
    );
    
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
