// Hierarchical Dual Contouring GPU Kernels
// Level-by-level octree construction with adaptive refinement

// Kernel 1: Evaluate corners for all nodes at a given octree level
// Processes entire level in parallel (thousands of nodes simultaneously)
__kernel void evaluateOctreeLevel(
    __global const float* nodeBoundsMin,         // Input: min corners (x,y,z) per node
    __global const float* nodeBoundsMax,         // Input: max corners (x,y,z) per node
    __global float* cornerValues,                // Output: 8 SDF values per node
    const unsigned int nodeCount,                // Number of nodes at this level
    PAYLOAD_ARGS,
    const float isoValue)
{
    const int nodeId = get_global_id(0);
    
    if (nodeId >= nodeCount)
    {
        return;
    }
    
    // Load node bounds
    const float3 minBounds = (float3)(nodeBoundsMin[nodeId * 3 + 0],
                                      nodeBoundsMin[nodeId * 3 + 1],
                                      nodeBoundsMin[nodeId * 3 + 2]);
    const float3 maxBounds = (float3)(nodeBoundsMax[nodeId * 3 + 0],
                                      nodeBoundsMax[nodeId * 3 + 1],
                                      nodeBoundsMax[nodeId * 3 + 2]);
    
    // Evaluate SDF at all 8 corners
    // Corner ordering: (0,0,0), (1,0,0), (0,1,0), (1,1,0), (0,0,1), (1,0,1), (0,1,1), (1,1,1)
    for (int cornerIdx = 0; cornerIdx < 8; ++cornerIdx)
    {
        const float3 corner = (float3)(
            (cornerIdx & 1) ? maxBounds.x : minBounds.x,
            (cornerIdx & 2) ? maxBounds.y : minBounds.y,
            (cornerIdx & 4) ? maxBounds.z : minBounds.z
        );
        
        const float distance = model(corner, PASS_PAYLOAD_ARGS).w;
        
        cornerValues[nodeId * 8 + cornerIdx] = distance - isoValue;
    }
}

// Kernel 2: Detect sign changes and determine subdivision necessity
// Fast pass - no SDF queries, just arithmetic on corner values
__kernel void detectIntersections(
    __global const float* cornerValues,          // Input: 8 values per node
    __global uchar* subdivisionFlags,            // Output: 1 if should subdivide, 0 otherwise
    const unsigned int nodeCount)
{
    const int nodeId = get_global_id(0);
    
    if (nodeId >= nodeCount)
    {
        return;
    }
    
    // Check all 12 edges for sign changes
    // Edge list: pairs of corner indices
    const int edges[12][2] = {
        {0, 1}, {2, 3}, {4, 5}, {6, 7},  // X-aligned edges
        {0, 2}, {1, 3}, {4, 6}, {5, 7},  // Y-aligned edges
        {0, 4}, {1, 5}, {2, 6}, {3, 7}   // Z-aligned edges
    };
    
    bool hasZeroCrossing = false;
    
    for (int edgeIdx = 0; edgeIdx < 12; ++edgeIdx)
    {
        const int c0 = edges[edgeIdx][0];
        const int c1 = edges[edgeIdx][1];
        
        const float v0 = cornerValues[nodeId * 8 + c0];
        const float v1 = cornerValues[nodeId * 8 + c1];
        
        // Sign change = surface crossing
        if (v0 * v1 < 0.0f)
        {
            hasZeroCrossing = true;
            break;
        }
    }
    
    subdivisionFlags[nodeId] = hasZeroCrossing ? 1 : 0;
}

// Kernel 3: Estimate curvature for adaptive refinement
// Computes gradient variance as proxy for surface curvature
__kernel void estimateCurvature(
    __global const float* leafCenters,           // Input: center positions (x,y,z) per leaf
    __global float* curvatureMetrics,            // Output: curvature estimate per leaf
    const unsigned int leafCount,
    PAYLOAD_ARGS,
    const float gradientEpsilon)
{
    const int leafId = get_global_id(0);
    
    if (leafId >= leafCount)
    {
        return;
    }
    
    const float3 center = (float3)(leafCenters[leafId * 3 + 0],
                                   leafCenters[leafId * 3 + 1],
                                   leafCenters[leafId * 3 + 2]);
    
    // Compute gradient at center
    const float3 offsets[6] = {
        (float3)(gradientEpsilon, 0.0f, 0.0f),
        (float3)(-gradientEpsilon, 0.0f, 0.0f),
        (float3)(0.0f, gradientEpsilon, 0.0f),
        (float3)(0.0f, -gradientEpsilon, 0.0f),
        (float3)(0.0f, 0.0f, gradientEpsilon),
        (float3)(0.0f, 0.0f, -gradientEpsilon)
    };
    
    float3 gradients[7];
    
    // Center gradient
    const float3 posXp = center + offsets[0];
    const float3 posXn = center + offsets[1];
    const float sdfXp = model(posXp, PASS_PAYLOAD_ARGS).w;
    const float sdfXn = model(posXn, PASS_PAYLOAD_ARGS).w;
    
    const float3 posYp = center + offsets[2];
    const float3 posYn = center + offsets[3];
    const float sdfYp = model(posYp, PASS_PAYLOAD_ARGS).w;
    const float sdfYn = model(posYn, PASS_PAYLOAD_ARGS).w;
    
    const float3 posZp = center + offsets[4];
    const float3 posZn = center + offsets[5];
    const float sdfZp = model(posZp, PASS_PAYLOAD_ARGS).w;
    const float sdfZn = model(posZn, PASS_PAYLOAD_ARGS).w;
    
    gradients[0] = (float3)(
        (sdfXp - sdfXn) / (2.0f * gradientEpsilon),
        (sdfYp - sdfYn) / (2.0f * gradientEpsilon),
        (sdfZp - sdfZn) / (2.0f * gradientEpsilon)
    );
    
    // Normalize center gradient
    const float centerLen = length(gradients[0]);
    if (centerLen > 1e-6f)
    {
        gradients[0] = gradients[0] / centerLen;
    }
    
    // Compute gradients at 6 neighboring points
    for (int i = 0; i < 6; ++i)
    {
        const float3 neighborPos = center + offsets[i];
        
        const float3 nXp = neighborPos + (float3)(gradientEpsilon, 0.0f, 0.0f);
        const float3 nXn = neighborPos - (float3)(gradientEpsilon, 0.0f, 0.0f);
        const float nSdfXp = model(nXp, PASS_PAYLOAD_ARGS).w;
        const float nSdfXn = model(nXn, PASS_PAYLOAD_ARGS).w;
        
        const float3 nYp = neighborPos + (float3)(0.0f, gradientEpsilon, 0.0f);
        const float3 nYn = neighborPos - (float3)(0.0f, gradientEpsilon, 0.0f);
        const float nSdfYp = model(nYp, PASS_PAYLOAD_ARGS).w;
        const float nSdfYn = model(nYn, PASS_PAYLOAD_ARGS).w;
        
        const float3 nZp = neighborPos + (float3)(0.0f, 0.0f, gradientEpsilon);
        const float3 nZn = neighborPos - (float3)(0.0f, 0.0f, gradientEpsilon);
        const float nSdfZp = model(nZp, PASS_PAYLOAD_ARGS).w;
        const float nSdfZn = model(nZn, PASS_PAYLOAD_ARGS).w;
        
        gradients[i + 1] = (float3)(
            (nSdfXp - nSdfXn) / (2.0f * gradientEpsilon),
            (nSdfYp - nSdfYn) / (2.0f * gradientEpsilon),
            (nSdfZp - nSdfZn) / (2.0f * gradientEpsilon)
        );
        
        // Normalize
        const float neighborLen = length(gradients[i + 1]);
        if (neighborLen > 1e-6f)
        {
            gradients[i + 1] = gradients[i + 1] / neighborLen;
        }
    }
    
    // Compute variance of normalized gradients as curvature proxy
    float3 meanGradient = (float3)(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 7; ++i)
    {
        meanGradient += gradients[i];
    }
    meanGradient = meanGradient / 7.0f;
    
    float variance = 0.0f;
    for (int i = 0; i < 7; ++i)
    {
        const float3 diff = gradients[i] - meanGradient;
        variance += dot(diff, diff);
    }
    variance = variance / 7.0f;
    
    curvatureMetrics[leafId] = variance;
}

// Kernel 4: Batch gradient evaluation for Hermite samples
// Reuses existing gradient computation logic from dual_contouring_sampling.cl
__kernel void batchGradients(
    __global const float4* positions,            // Input: sample positions
    __global float4* gradients,                  // Output: gradient vectors
    const unsigned int count,
    PAYLOAD_ARGS,
    const float gradientEpsilon)
{
    const int gid = get_global_id(0);
    
    if (gid >= count)
    {
        return;
    }
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // X gradient
    const float3 posXp = worldPos + (float3)(gradientEpsilon, 0.0f, 0.0f);
    const float3 posXn = worldPos - (float3)(gradientEpsilon, 0.0f, 0.0f);
    const float sdfXp = model(posXp, PASS_PAYLOAD_ARGS).w;
    const float sdfXn = model(posXn, PASS_PAYLOAD_ARGS).w;
    
    // Y gradient
    const float3 posYp = worldPos + (float3)(0.0f, gradientEpsilon, 0.0f);
    const float3 posYn = worldPos - (float3)(0.0f, gradientEpsilon, 0.0f);
    const float sdfYp = model(posYp, PASS_PAYLOAD_ARGS).w;
    const float sdfYn = model(posYn, PASS_PAYLOAD_ARGS).w;
    
    // Z gradient
    const float3 posZp = worldPos + (float3)(0.0f, 0.0f, gradientEpsilon);
    const float3 posZn = worldPos - (float3)(0.0f, 0.0f, gradientEpsilon);
    const float sdfZp = model(posZp, PASS_PAYLOAD_ARGS).w;
    const float sdfZn = model(posZn, PASS_PAYLOAD_ARGS).w;
    
    const float3 gradient = (float3)(
        (sdfXp - sdfXn) / (2.0f * gradientEpsilon),
        (sdfYp - sdfYn) / (2.0f * gradientEpsilon),
        (sdfZp - sdfZn) / (2.0f * gradientEpsilon)
    );
    
    gradients[gid] = (float4)(gradient.x, gradient.y, gradient.z, 0.0f);
}
