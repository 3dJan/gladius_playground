/// GPU kernel for batch SDF evaluation during mesh simplification
/// This kernel evaluates the SDF at multiple positions in parallel,
/// providing exact SDF error measurement without grid interpolation loss.

/// Evaluate SDF at multiple positions for mesh simplification
/// 
/// This kernel is used by QemMeshSimplifier to evaluate geometric error
/// at edge collapse candidate positions. Each work item evaluates one position.
///
/// @param positions     Input: 3D positions to evaluate (float3 array)
/// @param sdfValues     Output: SDF values at each position (float array)
/// @param positionCount Number of positions to evaluate
/// @param isoValue      Iso-surface value (typically 0)
__kernel void evaluateSdfBatch(
    __global const float* positions,       // 3 floats per position
    __global float* sdfValues,             // 1 float per position (output)
    const uint positionCount,
    const float isoValue
    PAYLOAD_ARGS)
{
    uint const id = get_global_id(0);
    if (id >= positionCount)
    {
        return;
    }

    // Load position
    float3 const pos = vload3(id, positions);

    // Evaluate SDF using the model function
    float4 const result = model(pos, PASS_PAYLOAD_ARGS);
    float const sdf = result.w - isoValue;

    // Store result
    sdfValues[id] = sdf;
}

/// Evaluate SDF and gradient at multiple positions
/// 
/// This kernel provides both SDF values and surface normals,
/// useful for validating that collapsed vertices stay close to the surface.
///
/// @param positions     Input: 3D positions to evaluate
/// @param sdfValues     Output: SDF values at each position
/// @param gradients     Output: Surface normals (gradients) at each position
/// @param positionCount Number of positions to evaluate
/// @param isoValue      Iso-surface value
/// @param epsilon       Step size for finite difference gradient computation
__kernel void evaluateSdfWithGradientBatch(
    __global const float* positions,       // 3 floats per position
    __global float* sdfValues,             // 1 float per position (output)
    __global float* gradients,             // 3 floats per position (output)
    const uint positionCount,
    const float isoValue,
    const float epsilon
    PAYLOAD_ARGS)
{
    uint const id = get_global_id(0);
    if (id >= positionCount)
    {
        return;
    }

    // Load position
    float3 const pos = vload3(id, positions);

    // Evaluate SDF
    float4 const result = model(pos, PASS_PAYLOAD_ARGS);
    float const sdf = result.w - isoValue;
    sdfValues[id] = sdf;

    // Compute gradient using central differences
    float const dx_pos = model(pos + (float3)(epsilon, 0.0f, 0.0f), PASS_PAYLOAD_ARGS).w;
    float const dx_neg = model(pos - (float3)(epsilon, 0.0f, 0.0f), PASS_PAYLOAD_ARGS).w;
    float const dy_pos = model(pos + (float3)(0.0f, epsilon, 0.0f), PASS_PAYLOAD_ARGS).w;
    float const dy_neg = model(pos - (float3)(0.0f, epsilon, 0.0f), PASS_PAYLOAD_ARGS).w;
    float const dz_pos = model(pos + (float3)(0.0f, 0.0f, epsilon), PASS_PAYLOAD_ARGS).w;
    float const dz_neg = model(pos - (float3)(0.0f, 0.0f, epsilon), PASS_PAYLOAD_ARGS).w;

    float3 grad = (float3)(
        (dx_pos - dx_neg) / (2.0f * epsilon),
        (dy_pos - dy_neg) / (2.0f * epsilon),
        (dz_pos - dz_neg) / (2.0f * epsilon)
    );

    // Normalize gradient to get surface normal
    float const gradLen = length(grad);
    if (gradLen > 1e-6f)
    {
        grad /= gradLen;
    }

    vstore3(grad, id, gradients);
}

/// Evaluate edge collapse validity using SDF
///
/// For each edge (defined by two vertex indices), this kernel:
/// 1. Computes the edge midpoint
/// 2. Evaluates SDF at the midpoint
/// 3. Computes SDF at the optimal QEM position (if provided)
/// 4. Returns the maximum SDF error
///
/// @param vertexPositions   All mesh vertex positions
/// @param edgeVertexA       First vertex index for each edge
/// @param edgeVertexB       Second vertex index for each edge
/// @param optimalPositions  QEM-computed optimal collapse positions (or midpoint if not available)
/// @param sdfErrors         Output: Maximum SDF error for each edge
/// @param edgeCount         Number of edges to evaluate
/// @param isoValue          Iso-surface value
__kernel void evaluateEdgeCollapseError(
    __global const float* vertexPositions,   // All vertex positions (3 floats per vertex)
    __global const uint* edgeVertexA,        // First vertex index per edge
    __global const uint* edgeVertexB,        // Second vertex index per edge
    __global const float* optimalPositions,  // Optimal collapse position per edge (3 floats)
    __global float* sdfErrors,               // Output: max SDF error per edge
    const uint edgeCount,
    const float isoValue
    PAYLOAD_ARGS)
{
    uint const id = get_global_id(0);
    if (id >= edgeCount)
    {
        return;
    }

    // Load vertex indices
    uint const vA = edgeVertexA[id];
    uint const vB = edgeVertexB[id];

    // Load vertex positions
    float3 const posA = vload3(vA, vertexPositions);
    float3 const posB = vload3(vB, vertexPositions);

    // Load optimal position
    float3 const optimalPos = vload3(id, optimalPositions);

    // Evaluate SDF at multiple points along the edge and at optimal position
    float maxError = 0.0f;

    // Evaluate at optimal position
    float const sdfOptimal = fabs(model(optimalPos, PASS_PAYLOAD_ARGS).w - isoValue);
    maxError = fmax(maxError, sdfOptimal);

    // Evaluate at edge midpoint
    float3 const midpoint = (posA + posB) * 0.5f;
    float const sdfMid = fabs(model(midpoint, PASS_PAYLOAD_ARGS).w - isoValue);
    maxError = fmax(maxError, sdfMid);

    // Evaluate at 1/4 and 3/4 points for better coverage
    float3 const quarter = posA * 0.75f + posB * 0.25f;
    float3 const threeQuarter = posA * 0.25f + posB * 0.75f;
    float const sdfQuarter = fabs(model(quarter, PASS_PAYLOAD_ARGS).w - isoValue);
    float const sdfThreeQuarter = fabs(model(threeQuarter, PASS_PAYLOAD_ARGS).w - isoValue);
    maxError = fmax(maxError, fmax(sdfQuarter, sdfThreeQuarter));

    sdfErrors[id] = maxError;
}

/// Detect sharp feature edges based on gradient discontinuity
///
/// For each edge, evaluate the surface normal at both endpoints and
/// compute the angle between them. Sharp features are detected when
/// the angle exceeds a threshold.
///
/// @param vertexPositions   All mesh vertex positions
/// @param edgeVertexA       First vertex index for each edge
/// @param edgeVertexB       Second vertex index for each edge
/// @param isSharpFeature    Output: 1 if sharp feature, 0 otherwise
/// @param edgeCount         Number of edges
/// @param angleThreshold    Cosine of angle threshold (e.g., 0.7 for ~45°)
/// @param epsilon           Step size for gradient computation
__kernel void detectSharpFeatureEdges(
    __global const float* vertexPositions,
    __global const uint* edgeVertexA,
    __global const uint* edgeVertexB,
    __global uchar* isSharpFeature,
    const uint edgeCount,
    const float angleThreshold,
    const float epsilon
    PAYLOAD_ARGS)
{
    uint const id = get_global_id(0);
    if (id >= edgeCount)
    {
        return;
    }

    uint const vA = edgeVertexA[id];
    uint const vB = edgeVertexB[id];

    float3 const posA = vload3(vA, vertexPositions);
    float3 const posB = vload3(vB, vertexPositions);

    // Compute gradient at position A using central differences
    float3 gradA;
    {
        float const dx_pos = model(posA + (float3)(epsilon, 0.0f, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dx_neg = model(posA - (float3)(epsilon, 0.0f, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dy_pos = model(posA + (float3)(0.0f, epsilon, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dy_neg = model(posA - (float3)(0.0f, epsilon, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dz_pos = model(posA + (float3)(0.0f, 0.0f, epsilon), PASS_PAYLOAD_ARGS).w;
        float const dz_neg = model(posA - (float3)(0.0f, 0.0f, epsilon), PASS_PAYLOAD_ARGS).w;
        gradA = normalize((float3)(dx_pos - dx_neg, dy_pos - dy_neg, dz_pos - dz_neg));
    }

    // Compute gradient at position B
    float3 gradB;
    {
        float const dx_pos = model(posB + (float3)(epsilon, 0.0f, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dx_neg = model(posB - (float3)(epsilon, 0.0f, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dy_pos = model(posB + (float3)(0.0f, epsilon, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dy_neg = model(posB - (float3)(0.0f, epsilon, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dz_pos = model(posB + (float3)(0.0f, 0.0f, epsilon), PASS_PAYLOAD_ARGS).w;
        float const dz_neg = model(posB - (float3)(0.0f, 0.0f, epsilon), PASS_PAYLOAD_ARGS).w;
        gradB = normalize((float3)(dx_pos - dx_neg, dy_pos - dy_neg, dz_pos - dz_neg));
    }

    // Compute gradient at edge midpoint
    float3 const midpoint = (posA + posB) * 0.5f;
    float3 gradMid;
    {
        float const dx_pos = model(midpoint + (float3)(epsilon, 0.0f, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dx_neg = model(midpoint - (float3)(epsilon, 0.0f, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dy_pos = model(midpoint + (float3)(0.0f, epsilon, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dy_neg = model(midpoint - (float3)(0.0f, epsilon, 0.0f), PASS_PAYLOAD_ARGS).w;
        float const dz_pos = model(midpoint + (float3)(0.0f, 0.0f, epsilon), PASS_PAYLOAD_ARGS).w;
        float const dz_neg = model(midpoint - (float3)(0.0f, 0.0f, epsilon), PASS_PAYLOAD_ARGS).w;
        gradMid = normalize((float3)(dx_pos - dx_neg, dy_pos - dy_neg, dz_pos - dz_neg));
    }

    // Compute dot products to detect discontinuity
    float const dotAB = dot(gradA, gradB);
    float const dotAMid = dot(gradA, gradMid);
    float const dotBMid = dot(gradB, gradMid);

    // Sharp feature if any gradient pair has low dot product
    float const minDot = fmin(dotAB, fmin(dotAMid, dotBMid));
    
    isSharpFeature[id] = (minDot < angleThreshold) ? 1 : 0;
}
