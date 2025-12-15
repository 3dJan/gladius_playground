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

// Forward declaration for gradient computation
float3 computeGradientWithEps(float3 pos, float epsilon, PAYLOAD_ARGS);

// Forward declaration for edge intersection
float3 findEdgeIntersection(float3 p0, float3 p1, float v0, float v1);

// ============================================================================
// Gradient Discontinuity Detection for CSG Operations
// ============================================================================
// When implicit functions use min/max operations (CSG union/intersection),
// gradients become discontinuous at the boundary where the operator switches.
// This causes holes because DC generates one vertex but the surface has
// multiple distinct components meeting at that point.
//
// Detection strategy: Cluster normals by direction. If we find clusters
// pointing in incompatible directions (dot product < threshold), we have
// a discontinuity requiring multiple vertices per cell.

/// Result of discontinuity analysis
typedef struct
{
    int componentCount;      ///< Number of distinct surface components (1-4)
    int componentIndices[12]; ///< Which component each sample belongs to (0-3)
    float discontinuityScore; ///< 0 = smooth, 1 = severe discontinuity
} DiscontinuityResult;

/// Analyze edge samples to detect gradient discontinuities from CSG operations.
/// Uses greedy clustering: assign each normal to nearest existing cluster,
/// or create new cluster if angle exceeds threshold.
///
/// @param normals Array of gradient normals at edge crossings
/// @param count Number of edge crossings
/// @param angleThreshold Cosine threshold for clustering (e.g., 0.3 = ~72°)
/// @return Analysis result with component count and assignments
DiscontinuityResult detectGradientDiscontinuity(
    float3 const* normals,
    int count,
    float angleThreshold)
{
    DiscontinuityResult result;
    result.componentCount = 0;
    result.discontinuityScore = 0.0f;
    
    if (count == 0)
    {
        return result;
    }
    
    // Cluster centroids (normalized)
    float3 clusterNormals[4];
    int clusterSizes[4] = {0, 0, 0, 0};
    
    // Process each normal
    for (int i = 0; i < count; i++)
    {
        float3 n = normals[i];
        float nLen = length(n);
        if (nLen < 1e-6f)
        {
            // Degenerate normal - assign to cluster 0
            result.componentIndices[i] = 0;
            continue;
        }
        n = n / nLen; // Normalize
        
        // Find best matching cluster
        int bestCluster = -1;
        float bestDot = angleThreshold; // Must exceed threshold to join
        
        for (int c = 0; c < result.componentCount; c++)
        {
            float d = dot(n, clusterNormals[c]);
            if (d > bestDot)
            {
                bestDot = d;
                bestCluster = c;
            }
        }
        
        if (bestCluster >= 0)
        {
            // Add to existing cluster - update centroid
            result.componentIndices[i] = bestCluster;
            clusterSizes[bestCluster]++;
            // Running average for centroid
            float3 newCentroid = clusterNormals[bestCluster] * (float)(clusterSizes[bestCluster] - 1) + n;
            float centroidLen = length(newCentroid);
            if (centroidLen > 1e-6f)
            {
                clusterNormals[bestCluster] = newCentroid / centroidLen;
            }
        }
        else if (result.componentCount < 4)
        {
            // Create new cluster
            int newCluster = result.componentCount;
            result.componentCount++;
            clusterNormals[newCluster] = n;
            clusterSizes[newCluster] = 1;
            result.componentIndices[i] = newCluster;
        }
        else
        {
            // Max clusters reached - assign to closest
            bestDot = -2.0f;
            for (int c = 0; c < 4; c++)
            {
                float d = dot(n, clusterNormals[c]);
                if (d > bestDot)
                {
                    bestDot = d;
                    bestCluster = c;
                }
            }
            result.componentIndices[i] = bestCluster;
        }
    }
    
    // Ensure at least 1 component
    if (result.componentCount == 0)
    {
        result.componentCount = 1;
    }
    
    // Compute discontinuity score based on cluster separation
    if (result.componentCount > 1)
    {
        float minDot = 1.0f;
        for (int i = 0; i < result.componentCount; i++)
        {
            for (int j = i + 1; j < result.componentCount; j++)
            {
                float d = dot(clusterNormals[i], clusterNormals[j]);
                minDot = fmin(minDot, d);
            }
        }
        // Score: 0 if parallel, 1 if opposite
        result.discontinuityScore = (1.0f - minDot) * 0.5f;
    }
    
    return result;
}

/// Detect gradient discontinuities and count vertices needed per cell.
/// Cells with CSG discontinuities need multiple vertices (one per component).
/// Also stores componentCount in node.padding[1] for later use.
__kernel void count_vertices(
    __global OctreeNode* nodes,
    __global int* countBuffer,
    const int numNodes,
    const float3 bboxMin,
    const float3 bboxMax,
    PAYLOAD_ARGS,
    const float isoValue,
    const float gradientEpsilon)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;
    
    OctreeNode node = nodes[id];
    
    // Halo nodes always get 1 vertex (for boundary stitching)
    if (node.padding[0] == 1)
    {
        countBuffer[id] = 1;
        nodes[id].padding[1] = 1; // componentCount = 1
        return;
    }
    
    // No surface crossing -> no vertex
    if (node.edgeMask == 0)
    {
        countBuffer[id] = 0;
        nodes[id].padding[1] = 0;
        return;
    }
    
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
    
    // Collect edge intersection normals
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
        normals[intersectionCount] = computeGradientWithEps(intersection, gradientEpsilon, PASS_PAYLOAD_ARGS);
        intersectionCount++;
    }
    
    // Detect gradient discontinuities - cluster normals by direction
    // Threshold 0.3 corresponds to ~72° angle between normals
    DiscontinuityResult discResult = detectGradientDiscontinuity(normals, intersectionCount, 0.3f);
    
    // Store component count in padding[1] for use by emit_vertices
    int componentCount = discResult.componentCount;
    if (componentCount < 1) componentCount = 1;
    if (componentCount > 4) componentCount = 4;
    
    nodes[id].padding[1] = (uchar)componentCount;
    countBuffer[id] = componentCount;
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
float3 computeGradientWithEps(float3 pos, float h, PAYLOAD_ARGS)
{
    // Central difference gradient using model evaluation
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

/// Compute gradient with default epsilon (for backward compatibility)
float3 computeGradient(float3 pos, PAYLOAD_ARGS)
{
    return computeGradientWithEps(pos, 0.01f, PASS_PAYLOAD_ARGS);
}

/// Generate one vertex per cell using SVD-based QEF solver for sharp edge preservation
/// For multi-component cells, generates separate vertices per component.
/// Also stores edge→component mapping in padding[2-4] for edges 5, 6, 10.
__kernel void emit_vertices(
    __global OctreeNode* nodes,
    __global int const* offsets,
    __global Vertex* outputVertices,
    const int numNodes,
    const float3 bboxMin,
    const float3 bboxMax,
    PAYLOAD_ARGS,
    const float isoValue,
    const float gradientEpsilon)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;

    OctreeNode node = nodes[id];
    int vertexIndex = offsets[id];
    
    // Get cell bounds (needed for both surface and halo nodes)
    ulong3 coords = decodeMorton3(node.mortonCode);
    uint depth = node.depth;
    float3 cellExtent = getCellExtent(bboxMin, bboxMax, depth);
    float3 cellMin = bboxMin + (float3)((float)coords.x, (float)coords.y, (float)coords.z) * cellExtent;
    float3 cellMax = cellMin + cellExtent;
    float3 cellCenter = (cellMin + cellMax) * 0.5f;
    
    // Handle halo nodes: project cell center onto the nearest surface point
    // Halo nodes are marked with padding[0] == 1
    if (node.padding[0] == 1)
    {
        // Start at cell center
        float3 pos = cellCenter;
        
        // Sample SDF at cell center
        float4 sdfResult = model(pos, PASS_PAYLOAD_ARGS);
        float dist = sdfResult.w - isoValue;
        
        // Gradient-descent projection onto surface (sphere tracing style)
        for (int iter = 0; iter < 16; iter++)
        {
            if (fabs(dist) < 1e-6f) break;  // Close enough to surface
            
            float3 gradient = computeGradientWithEps(pos, gradientEpsilon, PASS_PAYLOAD_ARGS);
            float gradLen = length(gradient);
            if (gradLen < 1e-8f) break;
            
            gradient = gradient / gradLen;
            
            // Move towards surface by the SDF distance (sphere tracing)
            pos = pos - gradient * dist;
            
            // Re-sample
            sdfResult = model(pos, PASS_PAYLOAD_ARGS);
            dist = sdfResult.w - isoValue;
        }
        
        // Use final gradient for normal
        float3 gradient = computeGradientWithEps(pos, gradientEpsilon, PASS_PAYLOAD_ARGS);
        float gradLen = length(gradient);
        if (gradLen > 1e-6f)
        {
            gradient = gradient / gradLen;
        }
        else
        {
            gradient = (float3)(0.0f, 0.0f, 1.0f);
        }
        
        Vertex v;
        v.position = (float4)(pos.x, pos.y, pos.z, 1.0f);
        v.normal = (float4)(gradient.x, gradient.y, gradient.z, 0.0f);
        outputVertices[vertexIndex] = v;
        return;
    }
    
    // Skip cells with no surface intersection (non-halo nodes without surface)
    if (node.edgeMask == 0) return;
    
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
    int edgeIndices[12]; // Track which original edge each sample came from
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
        normals[intersectionCount] = computeGradientWithEps(intersection, gradientEpsilon, PASS_PAYLOAD_ARGS);
        edgeIndices[intersectionCount] = e;
        intersectionCount++;
    }
    
    // Get component count from padding[1] (set by count_vertices)
    int componentCount = (int)node.padding[1];
    if (componentCount < 1) componentCount = 1;
    if (componentCount > 4) componentCount = 4;
    
    if (componentCount == 1)
    {
        // Single component: standard QEF solve with all samples
        QefResult qef = solveQefSvd(intersections, normals, intersectionCount, cellMin, cellMax);
        
        Vertex v;
        v.position = (float4)(qef.position.x, qef.position.y, qef.position.z, 1.0f);
        v.normal = (float4)(qef.normal.x, qef.normal.y, qef.normal.z, 0.0f);
        outputVertices[vertexIndex] = v;
    }
    else
    {
        // Multiple components: detect discontinuities and solve separate QEF per component
        DiscontinuityResult discResult = detectGradientDiscontinuity(normals, intersectionCount, 0.3f);
        
        // For each component, collect its samples and solve QEF
        for (int comp = 0; comp < componentCount; comp++)
        {
            float3 compIntersections[12];
            float3 compNormals[12];
            int compCount = 0;
            
            // Collect samples belonging to this component
            for (int i = 0; i < intersectionCount; i++)
            {
                if (discResult.componentIndices[i] == comp)
                {
                    compIntersections[compCount] = intersections[i];
                    compNormals[compCount] = normals[i];
                    compCount++;
                }
            }
            
            // If this component has no samples, use mass point of all samples
            // (this can happen if componentCount from count_vertices doesn't match exactly)
            if (compCount == 0)
            {
                // Fall back to mass point
                float3 massPoint = (float3)(0.0f, 0.0f, 0.0f);
                for (int i = 0; i < intersectionCount; i++)
                {
                    massPoint += intersections[i];
                }
                if (intersectionCount > 0)
                {
                    massPoint /= (float)intersectionCount;
                }
                else
                {
                    massPoint = cellCenter;
                }
                
                Vertex v;
                v.position = (float4)(massPoint.x, massPoint.y, massPoint.z, 1.0f);
                v.normal = (float4)(0.0f, 1.0f, 0.0f, 0.0f);
                outputVertices[vertexIndex + comp] = v;
            }
            else
            {
                // Solve QEF for this component's samples
                QefResult qef = solveQefSvd(compIntersections, compNormals, compCount, cellMin, cellMax);
                
                Vertex v;
                v.position = (float4)(qef.position.x, qef.position.y, qef.position.z, 1.0f);
                v.normal = (float4)(qef.normal.x, qef.normal.y, qef.normal.z, 0.0f);
                outputVertices[vertexIndex + comp] = v;
            }
        }
        
        // Store edge→component mapping for edges 5, 6, 10 (used by emit_indices)
        // Default to component 0 for edges not in the sample list
        uchar edge5Comp = 0, edge6Comp = 0, edge10Comp = 0;
        for (int i = 0; i < intersectionCount; i++)
        {
            int edgeIdx = edgeIndices[i];
            uchar comp = (uchar)discResult.componentIndices[i];
            if (edgeIdx == 5) edge5Comp = comp;
            else if (edgeIdx == 6) edge6Comp = comp;
            else if (edgeIdx == 10) edge10Comp = comp;
        }
        nodes[id].padding[2] = edge5Comp;
        nodes[id].padding[3] = edge6Comp;
        nodes[id].padding[4] = edge10Comp;
    }
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
// Halo Cell Generation for Watertight Meshes
// ============================================================================
//
// To ensure quads can always be formed, we need neighbor cells to exist for
// every surface-crossing cell. This kernel identifies missing neighbors and
// creates "halo nodes" for them (with edgeMask=0 since they don't have surface).
//
// For quad emission, we use edges 6, 5, 10 which require neighbors at:
// - Edge 6: (x, y+1, z), (x, y, z+1), (x, y+1, z+1)
// - Edge 5: (x+1, y, z), (x, y, z+1), (x+1, y, z+1)
// - Edge 10: (x+1, y, z), (x, y+1, z), (x+1, y+1, z)
//
// Combined unique neighbors needed: +X, +Y, +Z, +XY, +XZ, +YZ, +XYZ (7 neighbors)
// ============================================================================

/// Binary search to find a node by Morton code (for uniform-depth octrees)
/// Returns the index of the node, or -1 if not found
/// Note: This function is defined here for use by halo kernels.
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

/// Get the 4 cells that share a specific edge
/// Returns array of coordinate offsets relative to the given cell
/// Edge numbering (matching the edgeMask bits):
/// 0: X at y=0,z=0 | 1: Y at x=1,z=0 | 2: X at y=1,z=0 | 3: Y at x=0,z=0
/// 4: X at y=0,z=1 | 5: Y at x=1,z=1 | 6: X at y=1,z=1 | 7: Y at x=0,z=1
/// 8: Z at x=0,y=0 | 9: Z at x=1,y=0 | 10: Z at x=1,y=1 | 11: Z at x=0,y=1
inline void getEdgeNeighbors(int edge, __private long out_dx[4], __private long out_dy[4], __private long out_dz[4])
{
    // Each edge is shared by exactly 4 cells
    // Store offsets in the output arrays
    switch(edge)
    {
        case 0: // X-axis at min Y, min Z
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = 0; out_dy[1] = -1; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = 0; out_dz[2] = -1;
            out_dx[3] = 0; out_dy[3] = -1; out_dz[3] = -1;
            break;
        case 1: // Y-axis at max X, min Z
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = 1; out_dy[1] = 0; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = 0; out_dz[2] = -1;
            out_dx[3] = 1; out_dy[3] = 0; out_dz[3] = -1;
            break;
        case 2: // X-axis at max Y, min Z
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = 0; out_dy[1] = 1; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = 0; out_dz[2] = -1;
            out_dx[3] = 0; out_dy[3] = 1; out_dz[3] = -1;
            break;
        case 3: // Y-axis at min X, min Z
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = -1; out_dy[1] = 0; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = 0; out_dz[2] = -1;
            out_dx[3] = -1; out_dy[3] = 0; out_dz[3] = -1;
            break;
        case 4: // X-axis at min Y, max Z
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = 0; out_dy[1] = -1; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = 0; out_dz[2] = 1;
            out_dx[3] = 0; out_dy[3] = -1; out_dz[3] = 1;
            break;
        case 5: // Y-axis at max X, max Z
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = 1; out_dy[1] = 0; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = 0; out_dz[2] = 1;
            out_dx[3] = 1; out_dy[3] = 0; out_dz[3] = 1;
            break;
        case 6: // X-axis at max Y, max Z
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = 0; out_dy[1] = 1; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = 0; out_dz[2] = 1;
            out_dx[3] = 0; out_dy[3] = 1; out_dz[3] = 1;
            break;
        case 7: // Y-axis at min X, max Z
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = -1; out_dy[1] = 0; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = 0; out_dz[2] = 1;
            out_dx[3] = -1; out_dy[3] = 0; out_dz[3] = 1;
            break;
        case 8: // Z-axis at min X, min Y
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = -1; out_dy[1] = 0; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = -1; out_dz[2] = 0;
            out_dx[3] = -1; out_dy[3] = -1; out_dz[3] = 0;
            break;
        case 9: // Z-axis at max X, min Y
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = 1; out_dy[1] = 0; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = -1; out_dz[2] = 0;
            out_dx[3] = 1; out_dy[3] = -1; out_dz[3] = 0;
            break;
        case 10: // Z-axis at max X, max Y
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = 1; out_dy[1] = 0; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = 1; out_dz[2] = 0;
            out_dx[3] = 1; out_dy[3] = 1; out_dz[3] = 0;
            break;
        case 11: // Z-axis at min X, max Y
            out_dx[0] = 0; out_dy[0] = 0; out_dz[0] = 0;
            out_dx[1] = -1; out_dy[1] = 0; out_dz[1] = 0;
            out_dx[2] = 0; out_dy[2] = 1; out_dz[2] = 0;
            out_dx[3] = -1; out_dy[3] = 1; out_dz[3] = 0;
            break;
        default:
            // Invalid edge - return zeros
            for (int i = 0; i < 4; i++)
            {
                out_dx[i] = 0;
                out_dy[i] = 0;
                out_dz[i] = 0;
            }
            break;
    }
}

/// Count missing halo neighbors for each surface cell
/// Strategy: For each edge with a surface crossing, ensure all 4 cells sharing
/// that edge exist. This prevents holes when forming quads.
/// This is more precise than the previous 26-neighborhood approach.
__kernel void count_halo_neighbors(
    __global const OctreeNode* nodes,
    __global int* haloCounts,
    const int numNodes,
    const uint maxCoord,
    const uchar targetDepth)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;
    
    OctreeNode node = nodes[id];
    
    // Only process cells with surface
    if (node.edgeMask == 0)
    {
        haloCounts[id] = 0;
        return;
    }
    
    ulong3 coords = decodeMorton3(node.mortonCode);
    int count = 0;
    
    // For each edge with a surface crossing, check if all 4 sharing cells exist
    for (int edge = 0; edge < 12; edge++)
    {
        // Skip edges without surface crossing
        if (!(node.edgeMask & (1u << edge)))
            continue;
        
        // Get the 4 cells that share this edge
        long dx[4], dy[4], dz[4];
        getEdgeNeighbors(edge, dx, dy, dz);
        
        for (int i = 0; i < 4; i++)
        {
            // Skip self (always at index 0 with offset 0,0,0)
            if (dx[i] == 0 && dy[i] == 0 && dz[i] == 0)
                continue;
            
            // Check bounds
            long nx = (long)coords.x + dx[i];
            long ny = (long)coords.y + dy[i];
            long nz = (long)coords.z + dz[i];
            
            if (nx < 0 || ny < 0 || nz < 0)
                continue;
            if (nx > maxCoord || ny > maxCoord || nz > maxCoord)
                continue;
            
            // Check if neighbor exists
            ulong nMorton = encodeMorton3((ulong)nx, (ulong)ny, (ulong)nz);
            if (findNodeByMorton(nodes, numNodes, nMorton) < 0)
                count++;
        }
    }
    
    haloCounts[id] = count;
}

/// Emit halo nodes for missing edge neighbors
/// For each edge with a surface crossing, ensure all 4 sharing cells exist.
/// Halo nodes are created with edgeMask=0 so they don't try to emit quads themselves.
/// This prevents the cascading halo problem.
__kernel void emit_halo_neighbors(
    __global const OctreeNode* nodes,
    __global const int* haloOffsets,
    __global OctreeNode* haloNodes,
    const int numNodes,
    const uint maxCoord,
    const uchar targetDepth)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;
    
    OctreeNode node = nodes[id];
    
    // Only process cells with surface
    if (node.edgeMask == 0) return;
    
    ulong3 coords = decodeMorton3(node.mortonCode);
    int writeIdx = haloOffsets[id];
    
    // For each edge with a surface crossing, check if all 4 sharing cells exist
    for (int edge = 0; edge < 12; edge++)
    {
        // Skip edges without surface crossing
        if (!(node.edgeMask & (1u << edge)))
            continue;
        
        // Get the 4 cells that share this edge
        long dx[4], dy[4], dz[4];
        getEdgeNeighbors(edge, dx, dy, dz);
        
        for (int i = 0; i < 4; i++)
        {
            // Skip self (always at index 0)
            if (dx[i] == 0 && dy[i] == 0 && dz[i] == 0)
                continue;
            
            // Check bounds
            long nx = (long)coords.x + dx[i];
            long ny = (long)coords.y + dy[i];
            long nz = (long)coords.z + dz[i];
            
            if (nx < 0 || ny < 0 || nz < 0)
                continue;
            if (nx > maxCoord || ny > maxCoord || nz > maxCoord)
                continue;
            
            // Check if neighbor exists
            ulong nMorton = encodeMorton3((ulong)nx, (ulong)ny, (ulong)nz);
            if (findNodeByMorton(nodes, numNodes, nMorton) < 0)
            {
                // Create halo node WITHOUT edgeMask
                // This prevents halos from trying to emit quads (solving cascading problem)
                OctreeNode halo;
                halo.mortonCode = nMorton;
                halo.edgeMask = 0;  // CRITICAL: Halos don't emit quads
                halo.internalMask = 0;
                halo.depth = targetDepth;
                halo.padding[0] = 1; // Mark as halo node
                for (int p = 1; p < 7; p++) halo.padding[p] = 0;
                haloNodes[writeIdx++] = halo;
            }
        }
    }
}

/// Recompute edge masks for halo nodes.
///
/// Halo nodes are initially created without edgeMask/internalMask so they don't emit geometry.
/// However, to solve the "cascading halo problem" and to allow missing surface cells to become
/// real participants, we need to evaluate the SDF for halos and compute their edgeMask.
///
/// Any halo that actually contains the surface (sign changes) is converted into a real surface
/// node by clearing padding[0]. Nodes that remain empty keep padding[0]==1 and edgeMask==0.
__kernel void recompute_halo_edge_masks(
    __global OctreeNode* nodes,
    const int numNodes,
    const float3 bboxMin,
    const float3 bboxMax,
    PAYLOAD_ARGS,
    const float isoValue)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;

    OctreeNode node = nodes[id];
    if (node.padding[0] != 1)
    {
        return;
    }

    // Compute cell bounds
    ulong3 coords = decodeMorton3(node.mortonCode);
    uint depth = node.depth;
    float3 cellExtent = getCellExtent(bboxMin, bboxMax, depth);
    float3 cellMin = bboxMin + (float3)((float)coords.x, (float)coords.y, (float)coords.z) * cellExtent;
    float3 cellMax = cellMin + cellExtent;

    // Sample SDF at 8 corners
    float cornerValues[8];
    uint signMask = 0;
    for (int corner = 0; corner < 8; corner++)
    {
        int cx = (corner >> 0) & 1;
        int cy = (corner >> 1) & 1;
        int cz = (corner >> 2) & 1;

        float3 cornerPos = cellMin + (float3)((float)cx, (float)cy, (float)cz) * cellExtent;
        float4 sdfResult = model(cornerPos, PASS_PAYLOAD_ARGS);
        float sdfValue = sdfResult.w - isoValue;
        cornerValues[corner] = sdfValue;

        if (sdfValue < 0.0f)
        {
            signMask |= (1u << corner);
        }
    }

    bool const containsSurface = (signMask != 0u) && (signMask != 0xFFu);

    // Always store sign information (useful for downstream diagnostics)
    node.internalMask = signMask;
    node.edgeMask = 0;

    if (containsSurface)
    {
        const int edgeCorners[12][2] = {
            {0,1}, {1,3}, {3,2}, {2,0},
            {4,5}, {5,7}, {7,6}, {6,4},
            {0,4}, {1,5}, {3,7}, {2,6}
        };

        for (int e = 0; e < 12; e++)
        {
            int c0 = edgeCorners[e][0];
            int c1 = edgeCorners[e][1];
            if ((cornerValues[c0] < 0.0f) != (cornerValues[c1] < 0.0f))
            {
                node.edgeMask |= (1u << e);
            }
        }

        // Promote to real surface node.
        node.padding[0] = 0;
    }
    else
    {
        // Keep as halo.
        node.padding[0] = 1;
    }

    // Keep remaining padding bytes deterministic.
    for (int p = 1; p < 7; p++) node.padding[p] = 0;

    nodes[id] = node;
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

// Note: findNodeByMorton is defined earlier (in the Halo Cell Generation section)

/// Count quads per cell (first pass for prefix sum)
/// We use edges at the MAX corner (6, 5, 10) because for these edges,
/// the current cell has the smallest Morton code among the 4 cells sharing it.
/// This is because the neighbors are at +Y, +Z, or both, which have larger Morton codes.
///
/// Edge ownership rule: emit a quad if:
/// 1. The edge crosses the surface (edgeMask bit set)
/// 2. All 4 cells around this edge exist (including halo nodes)
/// 3. This cell is the owner (smallest Morton code among the 4)
///
/// For edges at the MIN corner (0, 3, 8), we also check - if the normal owner
/// doesn't exist, we emit the quad ourselves to close the boundary.
///
/// When disableBoundaryChecks is non-zero (chunked mode), the coord checks for
/// min (>0) and max (<maxCoord) are bypassed, allowing edges at chunk boundaries
/// to attempt emission. The findNodeByMorton check still correctly skips edges
/// where neighbor cells don't exist.
__kernel void count_quads(
    __global const OctreeNode* nodes,
    __global int* quadCounts,
    const int numNodes,
    const uint maxCoord,              // Maximum valid coordinate (2^depth - 1)
    const uint disableBoundaryChecks) // Non-zero to disable boundary coord checks
{
    int id = get_global_id(0);
    if (id >= numNodes) return;
    
    OctreeNode node = nodes[id];

    // Halo nodes must never emit quads/triangles. We keep this check explicit
    // (even though halos are typically created with edgeMask=0) to prevent
    // accidental geometry emission if future changes propagate edgeMask bits.
    if (node.padding[0] == 1)
    {
        quadCounts[id] = 0;
        return;
    }
    
    // Skip cells without surface (including halo nodes which have edgeMask=0)
    if (node.edgeMask == 0)
    {
        quadCounts[id] = 0;
        return;
    }
    
    ulong3 coords = decodeMorton3(node.mortonCode);
    
    int count = 0;     // Number of quads (6 indices each)
    int triCount = 0;  // Number of boundary triangles (3 indices each)
    
    // In chunked mode, all boundary checks pass
    bool const atMinBoundary = (disableBoundaryChecks == 0) && 
                               (coords.x == 0 || coords.y == 0 || coords.z == 0);
    bool const atMaxBoundary = (disableBoundaryChecks == 0) &&
                               (coords.x >= maxCoord || coords.y >= maxCoord || coords.z >= maxCoord);
    
    // =========================================================================
    // Edges at MAX corner (this cell is always the owner)
    // =========================================================================
    
    // Edge 6: X-axis at (y=max, z=max), corners 7-6: (1,1,1)-(0,1,1)
    // Shared by: (x,y,z), (x,y+1,z), (x,y,z+1), (x,y+1,z+1)
    // Current cell has smallest Morton code since neighbors have larger y and/or z
    if (node.edgeMask & (1 << 6))
    {
        // Check if neighbors exist within grid bounds (bypassed in chunked mode)
        if (disableBoundaryChecks || (coords.y < maxCoord && coords.z < maxCoord))
        {
            ulong nMorton1 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x, coords.y + 1, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            // All 3 neighbors must exist (surface or halo)
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
        if (disableBoundaryChecks || (coords.x < maxCoord && coords.z < maxCoord))
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
        if (disableBoundaryChecks || (coords.x < maxCoord && coords.y < maxCoord))
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
    
    // =========================================================================
    // Edges at MIN corner - emit if normal owner doesn't exist (boundary case)
    // These emit triangles instead of quads (missing 4th vertex)
    // =========================================================================
    
    // Edge 0: X-axis at (y=min, z=min), corners 0-1
    // Normally owned by cell (x, y-1, z-1) via its edge 6
    // We emit if that cell doesn't exist AND we have 2 neighbors
    if (node.edgeMask & (1 << 0))
    {
        if (disableBoundaryChecks || (coords.y > 0 && coords.z > 0))
        {
            // Check if normal owner exists
            ulong ownerMorton = encodeMorton3(coords.x, coords.y - 1, coords.z - 1);
            int ownerIdx = findNodeByMorton(nodes, numNodes, ownerMorton);
            
            // Only emit if owner doesn't exist (we take over)
            if (ownerIdx < 0)
            {
                ulong n1 = encodeMorton3(coords.x, coords.y - 1, coords.z);
                ulong n2 = encodeMorton3(coords.x, coords.y, coords.z - 1);
                
                int idx1 = findNodeByMorton(nodes, numNodes, n1);
                int idx2 = findNodeByMorton(nodes, numNodes, n2);
                
                // Emit triangle if we have 2 neighbors (3 vertices total)
                if (idx1 >= 0 && idx2 >= 0)
                {
                    triCount++;  // Count as triangle (3 indices) not quad (6 indices)
                }
            }
        }
    }
    
    // Edge 3: Y-axis at (x=min, z=min), corners 0-2
    // Normally owned by cell (x-1, y, z-1) via its edge 5
    if (node.edgeMask & (1 << 3))
    {
        if (disableBoundaryChecks || (coords.x > 0 && coords.z > 0))
        {
            ulong ownerMorton = encodeMorton3(coords.x - 1, coords.y, coords.z - 1);
            int ownerIdx = findNodeByMorton(nodes, numNodes, ownerMorton);
            
            if (ownerIdx < 0)
            {
                ulong n1 = encodeMorton3(coords.x - 1, coords.y, coords.z);
                ulong n2 = encodeMorton3(coords.x, coords.y, coords.z - 1);
                
                int idx1 = findNodeByMorton(nodes, numNodes, n1);
                int idx2 = findNodeByMorton(nodes, numNodes, n2);
                
                if (idx1 >= 0 && idx2 >= 0)
                {
                    triCount++;
                }
            }
        }
    }
    
    // Edge 8: Z-axis at (x=min, y=min), corners 0-4
    // Normally owned by cell (x-1, y-1, z) via its edge 10
    if (node.edgeMask & (1 << 8))
    {
        if (disableBoundaryChecks || (coords.x > 0 && coords.y > 0))
        {
            ulong ownerMorton = encodeMorton3(coords.x - 1, coords.y - 1, coords.z);
            int ownerIdx = findNodeByMorton(nodes, numNodes, ownerMorton);
            
            if (ownerIdx < 0)
            {
                ulong n1 = encodeMorton3(coords.x - 1, coords.y, coords.z);
                ulong n2 = encodeMorton3(coords.x, coords.y - 1, coords.z);
                
                int idx1 = findNodeByMorton(nodes, numNodes, n1);
                int idx2 = findNodeByMorton(nodes, numNodes, n2);
                
                if (idx1 >= 0 && idx2 >= 0)
                {
                    triCount++;
                }
            }
        }
    }
    
    // Quads = 6 indices each, triangles = 3 indices each
    quadCounts[id] = count * 6 + triCount * 3;
}

/// Get the component offset for a specific edge within a cell.
/// For multi-component cells, the component determines which vertex to use.
/// padding[2] = edge 5 component, padding[3] = edge 6 component, padding[4] = edge 10 component
/// 
/// NOTE: Currently disabled - always returns 0 because component indices are local
/// to each cell and don't match across neighbors. This causes mismatched vertices.
/// TODO: Implement global component assignment based on edge normal direction.
int getEdgeComponentOffset(OctreeNode node, int edgeNum)
{
    // DISABLED: Component mapping causes mesh tearing because different cells
    // may assign the same edge to different component indices.
    // Always use component 0 (base vertex) for now.
    return 0;
    
    /*
    // Only edges 5, 6, 10 are processed by emit_indices
    // For single-component cells, always return 0
    if (node.padding[1] <= 1) return 0;
    
    switch (edgeNum)
    {
        case 5:  return (int)node.padding[2];
        case 6:  return (int)node.padding[3];
        case 10: return (int)node.padding[4];
        default: return 0;
    }
    */
}

/// Emit indices for quads (second pass after prefix sum)
/// Must use the same edges as count_quads (6, 5, 10)
/// Winding order is determined by the sign of corner 7 (the max corner)
/// Uses edge→component mapping from padding[2-4] for multi-vertex cells
__kernel void emit_indices(
    __global OctreeNode const* nodes,
    __global int const* vertexOffsets,
    __global int const* indexOffsets,
    __global uint* outputIndices,
    const int numNodes,
    const uint maxCoord,
    const uint disableBoundaryChecks)
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
    
    // Get vertex index for a cell, accounting for edge→component mapping
    // For edge 5, 6, 10, use the stored component offset from padding[2-4]
    // For multi-component cells: vertex = vertexOffsets[nodeIdx] + componentOffset
    #define GET_VERTEX_FOR_EDGE(nodeIdx, edgeNum) \
        ((uint)(vertexOffsets[nodeIdx] + getEdgeComponentOffset(nodes[nodeIdx], edgeNum)))
    
    // Fallback for boundary edges (0, 3, 8) which don't have component mapping
    // These are only used for partial boundary triangles, and always use component 0
    #define GET_VERTEX(nodeIdx) ((uint)vertexOffsets[nodeIdx])
    
    // Edge 6: X-axis at (y=max, z=max), corners 7-6: (1,1,1)-(0,1,1)
    // Shared by: (x,y,z), (x,y+1,z), (x,y,z+1), (x,y+1,z+1)
    if (node.edgeMask & (1 << 6))
    {
        if (disableBoundaryChecks || (coords.y < maxCoord && coords.z < maxCoord))
        {
            ulong nMorton1 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x, coords.y + 1, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                uint v0 = GET_VERTEX_FOR_EDGE(id, 6);
                uint v1 = GET_VERTEX_FOR_EDGE(nIdx1, 6);
                uint v2 = GET_VERTEX_FOR_EDGE(nIdx2, 6);
                uint v3 = GET_VERTEX_FOR_EDGE(nIdx3, 6);
                
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
        if (disableBoundaryChecks || (coords.x < maxCoord && coords.z < maxCoord))
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                uint v0 = GET_VERTEX_FOR_EDGE(id, 5);
                uint v1 = GET_VERTEX_FOR_EDGE(nIdx1, 5);
                uint v2 = GET_VERTEX_FOR_EDGE(nIdx2, 5);
                uint v3 = GET_VERTEX_FOR_EDGE(nIdx3, 5);
                
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
        if (disableBoundaryChecks || (coords.x < maxCoord && coords.y < maxCoord))
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y + 1, coords.z);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                uint v0 = GET_VERTEX_FOR_EDGE(id, 10);
                uint v1 = GET_VERTEX_FOR_EDGE(nIdx1, 10);
                uint v2 = GET_VERTEX_FOR_EDGE(nIdx2, 10);
                uint v3 = GET_VERTEX_FOR_EDGE(nIdx3, 10);
                
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
    
    // =========================================================================
    // Boundary edges at MIN corner - emit TRIANGULAR cap if normal owner doesn't exist
    // These edges have only 3 vertices (owner cell missing), so we emit single triangles
    // =========================================================================
    
    // Edge 0: X-axis at (y=min, z=min)
    // Normally owned by cell (x, y-1, z-1) via its edge 6
    if (node.edgeMask & (1 << 0))
    {
        if (disableBoundaryChecks || (coords.y > 0 && coords.z > 0))
        {
            ulong ownerMorton = encodeMorton3(coords.x, coords.y - 1, coords.z - 1);
            int ownerIdx = findNodeByMorton(nodes, numNodes, ownerMorton);
            
            if (ownerIdx < 0)
            {
                ulong n1 = encodeMorton3(coords.x, coords.y - 1, coords.z);
                ulong n2 = encodeMorton3(coords.x, coords.y, coords.z - 1);
                
                int idx1 = findNodeByMorton(nodes, numNodes, n1);
                int idx2 = findNodeByMorton(nodes, numNodes, n2);
                
                if (idx1 >= 0 && idx2 >= 0)
                {
                    uint v0 = GET_VERTEX(id);
                    uint v1 = GET_VERTEX(idx1);  // y-1
                    uint v2 = GET_VERTEX(idx2);  // z-1
                    
                    // Corner 0 is at (0,0,0), bit 0 in internalMask
                    bool corner0Inside = (node.internalMask & (1 << 0)) != 0;
                    
                    // Emit single triangle with correct winding
                    if (corner0Inside)
                    {
                        outputIndices[writeOffset + 0] = v0;
                        outputIndices[writeOffset + 1] = v1;
                        outputIndices[writeOffset + 2] = v2;
                    }
                    else
                    {
                        outputIndices[writeOffset + 0] = v0;
                        outputIndices[writeOffset + 1] = v2;
                        outputIndices[writeOffset + 2] = v1;
                    }
                    
                    writeOffset += 3;
                }
            }
        }
    }
    
    // Edge 3: Y-axis at (x=min, z=min)
    if (node.edgeMask & (1 << 3))
    {
        if (disableBoundaryChecks || (coords.x > 0 && coords.z > 0))
        {
            ulong ownerMorton = encodeMorton3(coords.x - 1, coords.y, coords.z - 1);
            int ownerIdx = findNodeByMorton(nodes, numNodes, ownerMorton);
            
            if (ownerIdx < 0)
            {
                ulong n1 = encodeMorton3(coords.x - 1, coords.y, coords.z);
                ulong n2 = encodeMorton3(coords.x, coords.y, coords.z - 1);
                
                int idx1 = findNodeByMorton(nodes, numNodes, n1);
                int idx2 = findNodeByMorton(nodes, numNodes, n2);
                
                if (idx1 >= 0 && idx2 >= 0)
                {
                    uint v0 = GET_VERTEX(id);
                    uint v1 = GET_VERTEX(idx1);
                    uint v2 = GET_VERTEX(idx2);
                    
                    bool corner0Inside = (node.internalMask & (1 << 0)) != 0;
                    
                    if (corner0Inside)
                    {
                        outputIndices[writeOffset + 0] = v0;
                        outputIndices[writeOffset + 1] = v1;
                        outputIndices[writeOffset + 2] = v2;
                    }
                    else
                    {
                        outputIndices[writeOffset + 0] = v0;
                        outputIndices[writeOffset + 1] = v2;
                        outputIndices[writeOffset + 2] = v1;
                    }
                    
                    writeOffset += 3;
                }
            }
        }
    }
    
    // Edge 8: Z-axis at (x=min, y=min)
    if (node.edgeMask & (1 << 8))
    {
        if (disableBoundaryChecks || (coords.x > 0 && coords.y > 0))
        {
            ulong ownerMorton = encodeMorton3(coords.x - 1, coords.y - 1, coords.z);
            int ownerIdx = findNodeByMorton(nodes, numNodes, ownerMorton);
            
            if (ownerIdx < 0)
            {
                ulong n1 = encodeMorton3(coords.x - 1, coords.y, coords.z);
                ulong n2 = encodeMorton3(coords.x, coords.y - 1, coords.z);
                
                int idx1 = findNodeByMorton(nodes, numNodes, n1);
                int idx2 = findNodeByMorton(nodes, numNodes, n2);
                
                if (idx1 >= 0 && idx2 >= 0)
                {
                    uint v0 = GET_VERTEX(id);
                    uint v1 = GET_VERTEX(idx1);
                    uint v2 = GET_VERTEX(idx2);
                    
                    bool corner0Inside = (node.internalMask & (1 << 0)) != 0;
                    
                    if (corner0Inside)
                    {
                        outputIndices[writeOffset + 0] = v0;
                        outputIndices[writeOffset + 1] = v1;
                        outputIndices[writeOffset + 2] = v2;
                    }
                    else
                    {
                        outputIndices[writeOffset + 0] = v0;
                        outputIndices[writeOffset + 1] = v2;
                        outputIndices[writeOffset + 2] = v1;
                    }
                    
                    writeOffset += 3;
                }
            }
        }
    }
    
    #undef GET_VERTEX
}

// ============================================================================
// Diagnostic Kernel for Boundary Hole Analysis (Extended)
// ============================================================================
//
// This kernel collects statistics about ALL 12 edges and their emission status.
// It helps identify which edges are responsible for holes.
//
// Diagnostic counters (24 ints):
// For each of 12 edges (0-11), we store 2 counters:
// [edge*2]   = emitted count
// [edge*2+1] = skipped count (either bbox boundary or missing neighbor)
// ============================================================================

__kernel void count_quads_diagnostic(
    __global const OctreeNode* nodes,
    __global int* diagnosticCounters,  // Array of 24 ints
    const int numNodes,
    const uint maxCoord)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;
    
    OctreeNode node = nodes[id];

    // Diagnostics should focus on real surface cells.
    if (node.padding[0] == 1) return;
    
    // Skip cells without surface
    if (node.edgeMask == 0) return;
    
    ulong3 coords = decodeMorton3(node.mortonCode);
    
    // Edge neighbor lookup - each edge is shared by 4 cells
    // For edge at position (ex, ey, ez) axis A:
    // The 4 cells are at offsets in the 2 non-A axes
    
    // Edge 0: X-axis at (y=0, z=0), corners 0-1
    // Shared by: (x,y,z), (x,y-1,z), (x,y,z-1), (x,y-1,z-1)
    // Owned by cell (x,y-1,z-1) via edge 6
    if (node.edgeMask & (1 << 0))
    {
        if (coords.y == 0 || coords.z == 0)
        {
            atomic_inc(&diagnosticCounters[1]);  // at boundary, can't form full quad
        }
        else
        {
            // Check if owner cell exists
            ulong ownerMorton = encodeMorton3(coords.x, coords.y - 1, coords.z - 1);
            if (findNodeByMorton(nodes, numNodes, ownerMorton) >= 0)
            {
                atomic_inc(&diagnosticCounters[0]);  // owner will emit
            }
            else
            {
                // We take over, but check if we can form the quad
                ulong n1 = encodeMorton3(coords.x, coords.y - 1, coords.z);
                ulong n2 = encodeMorton3(coords.x, coords.y, coords.z - 1);
                int idx1 = findNodeByMorton(nodes, numNodes, n1);
                int idx2 = findNodeByMorton(nodes, numNodes, n2);
                if (idx1 >= 0 && idx2 >= 0)
                {
                    atomic_inc(&diagnosticCounters[0]);  // we emit
                }
                else
                {
                    atomic_inc(&diagnosticCounters[1]);  // skipped
                }
            }
        }
    }
    
    // Edge 1: Y-axis at (x=1, z=0), corners 1-3
    // Shared by: (x,y,z), (x+1,y,z), (x,y,z-1), (x+1,y,z-1)
    // Owned by cell (x,y,z-1) via edge 5
    if (node.edgeMask & (1 << 1))
    {
        if (coords.z == 0)
        {
            atomic_inc(&diagnosticCounters[3]);  // at boundary
        }
        else
        {
            ulong ownerMorton = encodeMorton3(coords.x, coords.y, coords.z - 1);
            if (findNodeByMorton(nodes, numNodes, ownerMorton) >= 0)
            {
                atomic_inc(&diagnosticCounters[2]);  // owner will emit
            }
            else
            {
                atomic_inc(&diagnosticCounters[3]);  // skipped - no emission path
            }
        }
    }
    
    // Edge 2: X-axis at (y=1, z=0), corners 2-3
    // Shared by: (x,y,z), (x,y+1,z), (x,y,z-1), (x,y+1,z-1)
    // Owned by cell (x,y,z-1) via edge 6
    if (node.edgeMask & (1 << 2))
    {
        if (coords.z == 0)
        {
            atomic_inc(&diagnosticCounters[5]);  // at boundary
        }
        else
        {
            ulong ownerMorton = encodeMorton3(coords.x, coords.y, coords.z - 1);
            if (findNodeByMorton(nodes, numNodes, ownerMorton) >= 0)
            {
                atomic_inc(&diagnosticCounters[4]);  // owner will emit
            }
            else
            {
                atomic_inc(&diagnosticCounters[5]);  // skipped
            }
        }
    }
    
    // Edge 3: Y-axis at (x=0, z=0), corners 0-2
    // Shared by: (x,y,z), (x-1,y,z), (x,y,z-1), (x-1,y,z-1)
    // Owned by cell (x-1,y,z-1) via edge 5
    if (node.edgeMask & (1 << 3))
    {
        if (coords.x == 0 || coords.z == 0)
        {
            atomic_inc(&diagnosticCounters[7]);  // at boundary
        }
        else
        {
            ulong ownerMorton = encodeMorton3(coords.x - 1, coords.y, coords.z - 1);
            if (findNodeByMorton(nodes, numNodes, ownerMorton) >= 0)
            {
                atomic_inc(&diagnosticCounters[6]);  // owner will emit
            }
            else
            {
                // We take over
                ulong n1 = encodeMorton3(coords.x - 1, coords.y, coords.z);
                ulong n2 = encodeMorton3(coords.x, coords.y, coords.z - 1);
                int idx1 = findNodeByMorton(nodes, numNodes, n1);
                int idx2 = findNodeByMorton(nodes, numNodes, n2);
                if (idx1 >= 0 && idx2 >= 0)
                {
                    atomic_inc(&diagnosticCounters[6]);
                }
                else
                {
                    atomic_inc(&diagnosticCounters[7]);
                }
            }
        }
    }
    
    // Edge 4: X-axis at (y=0, z=1), corners 4-5
    // Shared by: (x,y,z), (x,y-1,z), (x,y,z+1), (x,y-1,z+1)
    // Owned by cell (x,y-1,z) via edge 6
    if (node.edgeMask & (1 << 4))
    {
        if (coords.y == 0)
        {
            atomic_inc(&diagnosticCounters[9]);  // at boundary
        }
        else
        {
            ulong ownerMorton = encodeMorton3(coords.x, coords.y - 1, coords.z);
            if (findNodeByMorton(nodes, numNodes, ownerMorton) >= 0)
            {
                atomic_inc(&diagnosticCounters[8]);
            }
            else
            {
                atomic_inc(&diagnosticCounters[9]);
            }
        }
    }
    
    // Edge 5: Y-axis at (x=1, z=1), corners 5-7
    // Shared by: (x,y,z), (x+1,y,z), (x,y,z+1), (x+1,y,z+1)
    // This cell owns it!
    if (node.edgeMask & (1 << 5))
    {
        if (coords.x >= maxCoord || coords.z >= maxCoord)
        {
            atomic_inc(&diagnosticCounters[11]);  // at bbox boundary
        }
        else
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                atomic_inc(&diagnosticCounters[10]);
            }
            else
            {
                atomic_inc(&diagnosticCounters[11]);
            }
        }
    }
    
    // Edge 6: X-axis at (y=1, z=1), corners 6-7
    // Shared by: (x,y,z), (x,y+1,z), (x,y,z+1), (x,y+1,z+1)
    // This cell owns it!
    if (node.edgeMask & (1 << 6))
    {
        if (coords.y >= maxCoord || coords.z >= maxCoord)
        {
            atomic_inc(&diagnosticCounters[13]);
        }
        else
        {
            ulong nMorton1 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y, coords.z + 1);
            ulong nMorton3 = encodeMorton3(coords.x, coords.y + 1, coords.z + 1);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                atomic_inc(&diagnosticCounters[12]);
            }
            else
            {
                atomic_inc(&diagnosticCounters[13]);
            }
        }
    }
    
    // Edge 7: Y-axis at (x=0, z=1), corners 4-6
    // Shared by: (x,y,z), (x-1,y,z), (x,y,z+1), (x-1,y,z+1)
    // Owned by cell (x-1,y,z) via edge 5
    if (node.edgeMask & (1 << 7))
    {
        if (coords.x == 0)
        {
            atomic_inc(&diagnosticCounters[15]);
        }
        else
        {
            ulong ownerMorton = encodeMorton3(coords.x - 1, coords.y, coords.z);
            if (findNodeByMorton(nodes, numNodes, ownerMorton) >= 0)
            {
                atomic_inc(&diagnosticCounters[14]);
            }
            else
            {
                atomic_inc(&diagnosticCounters[15]);
            }
        }
    }
    
    // Edge 8: Z-axis at (x=0, y=0), corners 0-4
    // Shared by: (x,y,z), (x-1,y,z), (x,y-1,z), (x-1,y-1,z)
    // Owned by cell (x-1,y-1,z) via edge 10
    if (node.edgeMask & (1 << 8))
    {
        if (coords.x == 0 || coords.y == 0)
        {
            atomic_inc(&diagnosticCounters[17]);
        }
        else
        {
            ulong ownerMorton = encodeMorton3(coords.x - 1, coords.y - 1, coords.z);
            if (findNodeByMorton(nodes, numNodes, ownerMorton) >= 0)
            {
                atomic_inc(&diagnosticCounters[16]);
            }
            else
            {
                ulong n1 = encodeMorton3(coords.x - 1, coords.y, coords.z);
                ulong n2 = encodeMorton3(coords.x, coords.y - 1, coords.z);
                int idx1 = findNodeByMorton(nodes, numNodes, n1);
                int idx2 = findNodeByMorton(nodes, numNodes, n2);
                if (idx1 >= 0 && idx2 >= 0)
                {
                    atomic_inc(&diagnosticCounters[16]);
                }
                else
                {
                    atomic_inc(&diagnosticCounters[17]);
                }
            }
        }
    }
    
    // Edge 9: Z-axis at (x=1, y=0), corners 1-5
    // Shared by: (x,y,z), (x+1,y,z), (x,y-1,z), (x+1,y-1,z)
    // Owned by cell (x,y-1,z) via edge 10
    if (node.edgeMask & (1 << 9))
    {
        if (coords.y == 0)
        {
            atomic_inc(&diagnosticCounters[19]);
        }
        else
        {
            ulong ownerMorton = encodeMorton3(coords.x, coords.y - 1, coords.z);
            if (findNodeByMorton(nodes, numNodes, ownerMorton) >= 0)
            {
                atomic_inc(&diagnosticCounters[18]);
            }
            else
            {
                atomic_inc(&diagnosticCounters[19]);
            }
        }
    }
    
    // Edge 10: Z-axis at (x=1, y=1), corners 3-7
    // Shared by: (x,y,z), (x+1,y,z), (x,y+1,z), (x+1,y+1,z)
    // This cell owns it!
    if (node.edgeMask & (1 << 10))
    {
        if (coords.x >= maxCoord || coords.y >= maxCoord)
        {
            atomic_inc(&diagnosticCounters[21]);
        }
        else
        {
            ulong nMorton1 = encodeMorton3(coords.x + 1, coords.y, coords.z);
            ulong nMorton2 = encodeMorton3(coords.x, coords.y + 1, coords.z);
            ulong nMorton3 = encodeMorton3(coords.x + 1, coords.y + 1, coords.z);
            
            int nIdx1 = findNodeByMorton(nodes, numNodes, nMorton1);
            int nIdx2 = findNodeByMorton(nodes, numNodes, nMorton2);
            int nIdx3 = findNodeByMorton(nodes, numNodes, nMorton3);
            
            if (nIdx1 >= 0 && nIdx2 >= 0 && nIdx3 >= 0)
            {
                atomic_inc(&diagnosticCounters[20]);
            }
            else
            {
                atomic_inc(&diagnosticCounters[21]);
            }
        }
    }
    
    // Edge 11: Z-axis at (x=0, y=1), corners 2-6
    // Shared by: (x,y,z), (x-1,y,z), (x,y+1,z), (x-1,y+1,z)
    // Owned by cell (x-1,y,z) via edge 10
    if (node.edgeMask & (1 << 11))
    {
        if (coords.x == 0)
        {
            atomic_inc(&diagnosticCounters[23]);
        }
        else
        {
            ulong ownerMorton = encodeMorton3(coords.x - 1, coords.y, coords.z);
            if (findNodeByMorton(nodes, numNodes, ownerMorton) >= 0)
            {
                atomic_inc(&diagnosticCounters[22]);
            }
            else
            {
                atomic_inc(&diagnosticCounters[23]);
            }
        }
    }
}

// ============================================================================
// Discontinuity Diagnostic Kernel
// Counts cells that have gradient discontinuities (multiple surface components)
// Diagnostic counters layout (8 ints):
// [0] = cells with 1 component (smooth surface)
// [1] = cells with 2 components (one discontinuity)
// [2] = cells with 3 components
// [3] = cells with 4 components
// [4] = total cells analyzed
// [5] = sum of discontinuity scores (scaled by 1000)
// [6] = cells with discontinuity score > 0.5
// [7] = reserved
// ============================================================================

__kernel void count_discontinuities_diagnostic(
    __global OctreeNode const* nodes,
    __global int* discontinuityCounters,  // Array of 8 ints
    const int numNodes,
    const float3 bboxMin,
    const float3 bboxMax,
    PAYLOAD_ARGS,
    const float isoValue,
    const float gradientEpsilon)
{
    int id = get_global_id(0);
    if (id >= numNodes) return;

    OctreeNode node = nodes[id];
    
    // Skip halo nodes and cells without surface
    if (node.padding[0] == 1) return;
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
    
    // Collect edge intersection normals
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
        normals[intersectionCount] = computeGradientWithEps(intersection, gradientEpsilon, PASS_PAYLOAD_ARGS);
        intersectionCount++;
    }
    
    // Detect gradient discontinuities
    DiscontinuityResult discResult = detectGradientDiscontinuity(normals, intersectionCount, 0.3f);
    
    // Update counters
    atomic_inc(&discontinuityCounters[4]); // Total cells
    
    int compIdx = clamp(discResult.componentCount - 1, 0, 3);
    atomic_inc(&discontinuityCounters[compIdx]);
    
    // Track discontinuity scores
    int scoreScaled = (int)(discResult.discontinuityScore * 1000.0f);
    atomic_add(&discontinuityCounters[5], scoreScaled);
    
    if (discResult.discontinuityScore > 0.5f)
    {
        atomic_inc(&discontinuityCounters[6]);
    }
}

