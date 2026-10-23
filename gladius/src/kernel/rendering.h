
float4 cachedSdf(float3 pos, PAYLOAD_ARGS);
float4 modelInternal(float3 pos, PAYLOAD_ARGS);
// Model function - provided by dynamically generated code, called by static kernels
float4 model(float3 pos, PAYLOAD_ARGS);
// Translucency channel - second generated entry point, see TranslucencyCodeGenerator.
// Kept out of the float4 model ABI so the 141 existing model() call sites stay untouched.
float translucencyModel(float3 pos, PAYLOAD_ARGS);
struct DistanceColor map(float3 pos, PAYLOAD_ARGS);
struct DistanceColor mapCached(float3 pos, PAYLOAD_ARGS);

// Gradient magnitude estimation for adaptive over-relaxation (FR-004)
float estimateGradientMagnitude(float3 pos, PAYLOAD_ARGS);

// Enhanced rayCast with adaptive ω and distance initialization support
struct RayCastResult
rayCast(float3 eyePosition, float3 rayDirection, float startDistance, bool useDistanceInit, float distanceInitValue, PAYLOAD_ARGS);

// Legacy rayCast overload for backward compatibility (no distance init)
struct RayCastResult
rayCastLegacy(float3 eyePosition, float3 rayDirection, float startDistance, PAYLOAD_ARGS);

float3 surfaceNormal(float3 pos, PAYLOAD_ARGS);
float calcSoftshadow(float3 pos, float3 rayDirection, float mint, float tmax, PAYLOAD_ARGS);

float calcAmbientOcclusion(float3 pos, float3 normal, PAYLOAD_ARGS);

float3 reflect(float3 inVector, float3 normal);

float4
shadingMetal(float3 pos, float3 col, float3 normalOfSurface, float3 rayDirection, PAYLOAD_ARGS);

float4 shadingWhitePolymer(float3 pos, float3 normalOfSurface, float3 rayDirection, PAYLOAD_ARGS);

float3 Uncharted2ToneMapping(float3 color);

float3 fusion(float x);
float3 distanceMeter(float dist, float rayLength, float3 rayDir, float camHeight);

float4
shadingIsolines(float3 pos, float rayLength, float3 rayDirection, float height, PAYLOAD_ARGS);

float4 shadingBluePolymer(float3 pos, float3 normalOfSurface, float3 rayDirection, PAYLOAD_ARGS);

float4 determineColor(struct RayCastResult raycastingResult,
                      float3 eyePosition,
                      float3 rayDirection,
                      PAYLOAD_ARGS);

/// Translucency support - Beer-Lambert accumulation along the ray path.
/// The authored scalar channel is an ISO 19307 translucency alpha for a reference
/// thickness; converting it to a volumetric coefficient keeps the authored quantity
/// intact while letting the marcher integrate over the actual path length:
///   sigma = -ln(1 - alpha) / d0   =>   T(t) = exp(-sigma * t)
struct InteriorTransmittance
{
    float transmittance;
    float exitDistance;
    bool exited;
};

/// True when the high-quality path should march through the solid instead of stopping
/// at the first surface. Preview (AM_ONLY_PRECOMPSDF) always stays opaque.
bool isTranslucencyEnabled(PAYLOAD_ARGS);

/// Beer-Lambert coefficient of the translucency channel at a position (1/mm).
float attenuationCoefficient(float3 pos, PAYLOAD_ARGS);

/// Optical depth accumulated while the ray travels through the solid, starting at the
/// entry crossing. Fixed stepping on purpose: sphere tracing with |SDF| degenerates
/// inside a solid because every step lands back on the boundary.
struct InteriorTransmittance
interiorTransmittance(float3 eyePosition, float3 rayDirection, float entryDistance, PAYLOAD_ARGS);

/// Front-to-back accumulation over up to translucencyMaxSurfaces shells. The first
/// crossing is passed in so the caller's existing march is reused.
float4 renderTranslucentRay(struct RayCastResult firstResult,
                            float3 eyePosition,
                            float3 rayDirection,
                            PAYLOAD_ARGS);

float4 shadeRayResult(struct RayCastResult raycastingResult,
                      float3 eyePosition,
                      float3 rayDirection,
                      PAYLOAD_ARGS);

float16 modelViewPerspectiveMatrix(float3 eyePosition, float3 lookAt, float roll);
