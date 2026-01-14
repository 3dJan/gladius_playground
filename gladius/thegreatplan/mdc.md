Implement Manifold Dual Contouring (MDC) in OpenCL with the following requirements:

1. **Input**:
   - A 3D grid of cells representing an implicit surface (Signed Distance Field).
   - Hermite data: intersection points and normals for edges crossing the surface.

2. **Steps**:
   - **Kernel 1 (QEF Solve)**:
     - For each cell, gather Hermite samples.
     - Compute Quadratic Error Function (QEF) to find optimal vertex position.
     - Detect ambiguous topology (e.g., more than 4 samples or complex sign changes).
     - Output:
       - `baseVertices[cellId]` (float4)
       - `splitFlags[cellId]` (int: number of extra vertices needed)

   - **Prefix Sum (Blelloch Scan)**:
     - Implement an exclusive prefix sum on `splitFlags` to compute `vertexOffsets`.
     - This determines where each thread writes its duplicate vertices in the global buffer.

   - **Kernel 2 (Vertex Emission)**:
     - Write primary vertex to `outVertices[cellId]`.
     - If `splitFlags[cellId] > 0`, emit duplicates at positions:
       `outVertices[vertexOffsets[cellId] + i]` for `i` in [0, splitFlags[cellId]-1].
     - Apply small offsets to duplicates to maintain manifoldness.

   - **Kernel 3 (Face Emission)**:
     - Build faces referencing correct vertex IDs (including duplicates).
     - Ensure manifold connectivity (each edge belongs to exactly two faces).

3. **Blelloch Scan Details**:
   - Implement upsweep and downsweep phases for prefix sum in OpenCL.
   - Use local memory for block-level scan and global memory for inter-block aggregation.

4. **Output**:
   - A watertight, manifold mesh with vertices and faces stored in global buffers.

5. **Performance Considerations**:
   - Use work-groups for scan operations.
   - Avoid race conditions with atomic operations or prefix sums.
   - Optimize QEF solver using small matrix inversion or SVD.

Provide:
- Full OpenCL kernels for QEF pass, Blelloch scan, vertex emission, and face emission.
- Host-side code to orchestrate kernel launches and buffer allocations.
- Comments explaining each step.