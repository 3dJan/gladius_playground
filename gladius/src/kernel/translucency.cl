/// @file translucency.cl
/// @brief Beer-Lambert translucency compositor for the high-quality render kernels.
///
/// This module is only linked into RenderProgram: it calls the generated
/// translucencyModel entry point, which the slicer, contour and dual-contouring
/// programs do not have. Keeping it separate leaves those programs untouched.

/// @brief True when the ray should march through the solid instead of stopping at the
/// first surface. Only the high-quality paths qualify: the precomputed-SDF preview has
/// no access to the translucency channel, so it stays opaque by design.
bool isTranslucencyEnabled(PAYLOAD_ARGS)
{
    if ((renderingSettings.flags & RF_ENABLE_TRANSLUCENCY) == 0)
    {
        return false;
    }
    if (renderingSettings.approximation & AM_ONLY_PRECOMPSDF)
    {
        return false;
    }
    return renderingSettings.translucencyReferenceThicknessMm > 0.f;
}

/// @brief Beer-Lambert coefficient (1/mm) of the translucency channel at a position.
///
/// The graph's scalar channel is an ISO 19307 translucency alpha for a reference
/// thickness. Converting it to a volumetric coefficient keeps the authored quantity
/// intact while letting the marcher integrate along the actual path length:
///   sigma = -ln(1 - alpha) / d0    =>   T(t) = exp(-sigma * t)
/// so a slab of thickness d0 absorbs exactly the authored alpha. Clamping alpha below
/// 1 keeps sigma finite for fully opaque regions, which is what a real material has.
float attenuationCoefficient(float3 pos, PAYLOAD_ARGS)
{
    float const referenceThickness = max(renderingSettings.translucencyReferenceThicknessMm, 0.001f);
    float const alpha = clamp(translucencyModel(pos, PASS_PAYLOAD_ARGS), 0.f, 0.9999f);
    return -log(1.f - alpha) / referenceThickness;
}

/// @brief Optical depth accumulated while the ray travels through the solid.
///
/// Fixed stepping is used on purpose: sphere tracing with |SDF| degenerates inside a
/// solid because every step lands back on the boundary. The far crossing is refined so
/// the final partial segment is not over-counted.
struct InteriorTransmittance
interiorTransmittance(float3 eyePosition, float3 rayDirection, float entryDistance, PAYLOAD_ARGS)
{
    float const stepSize = max(renderingSettings.translucencyInteriorStepSize, 0.001f);
    int const maxSteps = max((int)(renderingSettings.translucencyMaxInteriorSteps), 1);
    float const transmittanceFloor = clamp(renderingSettings.translucencyFloor, 0.f, 1.f);
    float const maxTravelDistance = 100000.f;

    struct InteriorTransmittance result;
    float distance = entryDistance;
    float sigma = attenuationCoefficient(eyePosition + distance * rayDirection, PASS_PAYLOAD_ARGS);
    float opticalDepth = 0.f;

    for (int step = 0; step < maxSteps; ++step)
    {
        float nextDistance = distance + stepSize;
        float exitDistance = nextDistance;
        bool exited = false;

        if (modelInternal(eyePosition + nextDistance * rayDirection, PASS_PAYLOAD_ARGS).w > 0.f)
        {
            // Refine the far crossing so the final partial segment is not over-counted.
            float low = distance;
            float high = nextDistance;
            for (int refinement = 0; refinement < 5; ++refinement)
            {
                float const middle = (low + high) * 0.5f;
                if (modelInternal(eyePosition + middle * rayDirection, PASS_PAYLOAD_ARGS).w > 0.f)
                {
                    high = middle;
                }
                else
                {
                    low = middle;
                }
            }
            nextDistance = low;
            exitDistance = high;
            exited = true;
        }

        float const nextSigma =
          attenuationCoefficient(eyePosition + nextDistance * rayDirection, PASS_PAYLOAD_ARGS);
        // Trapezoidal integration of sigma over the segment.
        opticalDepth += (sigma + nextSigma) * 0.5f * (nextDistance - distance);

        if (exited)
        {
            result.transmittance = exp(-opticalDepth);
            result.exitDistance = exitDistance;
            result.exited = true;
            return result;
        }

        distance = nextDistance;
        sigma = nextSigma;
        if (distance > maxTravelDistance || exp(-opticalDepth) < transmittanceFloor)
        {
            break;
        }
    }

    result.transmittance = exp(-opticalDepth);
    result.exitDistance = distance;
    result.exited = false;
    return result;
}

/// @brief Front-to-back accumulation over up to translucencyMaxSurfaces shells.
///
/// Every surface is shaded exactly like the opaque path and then attenuated by the
/// optical depth travelled inside the solid before reaching the next surface. The
/// caller's first crossing is reused instead of re-marching it.
float4 renderTranslucentRay(struct RayCastResult firstResult,
                            float3 eyePosition,
                            float3 rayDirection,
                            PAYLOAD_ARGS)
{
    float4 const bgColor = (float4)(0.1f, 0.1f, 0.1f, 1.f);
    int const maxSurfaces = max((int)(renderingSettings.translucencyMaxSurfaces), 1);
    float const transmittanceFloor = clamp(renderingSettings.translucencyFloor, 0.f, 1.f);

    float transmittance = 1.f;
    float3 accumulated = (float3)(0.f);
    struct RayCastResult current = firstResult;

    for (int surface = 0; surface < maxSurfaces; ++surface)
    {
        if (current.hit < 0.f)
        {
            accumulated += transmittance * bgColor.xyz;
            break;
        }

        accumulated +=
          transmittance * determineColor(current, eyePosition, rayDirection, PASS_PAYLOAD_ARGS).xyz;

        struct InteriorTransmittance const interior =
          interiorTransmittance(eyePosition, rayDirection, current.traveledDistance, PASS_PAYLOAD_ARGS);
        transmittance *= interior.transmittance;
        if (!interior.exited || transmittance < transmittanceFloor)
        {
            break;
        }
        current = rayCastLegacy(eyePosition, rayDirection, interior.exitDistance, PASS_PAYLOAD_ARGS);
    }

    return (float4)(accumulated, 1.f);
}

/// @brief Single entry point used by the render kernels: opaque shading, or the
/// translucent compositor when the feature is on. Hits on the build plate or other
/// helper geometry stay opaque.
float4 shadeRayResult(struct RayCastResult raycastingResult,
                      float3 eyePosition,
                      float3 rayDirection,
                      PAYLOAD_ARGS)
{
    if (isTranslucencyEnabled(PASS_PAYLOAD_ARGS) && raycastingResult.hit > 0.f &&
        raycastingResult.type == 1.f)
    {
        return renderTranslucentRay(raycastingResult, eyePosition, rayDirection, PASS_PAYLOAD_ARGS);
    }
    return determineColor(raycastingResult, eyePosition, rayDirection, PASS_PAYLOAD_ARGS);
}
