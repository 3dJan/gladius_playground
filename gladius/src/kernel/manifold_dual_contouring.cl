/// Octree node structure matching the host side
typedef struct __attribute__((packed))
{
    ulong mortonCode;     ///< Z-order space-filling curve position
    uint edgeMask;        ///< Bit mask indicating which of 12 edges cross the surface
    uint internalMask;    ///< Bit mask for which of 8 corners are inside the surface
    uchar depth;          ///< Octree depth level (0 = root)
    uchar padding[7];     ///< Padding for 24-byte alignment
} OctreeNode;

typedef struct __attribute__((packed))
{
    float4 position; // w is padding
    float4 normal;   // w is padding
} Vertex;

// Helper functions for Morton codes

/// Spreads bits of a 21-bit number to every third bit position
ulong expandBits(ulong v)
{
    v = (v | (v << 32)) & 0x1f00000000ffffUL;
    v = (v | (v << 16)) & 0x1f0000ff0000ffUL;
    v = (v | (v << 8))  & 0x100f00f00f00f00fUL;
    v = (v | (v << 4))  & 0x10c30c30c30c30c3UL;
    v = (v | (v << 2))  & 0x1249249249249249UL;
    return v;
}

/// Encodes 3D coordinates into Morton code (Z-order curve)
ulong encodeMorton3(ulong x, ulong y, ulong z)
{
    return (expandBits(z) << 2) | (expandBits(y) << 1) | expandBits(x);
}

/// Compacts every third bit
ulong compactBits(ulong v)
{
    v &= 0x1249249249249249UL;
    v = (v | (v >> 2))  & 0x10c30c30c30c30c3UL;
    v = (v | (v >> 4))  & 0x100f00f00f00f00fUL;
    v = (v | (v >> 8))  & 0x1f0000ff0000ffUL;
    v = (v | (v >> 16)) & 0x1f00000000ffffUL;
    v = (v | (v >> 32)) & 0x1fffffUL;
    return v;
}

/// Decodes Morton code back to 3D coordinates
ulong3 decodeMorton3(ulong m)
{
    ulong x = compactBits(m);
    ulong y = compactBits(m >> 1);
    ulong z = compactBits(m >> 2);
    return (ulong3)(x, y, z);
}

/// Get the per-axis cell extents at a given depth
float3 getCellExtent(float3 bboxMin, float3 bboxMax, uint depth)
{
    float3 extent = bboxMax - bboxMin;
    float scale = 1.0f / (float)(1u << depth);
    return extent * scale;
}

// Each cell with surface intersection generates exactly 1 vertex
__kernel void count_vertices(
    __global OctreeNode* nodes,
    __global int* countBuffer,
    const int numNodes,
    const float3 bboxMin,
    const float3 bboxMax,
    PAYLOAD_ARGS,
    const float isoValue)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;
    
    OctreeNode node = nodes[id];
    
    // If edgeMask is 0, no edges cross the surface -> no vertex
    // If edgeMask != 0, we have surface crossing -> generate 1 vertex
    countBuffer[id] = (node.edgeMask != 0) ? 1 : 0;
}

// QEF (Quadratic Error Function) Solver
// The QEF solver finds the optimal vertex position that minimizes the sum of
// squared distances to all tangent planes defined by edge intersections.
// This naturally preserves sharp edges where multiple planes meet.

typedef struct
{
    float3 position;
    float3 normal;
    float error;
} QefResult;

// ============================================================================
// SVD-based QEF Solver for Sharp Feature Preservation
// ============================================================================

/// 3x3 matrix structure for QEF solving
typedef struct
{
    float m[3][3];
} Mat3;

/// Multiply 3x3 matrix by vector
float3 mat3MulVec(Mat3 const * m, float3 v)
{
    return (float3)(
        m->m[0][0] * v.x + m->m[0][1] * v.y + m->m[0][2] * v.z,
        m->m[1][0] * v.x + m->m[1][1] * v.y + m->m[1][2] * v.z,
        m->m[2][0] * v.x + m->m[2][1] * v.y + m->m[2][2] * v.z
    );
}

/// Multiply two 3x3 matrices
Mat3 mat3Mul(Mat3 const * a, Mat3 const * b)
{
    Mat3 result;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            result.m[i][j] = 0.0f;
            for (int k = 0; k < 3; k++)
            {
                result.m[i][j] += a->m[i][k] * b->m[k][j];
            }
        }
    }
    return result;
}

/// Transpose a 3x3 matrix
Mat3 mat3Transpose(Mat3 const * m)
{
    Mat3 result;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            result.m[i][j] = m->m[j][i];
        }
    }
    return result;
}

/// Compute the symmetric matrix A^T * A from normals
/// Each row of A is a normal vector n_i
/// A^T * A = sum of outer products n_i * n_i^T
Mat3 computeATA(float3 const * normals, int count)
{
    Mat3 ata;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            ata.m[i][j] = 0.0f;
        }
    }
    
    for (int k = 0; k < count; k++)
    {
        float3 n = normals[k];
        // Outer product n * n^T
        ata.m[0][0] += n.x * n.x;
        ata.m[0][1] += n.x * n.y;
        ata.m[0][2] += n.x * n.z;
        ata.m[1][0] += n.y * n.x;
        ata.m[1][1] += n.y * n.y;
        ata.m[1][2] += n.y * n.z;
        ata.m[2][0] += n.z * n.x;
        ata.m[2][1] += n.z * n.y;
        ata.m[2][2] += n.z * n.z;
    }
    
    return ata;
}

/// Compute A^T * b where b_i = dot(n_i, p_i)
float3 computeATb(float3 const * normals, float3 const * points, int count)
{
    float3 atb = (float3)(0.0f, 0.0f, 0.0f);
    
    for (int k = 0; k < count; k++)
    {
        float b_k = dot(normals[k], points[k]);
        atb += normals[k] * b_k;
    }
    
    return atb;
}

/// Solve 3x3 symmetric positive semi-definite system using Cholesky with regularization
/// Returns solution to (A^T A + lambda*I) x = A^T b
float3 solveSymmetricSystem(Mat3 const * ata, float3 atb, float lambda)
{
    // Add regularization to handle rank-deficient cases (sharp edges, corners)
    Mat3 m = *ata;
    m.m[0][0] += lambda;
    m.m[1][1] += lambda;
    m.m[2][2] += lambda;
    
    // Solve using Cramer's rule (simple and robust for 3x3)
    float det = m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1])
              - m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0])
              + m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
    
    if (fabs(det) < 1e-10f)
    {
        // Matrix still singular, return zero (will fall back to mass point)
        return (float3)(0.0f, 0.0f, 0.0f);
    }
    
    float invDet = 1.0f / det;
    
    // Compute inverse using adjugate matrix
    float3 result;
    result.x = invDet * (
        atb.x * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) +
        atb.y * (m.m[0][2] * m.m[2][1] - m.m[0][1] * m.m[2][2]) +
        atb.z * (m.m[0][1] * m.m[1][2] - m.m[0][2] * m.m[1][1])
    );
    result.y = invDet * (
        atb.x * (m.m[1][2] * m.m[2][0] - m.m[1][0] * m.m[2][2]) +
        atb.y * (m.m[0][0] * m.m[2][2] - m.m[0][2] * m.m[2][0]) +
        atb.z * (m.m[0][2] * m.m[1][0] - m.m[0][0] * m.m[1][2])
    );
    result.z = invDet * (
        atb.x * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]) +
        atb.y * (m.m[0][1] * m.m[2][0] - m.m[0][0] * m.m[2][1]) +
        atb.z * (m.m[0][0] * m.m[1][1] - m.m[0][1] * m.m[1][0])
    );
    
    return result;
}

/// Compute QEF error: sum of squared distances to tangent planes
float computeQefError(float3 vertex, float3 const * normals, float3 const * points, int count)
{
    float error = 0.0f;
    for (int i = 0; i < count; i++)
    {
        float dist = dot(normals[i], vertex - points[i]);
        error += dist * dist;
    }
    return error;
}

/// Check if normals are consistent (all pointing roughly the same direction)
/// Returns a value between 0 (inconsistent) and 1 (fully consistent)
float computeNormalConsistency(float3 const * normals, int count, float3 avgNormal)
{
    if (count <= 1) return 1.0f;
    
    float minDot = 1.0f;
    for (int i = 0; i < count; i++)
    {
        float d = dot(normals[i], avgNormal);
        minDot = fmin(minDot, d);
    }
    // Map from [-1, 1] to [0, 1]
    return clamp((minDot + 1.0f) * 0.5f, 0.0f, 1.0f);
}

/// Compute the spread of normals - measures how "diverse" the normal directions are
/// Returns 0 for parallel normals, higher values for more spread
float computeNormalSpread(float3 const * normals, int count)
{
    if (count <= 1) return 0.0f;
    
    float minPairwiseDot = 1.0f;
    for (int i = 0; i < count; i++)
    {
        for (int j = i + 1; j < count; j++)
        {
            float d = dot(normals[i], normals[j]);
            minPairwiseDot = fmin(minPairwiseDot, d);
        }
    }
    // Returns 0 for parallel normals, 1 for perpendicular, 2 for opposite
    return 1.0f - minPairwiseDot;
}

/// Check if a displacement direction is supported by the normals
/// Returns how well the displacement aligns with the "inward" direction from the surface
/// For sharp features, we expect displacement to point INTO the corner/edge
float computeDisplacementSupport(float3 displacement, float3 const * normals, int count)
{
    if (count == 0) return 0.0f;
    
    float dispLen = length(displacement);
    if (dispLen < 1e-6f) return 1.0f;  // No displacement is always safe
    
    float3 dispDir = displacement / dispLen;
    
    // For a valid sharp feature vertex, the displacement should have
    // NEGATIVE dot product with most normals (moving INTO the corner)
    // or POSITIVE dot product (moving along the edge)
    // But it should NOT be perpendicular to ALL normals
    
    float maxAbsDot = 0.0f;
    int supportCount = 0;
    for (int i = 0; i < count; i++)
    {
        float d = dot(dispDir, normals[i]);
        maxAbsDot = fmax(maxAbsDot, fabs(d));
        // Count how many normals somewhat support this displacement direction
        if (fabs(d) > 0.1f) supportCount++;
    }
    
    // If displacement is nearly perpendicular to all normals, it's suspicious
    // (indicates the solver found a spurious minimum)
    float support = maxAbsDot;
    
    // Bonus if multiple normals support the displacement
    if (supportCount > 1)
    {
        support = fmin(support + 0.2f, 1.0f);
    }
    
    return support;
}

/// Estimate condition number of ATA matrix using trace and Frobenius norm ratio
/// Returns a value between 0 (ill-conditioned) and 1 (well-conditioned)
float estimateMatrixCondition(Mat3 const * ata)
{
    // Frobenius norm squared
    float frobSq = 0.0f;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            frobSq += ata->m[i][j] * ata->m[i][j];
        }
    }
    
    // Trace (sum of eigenvalues)
    float trace = ata->m[0][0] + ata->m[1][1] + ata->m[2][2];
    
    if (trace < 1e-10f) return 0.0f;
    
    // For well-conditioned matrices, frobSq ~ trace^2 / 3
    // For ill-conditioned, one eigenvalue dominates so frobSq >> trace^2 / 3
    float expectedFrobSq = trace * trace / 3.0f;
    float ratio = expectedFrobSq / fmax(frobSq, 1e-10f);
    
    return clamp(ratio, 0.0f, 1.0f);
}

/// Solve QEF using least squares with regularization for sharp feature preservation
/// This finds the vertex position that minimizes distance to all tangent planes
/// Sharp edges and corners are naturally preserved because the solution falls
/// on the intersection of the tangent planes.
QefResult solveQefSvd(float3* intersections, float3* normals, int count, float3 cellMin, float3 cellMax)
{
    QefResult result;
    float3 cellCenter = (cellMin + cellMax) * 0.5f;
    float3 cellSize = cellMax - cellMin;
    float cellDiag = length(cellSize);
    float cellRadius = cellDiag * 0.5f;
    
    if (count == 0)
    {
        result.position = cellCenter;
        result.normal = (float3)(0.0f, 0.0f, 1.0f);
        result.error = 0.0f;
        return result;
    }
    
    // Compute mass point as initial guess and fallback
    float3 massPoint = (float3)(0.0f, 0.0f, 0.0f);
    float3 avgNormal = (float3)(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < count; i++)
    {
        massPoint += intersections[i];
        avgNormal += normals[i];
    }
    massPoint /= (float)count;
    
    float avgNormalLen = length(avgNormal);
    if (avgNormalLen > 1e-6f)
    {
        avgNormal /= avgNormalLen;
    }
    else
    {
        avgNormal = (float3)(0.0f, 1.0f, 0.0f);
    }
    
    // Check normal consistency - if normals point in very different directions,
    // we have a sharp feature; if they're inconsistent (some opposite), use mass point
    float normalConsistency = computeNormalConsistency(normals, count, avgNormal);
    
    // Build the normal equations: A^T A x = A^T b
    Mat3 ata = computeATA(normals, count);
    float3 atb = computeATb(normals, intersections, count);
    
    // Check matrix conditioning
    float matrixCondition = estimateMatrixCondition(&ata);
    
    // Compute confidence in QEF solution based on conditioning and normal consistency
    // Low consistency (mixed normals) is OK for sharp features, but combined with
    // poor conditioning suggests numerical issues
    float qefConfidence = matrixCondition;
    if (normalConsistency < 0.3f && matrixCondition < 0.5f)
    {
        // Both poor - likely numerical issues, fall back to mass point
        qefConfidence = 0.0f;
    }
    
    float3 qefVertex = massPoint;  // Default to mass point
    
    // Only fall back to mass point if matrix is severely ill-conditioned
    // Otherwise solve the QEF to preserve sharp features
    if (qefConfidence > 0.01f)
    {
        // Solve with mild regularization
        float3 atbShifted = atb - mat3MulVec(&ata, massPoint);
        
        // Light regularization - just enough for numerical stability
        float lambda = 0.01f * (float)count;
        float3 solution = solveSymmetricSystem(&ata, atbShifted, lambda);
        
        // Shift back to world coordinates
        qefVertex = massPoint + solution;
    }
    
    // Check if the displacement is supported by the normals
    // This helps detect artifact-producing solutions
    float3 qefDisplacement = qefVertex - massPoint;
    float dispSupport = computeDisplacementSupport(qefDisplacement, normals, count);
    
    // Compute normal spread to detect sharp features
    float normalSpread = computeNormalSpread(normals, count);
    
    // For sharp features (high spread), we need good displacement support
    // If displacement is poorly supported (perpendicular to all normals), it's likely an artifact
    if (normalSpread > 0.5f && dispSupport < 0.3f)
    {
        // Suspicious displacement - blend toward mass point
        float blend = dispSupport / 0.3f;  // 0 to 1 as support goes from 0 to 0.3
        qefVertex = mix(massPoint, qefVertex, blend);
    }
    
    // Compute displacement from cell center (not mass point) 
    // This allows vertex to move toward sharp features
    float3 displacement = qefVertex - cellCenter;
    float dispLen = length(displacement);
    
    // Allow displacement up to half the cell diagonal - this is necessary
    // for vertices to reach edges and corners of the cell for sharp features
    float maxDisp = cellDiag * 0.5f;
    
    if (dispLen > maxDisp)
    {
        // Limit displacement magnitude
        displacement = displacement * (maxDisp / dispLen);
        qefVertex = cellCenter + displacement;
    }
    
    // Final clamp to cell bounds
    result.position = clamp(qefVertex, cellMin, cellMax);
    result.normal = avgNormal;
    result.error = computeQefError(result.position, normals, intersections, count);
    
    return result;
}

/// Find intersection point on an edge using linear interpolation
float3 findEdgeIntersection(float3 p0, float3 p1, float v0, float v1)
{
    // Linear interpolation to find zero crossing
    if (fabs(v1 - v0) < 1e-6f) {
        return (p0 + p1) * 0.5f;
    }
    float t = -v0 / (v1 - v0);
    t = clamp(t, 0.0f, 1.0f);
    return p0 + t * (p1 - p0);
}

/// Compute gradient (normal) at a point using finite differences
float3 computeGradient(float3 pos, PAYLOAD_ARGS)
{
    // Central difference gradient using model evaluation
    const float h = 0.001f;
    const float h2 = 2.0f * h;
    
    const float3 posXp = pos + (float3)(h, 0.0f, 0.0f);
    const float3 posXn = pos - (float3)(h, 0.0f, 0.0f);
    const float3 posYp = pos + (float3)(0.0f, h, 0.0f);
    const float3 posYn = pos - (float3)(0.0f, h, 0.0f);
    const float3 posZp = pos + (float3)(0.0f, 0.0f, h);
    const float3 posZn = pos - (float3)(0.0f, 0.0f, h);
    
    const float sdfXp = model(posXp, PASS_PAYLOAD_ARGS).w;
    const float sdfXn = model(posXn, PASS_PAYLOAD_ARGS).w;
    const float sdfYp = model(posYp, PASS_PAYLOAD_ARGS).w;
    const float sdfYn = model(posYn, PASS_PAYLOAD_ARGS).w;
    const float sdfZp = model(posZp, PASS_PAYLOAD_ARGS).w;
    const float sdfZn = model(posZn, PASS_PAYLOAD_ARGS).w;
    
    float3 gradient;
    gradient.x = (sdfXp - sdfXn) / h2;
    gradient.y = (sdfYp - sdfYn) / h2;
    gradient.z = (sdfZp - sdfZn) / h2;
    
    const float gradLengthSq = dot(gradient, gradient);
    if (gradLengthSq > 1e-8f) {
        gradient /= sqrt(gradLengthSq);
    } else {
        gradient = (float3)(0.0f, 1.0f, 0.0f);
    }
    
    return gradient;
}

/// Generate one vertex per cell using SVD-based QEF solver for sharp edge preservation
__kernel void emit_vertices(
    __global OctreeNode const* nodes,
    __global int const* offsets,
    __global Vertex* outputVertices,
    const int numNodes,
    const float3 bboxMin,
    const float3 bboxMax,
    PAYLOAD_ARGS,
    const float isoValue)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;

    OctreeNode node = nodes[id];
    int vertexIndex = offsets[id];
    
    // Skip cells with no surface intersection
    if (node.edgeMask == 0) return;
    
    // Get cell bounds
    ulong3 coords = decodeMorton3(node.mortonCode);
    uint depth = node.depth;
    float3 cellExtent = getCellExtent(bboxMin, bboxMax, depth);
    
    float3 cellMin = bboxMin + (float3)((float)coords.x, (float)coords.y, (float)coords.z) * cellExtent;
    float3 cellMax = cellMin + cellExtent;
    
    // Sample SDF at 8 corners
    float cornerValues[8];
    for (int corner = 0; corner < 8; corner++)
    {
        int cx = (corner >> 0) & 1;
        int cy = (corner >> 1) & 1;
        int cz = (corner >> 2) & 1;
        
        float3 cornerPos = cellMin + (float3)((float)cx, (float)cy, (float)cz) * cellExtent;
        float4 sdfResult = model(cornerPos, PASS_PAYLOAD_ARGS);
        cornerValues[corner] = sdfResult.w - isoValue;
    }
    
    // Edge table
    const int edgeCorners[12][2] = {
        {0,1}, {1,3}, {3,2}, {2,0},  // Bottom face
        {4,5}, {5,7}, {7,6}, {6,4},  // Top face
        {0,4}, {1,5}, {3,7}, {2,6}   // Vertical edges
    };
    
    // Collect all edge intersections and their normals
    float3 intersections[12];
    float3 normals[12];
    int intersectionCount = 0;
    
    for (int e = 0; e < 12; e++)
    {
        if ((node.edgeMask & (1 << e)) == 0) continue;
        
        int c0 = edgeCorners[e][0];
        int c1 = edgeCorners[e][1];
        
        int cx0 = (c0 >> 0) & 1, cy0 = (c0 >> 1) & 1, cz0 = (c0 >> 2) & 1;
        int cx1 = (c1 >> 0) & 1, cy1 = (c1 >> 1) & 1, cz1 = (c1 >> 2) & 1;
        
        float3 p0 = cellMin + (float3)((float)cx0, (float)cy0, (float)cz0) * cellExtent;
        float3 p1 = cellMin + (float3)((float)cx1, (float)cy1, (float)cz1) * cellExtent;
        
        float3 intersection = findEdgeIntersection(p0, p1, cornerValues[c0], cornerValues[c1]);
        intersections[intersectionCount] = intersection;
        normals[intersectionCount] = computeGradient(intersection, PASS_PAYLOAD_ARGS);
        intersectionCount++;
    }
    
    // Solve QEF using SVD to find optimal vertex position
    QefResult qef = solveQefSvd(intersections, normals, intersectionCount, cellMin, cellMax);
    
    // Output vertex
    Vertex v;
    v.position = (float4)(qef.position.x, qef.position.y, qef.position.z, 1.0f);
    v.normal = (float4)(qef.normal.x, qef.normal.y, qef.normal.z, 0.0f);
    
    outputVertices[vertexIndex] = v;
}

// Octree construction kernel
// Build octree by subdividing cells that contain the surface
__kernel void construct_octree_level(
    __global OctreeNode* inputNodes,
    __global OctreeNode* outputNodes,
    __global int* outputCount,
    const int numInputNodes,
    const float3 bboxMin,
    const float3 bboxMax,
    const uint currentDepth,
    const uint maxDepth,
    const uint initialDepth,
    PAYLOAD_ARGS,
    const float isoValue)
{
    int id = get_global_id(0);
    if (id >= numInputNodes) return;
    
    OctreeNode parent = inputNodes[id];
    
    // Don't subdivide if we're at max depth
    if (currentDepth >= maxDepth) return;
    
    // Get parent cell bounds
    ulong3 parentCoords = decodeMorton3(parent.mortonCode);
    float3 cellExtent = getCellExtent(bboxMin, bboxMax, currentDepth);
    float3 parentMin = bboxMin + (float3)((float)parentCoords.x, (float)parentCoords.y, (float)parentCoords.z) * cellExtent;
    
    // Subdivide into 8 children
    float3 childExtent = cellExtent * 0.5f;
    
    for (int childIdx = 0; childIdx < 8; childIdx++) {
        // Compute child offset (0-7 maps to 3D grid)
        int dx = (childIdx >> 0) & 1;
        int dy = (childIdx >> 1) & 1;
        int dz = (childIdx >> 2) & 1;
        
        ulong childX = (parentCoords.x << 1) | dx;
        ulong childY = (parentCoords.y << 1) | dy;
        ulong childZ = (parentCoords.z << 1) | dz;
        
        ulong childMorton = encodeMorton3(childX, childY, childZ);
        
        float3 childMin = parentMin + (float3)((float)dx, (float)dy, (float)dz) * childExtent;
        float3 childMax = childMin + childExtent;
        
        // Sample SDF at 8 corners of child cell using model evaluation
        float cornerValues[8];
        uint signMask = 0;
        
        for (int corner = 0; corner < 8; corner++) {
            int cx = (corner >> 0) & 1;
            int cy = (corner >> 1) & 1;
            int cz = (corner >> 2) & 1;
            
            float3 cornerPos = childMin + (float3)((float)cx, (float)cy, (float)cz) * childExtent;
            
            // Evaluate SDF using the model function
            float4 sdfResult = model(cornerPos, PASS_PAYLOAD_ARGS);
            float sdfValue = sdfResult.w - isoValue;
            cornerValues[corner] = sdfValue;
            
            if (sdfValue < 0.0f) {
                signMask |= (1 << corner);
            }
        }
        
        bool const containsSurface = (signMask != 0) && (signMask != 0xFF);
        bool const forceSubdivision = currentDepth < initialDepth;

        // Check if child contains surface (sign changes) or needs forced subdivision
        if (forceSubdivision || containsSurface) {
            // Child contains surface or is part of forced refinement, add to output
            int outputIdx = atomic_inc(outputCount);
            
            OctreeNode child;
            child.mortonCode = childMorton;
            child.edgeMask = 0; // Will be computed below
            child.internalMask = signMask; // Store which corners are inside (negative SDF)
            child.depth = (uchar)min((uint)255, currentDepth + 1U);
            for (int p = 0; p < 7; p++) child.padding[p] = 0;
            
            // Compute edge mask (which of 12 edges cross surface)
            // Edge numbering: 0-3 bottom face (X,Y,X,Y), 4-7 top face, 8-11 vertical
            const int edgeCorners[12][2] = {
                {0,1}, {1,3}, {3,2}, {2,0},  // Bottom face
                {4,5}, {5,7}, {7,6}, {6,4},  // Top face
                {0,4}, {1,5}, {3,7}, {2,6}   // Vertical edges
            };
            
            if (containsSurface) {
                for (int e = 0; e < 12; e++) {
                    int c0 = edgeCorners[e][0];
                    int c1 = edgeCorners[e][1];
                    
                    // Check for sign change
                    if ((cornerValues[c0] < 0.0f) != (cornerValues[c1] < 0.0f)) {
                        child.edgeMask |= (1 << e);
                    }
                }
            }
            
            outputNodes[outputIdx] = child;
        }
    }
}

// ============================================================================
// GPU-Based Watertight Index Generation
// ============================================================================
//
// Strategy for watertightness:
// 1. Each edge with a sign change generates exactly one quad (2 triangles)
// 2. A quad is formed by 4 cells sharing that edge
// 3. To avoid duplicates, only the cell with the smallest Morton code among
//    the 4 neighbors "owns" the edge and emits the quad
// 4. Cells are sorted by Morton code, enabling binary search for neighbors
//
// For uniform grids (all cells at same depth):
// - Edge 0 (X-axis at min Y, min Z) shared by: (x,y,z), (x,y-1,z), (x,y,z-1), (x,y-1,z-1)
// - Edge 3 (Y-axis at min X, min Z) shared by: (x,y,z), (x-1,y,z), (x,y,z-1), (x-1,y,z-1)
// - Edge 8 (Z-axis at min X, min Y) shared by: (x,y,z), (x-1,y,z), (x,y-1,z), (x-1,y-1,z)
// ============================================================================

/// Binary search to find a node by Morton code (for uniform-depth octrees)
/// Returns the index of the node, or -1 if not found
int findNodeByMorton(__global const OctreeNode* nodes, int numNodes, ulong targetMorton)
{
    int left = 0;
    int right = numNodes - 1;
    
    while (left <= right)
    {
        int mid = (left + right) / 2;
        ulong midMorton = nodes[mid].mortonCode;
        
        if (midMorton == targetMorton)
        {
            return mid;
        }
        else if (midMorton < targetMorton)
        {
            left = mid + 1;
        }
        else
        {
            right = mid - 1;
        }
    }
    
    return -1;
}

/// Count quads per cell (first pass for prefix sum)
/// We use edges at the MAX corner (6, 5, 10) because for these edges,
/// the current cell has the smallest Morton code among the 4 cells sharing it.
/// This is because the neighbors are at +Y, +Z, or both, which have larger Morton codes.
///
/// Edge ownership rule: emit a quad if:
/// 1. The edge crosses the surface (edgeMask bit set)
/// 2. All 4 cells around this edge exist (have surface)
__kernel void count_quads(
    __global const OctreeNode* nodes,
    __global int* quadCounts,
    const int numNodes,
    const uint maxCoord)  // Maximum valid coordinate (2^depth - 1)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;
    
    OctreeNode node = nodes[id];
    
    // Skip cells without surface
    if (node.edgeMask == 0)
    {
        quadCounts[id] = 0;
        return;
    }
    
    ulong3 coords = decodeMorton3(node.mortonCode);
    
    int count = 0;
    
    // Edge 6: X-axis at (y=max, z=max), corners 7-6: (1,1,1)-(0,1,1)
    // Shared by: (x,y,z), (x,y+1,z), (x,y,z+1), (x,y+1,z+1)
    // Current cell has smallest Morton code since neighbors have larger y and/or z
    if (node.edgeMask & (1 << 6))
    {
        // Check if neighbors exist within grid bounds
        if (coords.y < maxCoord && coords.z < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x, coords.y + 1, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            // All 3 neighbors must exist (contain surface)
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                count++;
            }
        }
    }
    
    // Edge 5: Y-axis at (x=max, z=max), corners 5-7: (1,0,1)-(1,1,1)
    // Shared by: (x,y,z), (x+1,y,z), (x,y,z+1), (x+1,y,z+1)
    if (node.edgeMask & (1 << 5))
    {
        if (coords.x < maxCoord && coords.z < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                count++;
            }
        }
    }
    
    // Edge 10: Z-axis at (x=max, y=max), corners 3-7: (1,1,0)-(1,1,1)
    // Shared by: (x,y,z), (x+1,y,z), (x,y+1,z), (x+1,y+1,z)
    if (node.edgeMask & (1 << 10))
    {
        if (coords.x < maxCoord && coords.y < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y + 1, coords.z);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                count++;
            }
        }
    }
    
    // Each quad = 2 triangles = 6 indices
    quadCounts[id] = count * 6;
}

/// Emit indices for quads (second pass after prefix sum)
/// Must use the same edges as count_quads (6, 5, 10)
/// Winding order is determined by the sign of corner 7 (the max corner)
__kernel void emit_indices(
    __global OctreeNode const* nodes,
    __global int const* vertexOffsets,
    __global int const* indexOffsets,
    __global uint* outputIndices,
    const int numNodes,
    const uint maxCoord)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;

    OctreeNode node = nodes[id];
    
    // Skip cells without surface
    if (node.edgeMask == 0) return;
    
    ulong3 coords = decodeMorton3(node.mortonCode);
    
    int writeOffset = indexOffsets[id];
    
    // Corner 7 is at (1,1,1), bit 7 in internalMask
    bool corner7Inside = (node.internalMask & (1 << 7)) != 0;
    
    // Get vertex index for a cell (1 vertex per cell)
    #define GET_VERTEX(nodeIdx) ((uint)vertexOffsets[nodeIdx])
    
    // Edge 6: X-axis at (y=max, z=max), corners 7-6: (1,1,1)-(0,1,1)
    // Shared by: (x,y,z), (x,y+1,z), (x,y,z+1), (x,y+1,z+1)
    if (node.edgeMask & (1 << 6))
    {
        if (coords.y < maxCoord && coords.z < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x, coords.y + 1, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                uint v0 = GET_VERTEX(id);
                uint v1 = GET_VERTEX(nIdx1);
                uint v2 = GET_VERTEX(nIdx2);
                uint v3 = GET_VERTEX(nIdx3);
                
                // Emit 2 triangles for quad
                // Winding depends on whether corner 7 is inside
                if (corner7Inside)
                {
                    // Corner 7 inside: wind CCW when viewed from +X
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v2;
                    outputIndices[writeOffset + 2] = v1;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v2;
                    outputIndices[writeOffset + 5] = v3;
                }
                else
                {
                    // Corner 7 outside: wind CW when viewed from +X
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v1;
                    outputIndices[writeOffset + 2] = v2;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v3;
                    outputIndices[writeOffset + 5] = v2;
                }
                
                writeOffset += 6;
            }
        }
    }
    
    // Edge 5: Y-axis at (x=max, z=max), corners 5-7: (1,0,1)-(1,1,1)
    // Shared by: (x,y,z), (x+1,y,z), (x,y,z+1), (x+1,y,z+1)
    if (node.edgeMask & (1 << 5))
    {
        if (coords.x < maxCoord && coords.z < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                uint v0 = GET_VERTEX(id);
                uint v1 = GET_VERTEX(nIdx1);
                uint v2 = GET_VERTEX(nIdx2);
                uint v3 = GET_VERTEX(nIdx3);
                
                // INVERTED logic compared to edges 6 and 10
                if (corner7Inside)
                {
                    // Need -Y: base winding gives -Y, so NO swap
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v1;
                    outputIndices[writeOffset + 2] = v2;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v3;
                    outputIndices[writeOffset + 5] = v2;
                }
                else
                {
                    // Need +Y: swap to get +Y
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v2;
                    outputIndices[writeOffset + 2] = v1;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v2;
                    outputIndices[writeOffset + 5] = v3;
                }
                
                writeOffset += 6;
            }
        }
    }
    
    // Edge 10: Z-axis at (x=max, y=max), corners 3-7: (1,1,0)-(1,1,1)
    // Shared by: (x,y,z), (x+1,y,z), (x,y+1,z), (x+1,y+1,z)
    if (node.edgeMask & (1 << 10))
    {
        if (coords.x < maxCoord && coords.y < maxCoord)
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y + 1, coords.z);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                uint v0 = GET_VERTEX(id);
                uint v1 = GET_VERTEX(nIdx1);
                uint v2 = GET_VERTEX(nIdx2);
                uint v3 = GET_VERTEX(nIdx3);
                
                if (corner7Inside)
                {
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v2;
                    outputIndices[writeOffset + 2] = v1;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v2;
                    outputIndices[writeOffset + 5] = v3;
                }
                else
                {
                    outputIndices[writeOffset + 0] = v0;
                    outputIndices[writeOffset + 1] = v1;
                    outputIndices[writeOffset + 2] = v2;
                    
                    outputIndices[writeOffset + 3] = v1;
                    outputIndices[writeOffset + 4] = v3;
                    outputIndices[writeOffset + 5] = v2;
                }
                
                writeOffset += 6;
            }
        }
    }
    
    #undef GET_VERTEX
}
