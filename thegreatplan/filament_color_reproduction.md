# Filament Color Reproduction Planner

## Overview

This program computes optimal filament layer thicknesses to reproduce a target color palette using translucent 3D printing filaments. It uses the Beer-Lambert transmission model to predict how stacked filament layers produce colors when backlit (lithophane-style prints).

## Physical Model: Transmission Distance

### Core Concept

Each filament has:
- **Transmission Color** $T = (T_R, T_G, T_B) \in [0,1]^3$: The RGB color after light passes through one "transmission distance" of material
- **Transmission Distance** $d$ (mm): The thickness at which the transmission color is measured

### Beer-Lambert Law

For a single filament layer of thickness $t$:

$$\tau_c(t) = T_c^{t/d} = \exp\left(-\frac{-\ln(T_c)}{d} \cdot t\right)$$

where:
- $\tau_c$ = transmittance for channel $c \in \{R, G, B\}$
- $T_c$ = transmission color component
- $d$ = transmission distance
- $t$ = actual layer thickness

### Stacked Layers

For $n$ filament layers with thicknesses $t_1, \ldots, t_n$:

$$C_c = \prod_{i=1}^{n} T_{i,c}^{t_i/d_i} = \exp\left(-\sum_{i=1}^{n} \alpha_{i,c} \cdot t_i\right)$$

where $\alpha_{i,c} = -\ln(T_{i,c}) / d_i$ is the absorption coefficient.

### Linear Formulation

Taking logarithms, define **optical thickness** per channel:

$$L_c = -\ln(C_c) = \sum_{i=1}^{n} \alpha_{i,c} \cdot t_i$$

This is linear in the thicknesses $t_i$, making optimization tractable.

---

## Program Architecture

### 1. Data Structures

```cpp
/// A single filament type with its optical properties
struct Filament
{
    std::string name;                    ///< Human-readable name (e.g., "Prusament Galaxy Black")
    Eigen::Vector3f transmissionColor;   ///< RGB transmission color in linear space [0,1]
    float transmissionDistance;          ///< Distance (mm) for transmission color measurement
    Eigen::Vector3f alpha;               ///< Precomputed absorption coefficients per channel
    
    void computeAlpha()
    {
        for (int c = 0; c < 3; ++c)
        {
            // Clamp to avoid log(0)
            float const tc = std::max(transmissionColor[c], 1e-6f);
            alpha[c] = -std::log(tc) / transmissionDistance;
        }
    }
};

/// Constraint on layer thickness
struct ThicknessConstraints
{
    float minThickness = 0.0f;     ///< Minimum layer thickness (mm), e.g., 0.04
    float maxThickness = 10.0f;    ///< Maximum layer thickness (mm)
    float layerHeight = 0.0f;      ///< If > 0, quantize to multiples of this
};

/// A target color to reproduce
struct TargetColor
{
    Eigen::Vector3f linearRgb;     ///< Target color in linear RGB [0,1]
    float weight = 1.0f;           ///< Importance weight for optimization
};

/// Result for one target color
struct ColorSolution
{
    Eigen::Vector3f targetColor;           ///< The requested color
    Eigen::Vector3f achievedColor;         ///< The actually achievable color
    std::vector<float> thicknesses;        ///< Thickness per filament (in order)
    float colorError;                      ///< Delta-E or similar error metric
};

/// Complete solution for a palette
struct PaletteSolution
{
    std::vector<std::size_t> filamentOrder;    ///< Indices into original filament list
    std::vector<ColorSolution> colorSolutions; ///< Solution per target color
    float totalError;                          ///< Sum of weighted color errors
    float coverageScore;                       ///< Fraction of palette achievable within tolerance
};
```

### 2. Core Algorithms

#### 2.1 Forward Model: Predict Color from Thicknesses

```cpp
/// Compute resulting color given filament stack and thicknesses
Eigen::Vector3f predictColor(
    std::vector<Filament> const& filaments,
    std::vector<float> const& thicknesses)
{
    Eigen::Vector3f opticalThickness = Eigen::Vector3f::Zero();
    
    for (std::size_t i = 0; i < filaments.size(); ++i)
    {
        opticalThickness += filaments[i].alpha * thicknesses[i];
    }
    
    // C = exp(-L)
    return Eigen::Vector3f(
        std::exp(-opticalThickness[0]),
        std::exp(-opticalThickness[1]),
        std::exp(-opticalThickness[2])
    );
}
```

#### 2.2 Inverse Problem: Solve for Thicknesses

Given target color $C^{\text{target}}$, solve:

$$\min_{t \geq 0} \left\| A \cdot t - L^{\text{target}} \right\|^2$$

where:
- $A$ is a $3 \times n$ matrix with $A_{c,i} = \alpha_{i,c}$
- $L^{\text{target}}_c = -\ln(C^{\text{target}}_c)$
- Subject to: $t_{\min} \leq t_i \leq t_{\max}$

**Algorithm: Bounded Non-Negative Least Squares (BNNLS)**

```cpp
/// Solve for optimal thicknesses to match target color
std::vector<float> solveForThicknesses(
    std::vector<Filament> const& filaments,
    Eigen::Vector3f const& targetLinearRgb,
    ThicknessConstraints const& constraints)
{
    std::size_t const n = filaments.size();
    
    // Build A matrix (3 x n)
    Eigen::MatrixXf A(3, n);
    for (std::size_t i = 0; i < n; ++i)
    {
        A.col(i) = filaments[i].alpha;
    }
    
    // Compute target optical thickness
    Eigen::Vector3f L_target;
    for (int c = 0; c < 3; ++c)
    {
        float const tc = std::clamp(targetLinearRgb[c], 1e-6f, 1.0f);
        L_target[c] = -std::log(tc);
    }
    
    // Solve using iterative bounded least squares
    // (See Section 3.1 for algorithm details)
    return boundedNnls(A, L_target, constraints);
}
```

#### 2.3 Filament Order Optimization

The order of filaments affects which colors are achievable. We need to find the best permutation.

**Approach: Greedy + Local Search**

1. **Greedy Initialization**: 
   - For each position, choose the filament that maximizes marginal coverage improvement

2. **Local Search Refinement**:
   - Try pairwise swaps
   - Accept swaps that improve total coverage/error

3. **Optional: Genetic Algorithm** for larger filament sets

```cpp
/// Find optimal filament ordering for a target palette
std::vector<std::size_t> optimizeFilamentOrder(
    std::vector<Filament> const& filaments,
    std::vector<TargetColor> const& palette,
    ThicknessConstraints const& constraints)
{
    // Greedy initialization
    std::vector<std::size_t> order = greedyFilamentOrder(filaments, palette, constraints);
    
    // Local search refinement
    order = localSearchRefinement(order, filaments, palette, constraints);
    
    return order;
}
```

---

## Detailed Algorithm: Bounded NNLS Solver

### 3.1 Active Set Method

Since we have box constraints ($t_{\min} \leq t_i \leq t_{\max}$), use an active-set approach:

```
Algorithm: Bounded Least Squares

Input: A (3×n), b (3×1), bounds [lo, hi]
Output: x (n×1) minimizing ||Ax - b||² subject to lo ≤ x ≤ hi

1. Initialize x = clamp(A⁺b, lo, hi)  // Pseudo-inverse, clamped
2. Repeat until convergence:
   a. Compute gradient g = A'(Ax - b)
   b. Identify active set: 
      - L = {i : x[i] = lo[i] and g[i] > 0}
      - U = {i : x[i] = hi[i] and g[i] < 0}
      - F = remaining free variables
   c. Solve reduced problem on F: A_F * dx_F = -(Ax - b)
   d. Line search: find step size α that respects bounds
   e. Update x += α * dx
3. Return x
```

### 3.2 Simpler Alternative: Projected Gradient Descent

```cpp
std::vector<float> boundedNnls(
    Eigen::MatrixXf const& A,
    Eigen::Vector3f const& b,
    ThicknessConstraints const& constraints)
{
    std::size_t const n = A.cols();
    Eigen::VectorXf x = Eigen::VectorXf::Zero(n);
    
    // Initialize with clamped pseudo-inverse solution
    Eigen::VectorXf x_init = A.completeOrthogonalDecomposition().solve(b);
    for (std::size_t i = 0; i < n; ++i)
    {
        x[i] = std::clamp(x_init[i], constraints.minThickness, constraints.maxThickness);
    }
    
    // Projected gradient descent
    float const stepSize = 0.1f / A.squaredNorm();
    for (int iter = 0; iter < 1000; ++iter)
    {
        Eigen::Vector3f residual = A * x - b;
        Eigen::VectorXf gradient = A.transpose() * residual;
        
        // Gradient step
        x -= stepSize * gradient;
        
        // Project onto bounds
        for (std::size_t i = 0; i < n; ++i)
        {
            x[i] = std::clamp(x[i], constraints.minThickness, constraints.maxThickness);
        }
        
        // Check convergence
        if (gradient.norm() < 1e-6f)
        {
            break;
        }
    }
    
    // Quantize to layer height if specified
    if (constraints.layerHeight > 0.0f)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            x[i] = std::round(x[i] / constraints.layerHeight) * constraints.layerHeight;
            x[i] = std::clamp(x[i], constraints.minThickness, constraints.maxThickness);
        }
    }
    
    return std::vector<float>(x.data(), x.data() + n);
}
```

---

## Filament Ordering Strategy

### 4.1 Coverage Metric

For a given filament order, compute:

$$\text{Coverage} = \frac{1}{|P|} \sum_{c \in P} w_c \cdot \mathbb{1}[\Delta E(c, \hat{c}) < \epsilon]$$

where:
- $P$ = target palette
- $\hat{c}$ = best achievable color for target $c$
- $\Delta E$ = perceptual color difference (CIE Lab Delta-E)
- $\epsilon$ = acceptable error threshold (e.g., 2.0)

### 4.2 Greedy Order Selection

```cpp
std::vector<std::size_t> greedyFilamentOrder(
    std::vector<Filament> const& filaments,
    std::vector<TargetColor> const& palette,
    ThicknessConstraints const& constraints)
{
    std::vector<std::size_t> order;
    std::vector<bool> used(filaments.size(), false);
    
    while (order.size() < filaments.size())
    {
        float bestImprovement = -std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        
        for (std::size_t i = 0; i < filaments.size(); ++i)
        {
            if (used[i]) continue;
            
            // Temporarily add filament i
            std::vector<std::size_t> testOrder = order;
            testOrder.push_back(i);
            
            // Evaluate coverage with this order
            float const coverage = evaluateCoverage(
                reorderFilaments(filaments, testOrder), palette, constraints);
            
            float const improvement = coverage - currentCoverage;
            if (improvement > bestImprovement)
            {
                bestImprovement = improvement;
                bestIdx = i;
            }
        }
        
        order.push_back(bestIdx);
        used[bestIdx] = true;
    }
    
    return order;
}
```

### 4.3 Local Search Refinement

```cpp
std::vector<std::size_t> localSearchRefinement(
    std::vector<std::size_t> order,
    std::vector<Filament> const& filaments,
    std::vector<TargetColor> const& palette,
    ThicknessConstraints const& constraints)
{
    float bestScore = evaluateCoverage(
        reorderFilaments(filaments, order), palette, constraints);
    
    bool improved = true;
    while (improved)
    {
        improved = false;
        
        // Try all pairwise swaps
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            for (std::size_t j = i + 1; j < order.size(); ++j)
            {
                std::swap(order[i], order[j]);
                
                float const score = evaluateCoverage(
                    reorderFilaments(filaments, order), palette, constraints);
                
                if (score > bestScore)
                {
                    bestScore = score;
                    improved = true;
                }
                else
                {
                    // Revert swap
                    std::swap(order[i], order[j]);
                }
            }
        }
    }
    
    return order;
}
```

---

## Color Space Conversions

### 5.1 sRGB ↔ Linear RGB

```cpp
float srgbToLinear(float srgb)
{
    if (srgb <= 0.04045f)
    {
        return srgb / 12.92f;
    }
    return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float linear)
{
    if (linear <= 0.0031308f)
    {
        return linear * 12.92f;
    }
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}
```

### 5.2 RGB → CIE Lab (for Delta-E)

```cpp
Eigen::Vector3f rgbToLab(Eigen::Vector3f const& linearRgb)
{
    // RGB to XYZ (sRGB D65)
    Eigen::Matrix3f const M = (Eigen::Matrix3f() <<
        0.4124564f, 0.3575761f, 0.1804375f,
        0.2126729f, 0.7151522f, 0.0721750f,
        0.0193339f, 0.1191920f, 0.9503041f
    ).finished();
    
    Eigen::Vector3f xyz = M * linearRgb;
    
    // Normalize by D65 white point
    xyz[0] /= 0.95047f;
    xyz[1] /= 1.00000f;
    xyz[2] /= 1.08883f;
    
    // XYZ to Lab
    auto f = [](float t) -> float {
        float const delta = 6.0f / 29.0f;
        if (t > delta * delta * delta)
        {
            return std::cbrt(t);
        }
        return t / (3.0f * delta * delta) + 4.0f / 29.0f;
    };
    
    float const L = 116.0f * f(xyz[1]) - 16.0f;
    float const a = 500.0f * (f(xyz[0]) - f(xyz[1]));
    float const b = 200.0f * (f(xyz[1]) - f(xyz[2]));
    
    return Eigen::Vector3f(L, a, b);
}

float deltaE(Eigen::Vector3f const& lab1, Eigen::Vector3f const& lab2)
{
    return (lab1 - lab2).norm();  // CIE76, simple Euclidean
}
```

---

## User Interface Design

### 6.1 Input Specification

```cpp
struct ProgramInput
{
    /// List of available filaments
    std::vector<Filament> filaments;
    
    /// Target colors to reproduce (can be from image histogram)
    std::vector<TargetColor> palette;
    
    /// Constraints
    ThicknessConstraints constraints;
    
    /// Maximum acceptable Delta-E for "good match"
    float maxDeltaE = 3.0f;
};
```

### 6.2 Output Format

```cpp
struct ProgramOutput
{
    /// Optimal filament order (indices into input filaments)
    std::vector<std::size_t> filamentOrder;
    
    /// Ordered filament names for reference
    std::vector<std::string> orderedFilamentNames;
    
    /// Solution table: for each target color, the required thicknesses
    /// Row = target color index
    /// Columns = [Target R, G, B] [Achieved R, G, B] [t_1, t_2, ..., t_n] [DeltaE]
    std::vector<ColorSolution> solutions;
    
    /// Summary statistics
    float coveragePercent;      ///< % of colors within tolerance
    float averageDeltaE;        ///< Mean color error
    float maxDeltaE;            ///< Worst case error
};
```

### 6.3 CLI Interface

```
Usage: filament_color_solver [options] <filaments.json> <palette.json>

Options:
  --min-thickness <mm>    Minimum layer thickness (default: 0.0)
  --max-thickness <mm>    Maximum layer thickness (default: 10.0)
  --layer-height <mm>     Quantize to layer height (default: 0, no quantization)
  --max-delta-e <value>   Acceptable color error (default: 3.0)
  --output <file.csv>     Output CSV file (default: stdout)
  --verbose               Print optimization progress

Example:
  filament_color_solver --min-thickness 0.04 --max-thickness 5.0 \
                        --layer-height 0.04 filaments.json palette.json
```

---

## File Formats

### 7.1 Filaments JSON

```json
{
  "filaments": [
    {
      "name": "Prusament Galaxy Black",
      "transmission_color_srgb": [0.05, 0.05, 0.08],
      "transmission_distance_mm": 0.4
    },
    {
      "name": "Prusament Orange",
      "transmission_color_srgb": [0.95, 0.45, 0.10],
      "transmission_distance_mm": 1.2
    },
    {
      "name": "Generic White",
      "transmission_color_srgb": [0.98, 0.98, 0.98],
      "transmission_distance_mm": 0.3
    }
  ]
}
```

### 7.2 Palette JSON

```json
{
  "colors": [
    { "srgb": [1.0, 0.0, 0.0], "weight": 1.0 },
    { "srgb": [0.0, 1.0, 0.0], "weight": 1.0 },
    { "srgb": [0.0, 0.0, 1.0], "weight": 1.0 },
    { "srgb": [0.5, 0.5, 0.5], "weight": 2.0 }
  ]
}
```

### 7.3 Output CSV

```csv
# Filament order: Galaxy Black, Orange, White
# Constraints: min=0.04mm, max=5.0mm, layer_height=0.04mm
Target_R,Target_G,Target_B,Achieved_R,Achieved_G,Achieved_B,t_GalaxyBlack,t_Orange,t_White,DeltaE
1.000,0.000,0.000,0.912,0.087,0.052,0.120,2.400,0.000,12.3
0.000,1.000,0.000,0.245,0.678,0.234,0.080,0.000,1.600,28.5
...
```

---

## Implementation Phases

### Phase 1: Core Math Library
- [ ] Filament struct with absorption coefficient computation
- [ ] Forward model: predict color from thicknesses
- [ ] sRGB ↔ Linear RGB conversions
- [ ] RGB → Lab conversion for Delta-E

### Phase 2: Inverse Solver
- [ ] Bounded NNLS solver (projected gradient descent)
- [ ] Layer height quantization
- [ ] Error metric computation

### Phase 3: Ordering Optimization
- [ ] Greedy order initialization
- [ ] Local search refinement
- [ ] Coverage evaluation

### Phase 4: I/O and CLI
- [ ] JSON parsing for filaments and palette
- [ ] CSV output generation
- [ ] Command-line argument parsing

### Phase 5: Advanced Features (Optional)
- [ ] Genetic algorithm for larger filament sets
- [ ] Image histogram extraction as palette input
- [ ] Visualization of achievable color gamut
- [ ] GPU acceleration for large palettes

---

## Recommended Libraries

| Library | Purpose | Availability |
|---------|---------|--------------|
| **Eigen** | Linear algebra, matrix operations | VCPKG ✓ |
| **nlohmann/json** | JSON parsing | VCPKG ✓ |
| **CLI11** | Command-line parsing | VCPKG ✓ |
| **fmt** | String formatting | VCPKG ✓ |

All recommended libraries are available through VCPKG and are widely used in the C++ community.

---

## Example Workflow

1. **User provides filaments.json** with 4 filament colors
2. **User provides palette.json** with 20 target colors from an image
3. **Program optimizes filament order** (4! = 24 permutations, exhaustive search feasible)
4. **For each permutation**, solve thickness for all 20 colors, compute total error
5. **Select best permutation** based on coverage/error
6. **Output CSV** with thickness table

For the best order, user can then:
- Import CSV into slicer
- Set layer thicknesses per region
- Print lithophane or multi-color model

---

## Mathematical Notes

### Why Order Matters

In the Beer-Lambert model, the order of layers doesn't affect the final color (multiplication is commutative). However, **in practice**:

1. **Layer adhesion**: Some color combinations print better in certain orders
2. **Diffusion effects**: Real filaments scatter light, causing order-dependent effects
3. **Practical constraints**: You may want specific colors on top/bottom

For pure transmission modeling, order is irrelevant. The program structure supports order optimization for future extensions with scattering models.

### Gamut Limitations

Not all colors are achievable. The **achievable gamut** is:

$$\mathcal{G} = \left\{ \exp\left(-\sum_i \alpha_i t_i\right) : t_i \in [t_{\min}, t_{\max}] \right\}$$

This is a convex set in log-space. Very saturated colors or colors outside the filament's "span" may be unreachable.

---

## References

1. Beer-Lambert Law: https://en.wikipedia.org/wiki/Beer%E2%80%93Lambert_law
2. CIE Lab color space: https://en.wikipedia.org/wiki/CIELAB_color_space
3. Non-negative least squares: Lawson & Hanson (1995)
4. Kubelka-Munk Theory: https://en.wikipedia.org/wiki/Kubelka%E2%80%93Munk_theory

---

## Appendix: Transmission vs Reflection Modes

This document focuses on the **transmission (lithophane)** model where light passes through stacked filament layers. For frontlit viewing (like HueForge), a different physical model applies.

### Transmission Mode (This Document)
- **Light path**: Through the material
- **Model**: Beer-Lambert absorption
- **Effect**: Thicker = darker
- **Use case**: Backlit prints, lamp shades

### Reflection Mode (See color_aware_3mf_export.md)
- **Light path**: Into surface, scatter, back out
- **Model**: Kubelka-Munk or layered visibility
- **Effect**: Layer colors blend based on opacity
- **Use case**: Frontlit wall art, HueForge-style prints

### Key Difference

In **transmission**, all layers contribute multiplicatively to absorption:
$$C = \prod_i T_i^{t_i/d_i}$$

In **reflection**, top layers occlude bottom layers based on opacity:
$$C = \sum_i C_i \cdot V_i$$

where $V_i$ is the visibility of layer $i$ through all layers above it.

### When to Use Which

| Scenario | Mode |
|----------|------|
| Light behind print | Transmission |
| Light in front of print | Reflection |
| Translucent material | Both can apply |
| Opaque material | Reflection only |
| 3D object with mixed lighting | Hybrid (per-face)
