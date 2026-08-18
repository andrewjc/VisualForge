// Mirrors vf::renderer::lighting's evaluation branch for branch. Every rule
// here exists once on each side and nowhere else, so the mirror and its
// oracle cannot drift by interpretation.

// Phase 18 supplies the shadow term by ray query. Without the extension the
// module has no way to trace, and direct lighting stays unshadowed and says
// so, which is what lets a parity metric mask the term rather than attribute
// the difference to an error.
#ifdef VF_RAY_QUERY
const bool kVfShadowTermAvailable = true;
#else
const bool kVfShadowTermAvailable = false;
#endif

// Mirrors vf::renderer::accel::kDirectionalShadowDistance.
const float kVfDirectionalShadowDistance = 1.0e6;
// Mirrors the epsilons in vf::renderer::accel::OffsetRayOrigin.
const float kVfShadowBaseEpsilon = 1.0e-3;
const float kVfShadowRelativeEpsilon = 1.0e-5;

uint vfLightType(GpuLightRecordV1 light)
{
    return uint(light.color.w + 0.5);
}

// Outside the captured radius the light contributes nothing at all, which is
// what lets a light list be culled by radius without changing the image.
float vfAttenuation(GpuLightRecordV1 light, float distance)
{
    uint type = vfLightType(light);
    if (type == kVfLightTypeDirectional || type == kVfLightTypeAmbient) {
        return 1.0;
    }
    float radius = light.attenuation.w;
    if (distance >= radius) return 0.0;
    float denominator = light.attenuation.x +
        light.attenuation.y * distance +
        light.attenuation.z * distance * distance;
    if (!(denominator > 0.0)) return 0.0;
    return clamp(1.0 / denominator, 0.0, 1.0);
}

// Cones are compared as cosines because that is what a dot product already
// is; converting to angles per fragment would repeat arithmetic for nothing.
float vfCone(GpuLightRecordV1 light, float axisCosine)
{
    if (vfLightType(light) != kVfLightTypeSpot) return 1.0;
    float inner = light.cone.x;
    float outer = light.cone.y;
    if (axisCosine >= inner) return 1.0;
    if (axisCosine <= outer) return 0.0;
    float span = inner - outer;
    if (!(span > 0.0)) return 0.0;
    return clamp(pow((axisCosine - outer) / span, light.cone.z), 0.0, 1.0);
}

// `position` and `normal` are camera relative, matching the light positions
// the host uploaded.
vec3 vfDirectLighting(
    GpuLightRecordV1 light,
    vec3 position,
    vec3 normal)
{
    uint type = vfLightType(light);
    vec3 radiance = light.color.rgb;
    if (type == kVfLightTypeAmbient) {
        return radiance;
    }
    vec3 toLight;
    float distance = 0.0;
    if (type == kVfLightTypeDirectional) {
        // The record stores the direction the light travels, so the vector
        // toward it is the negation.
        toLight = -light.direction.xyz;
    } else {
        toLight = light.position.xyz - position;
        distance = length(toLight);
    }
    float toLightLength = length(toLight);
    if (!(toLightLength > 0.0)) return vec3(0.0);
    toLight /= toLightLength;

    // Clamped at zero: a surface facing away receives nothing, and a
    // negative cosine would subtract light from the frame.
    float cosine = max(0.0, dot(normal, toLight));
    float scale = cosine * vfAttenuation(light, distance);
    if (type == kVfLightTypeSpot) {
        scale *= vfCone(light, -dot(light.direction.xyz, toLight));
    }
    return radiance * scale;
}

// Mirrors vf::renderer::accel::OffsetRayOrigin. A ray starting on the surface
// re-hits the triangle that spawned it and every lit pixel shadows itself.
// Float spacing grows with magnitude, so the offset scales with the point's
// own distance from the origin rather than being a fixed epsilon.
vec3 vfOffsetRayOrigin(vec3 position, vec3 geometricNormal, float scale)
{
    float lengthSquared = dot(geometricNormal, geometricNormal);
    // A degenerate normal cannot offset in any direction, so the origin is
    // returned unchanged rather than moved somewhere arbitrary.
    if (!(lengthSquared > 0.0) || isinf(lengthSquared)) return position;
    vec3 normal = geometricNormal * (1.0 / sqrt(lengthSquared));
    float magnitude = length(position);
    float offset =
        scale * (kVfShadowBaseEpsilon + kVfShadowRelativeEpsilon * magnitude);
    return position + normal * offset;
}

// Mirrors vf::renderer::accel::ShadowRayForLight. One rule for the ray a
// light casts, so the query below and the CPU oracle cannot disagree about
// the origin, the direction, or how far the ray reaches.
void vfShadowRayForLight(
    GpuLightRecordV1 light,
    vec3 position,
    vec3 geometricNormal,
    out vec3 origin,
    out vec3 direction,
    out float maximumDistance)
{
    origin = vfOffsetRayOrigin(position, geometricNormal, 1.0);
    if (vfLightType(light) == kVfLightTypeDirectional) {
        // The record stores the direction the light travels, so the vector
        // toward it is the negation.
        direction = -light.direction.xyz;
        maximumDistance = kVfDirectionalShadowDistance;
        return;
    }
    // Measured from the offset origin, which is where the ray starts.
    vec3 toLight = light.position.xyz - origin;
    float distance = length(toLight);
    if (!(distance > 0.0) || isinf(distance)) {
        direction = vec3(0.0);
        maximumDistance = 0.0;
        return;
    }
    direction = toLight * (1.0 / distance);
    // Stopping at the light: anything past it is behind the light and cannot
    // shadow the surface, so reaching further would invent occluders.
    maximumDistance = distance;
}

#ifdef VF_RAY_QUERY
// Whether a ray-query candidate on this geometry actually blocks. The
// structure leaves alpha-tested geometry non-opaque, so traversal reports it
// as a candidate rather than committing it, and the shader decides -- through
// the same coverage rule and the same per-object alpha record the raster pass
// used, because a shadow silhouette disagreeing with the surface silhouette
// is exactly the artefact this is for.
//
// Opaque geometry never arrives here: the structure commits it without asking.
bool vfCandidateOccludes(GpuGeometryRecordV1 record, uint triangle, vec2 bary)
{
    if ((record.flags & kVfGeometryAlphaTested) == 0u) return true;
    float sampledAlpha = 1.0;
    if (record.textureIndex != 0xFFFFFFFFu) {
        sampledAlpha = textureLod(sceneMaterialTextures[record.textureIndex],
            vfHitTexCoord(record, triangle, bary), 0.0).a;
    }
    // Dithered against the pixel being shaded rather than the geometry being
    // crossed: a dithered fade is a screen-space pattern, and the occluder's
    // own coordinates would give the shadow a second one that swims with the
    // light rather than sitting still on the surface.
    return vfEvaluateCoverage(sceneVisibility.records[record.objectIndex],
        sampledAlpha, uvec2(gl_FragCoord.xy)).covered;
}

// Fully occluded or fully lit. Opaque geometry commits without asking, so a
// committed hit is a real occlusion and the query can stop at the first one
// instead of walking the whole ray. Alpha-tested geometry is left non-opaque
// by the structure and arrives as a candidate, which is what gives a cutout a
// shadow shaped like its texture rather than like its triangle.
float vfShadowTerm(GpuLightRecordV1 light, vec3 position, vec3 normal)
{
    // Switched off for the frame, mirroring lighting::ShadeSurfaceGpu. Fully
    // lit rather than fully shadowed: an isolation must remove the term, and
    // shadowing everything removes the light as well.
    if ((sceneEnvironment.record.flagsAndCount.x &
        kVfEnvironmentShadowsDisabled) != 0u) {
        return 1.0;
    }
    vec3 origin;
    vec3 direction;
    float maximumDistance;
    vfShadowRayForLight(
        light, position, normal, origin, direction, maximumDistance);
    if (!(maximumDistance > 0.0)) return 1.0;

    rayQueryEXT query;
    // No `gl_RayFlagsOpaqueEXT`. Forcing every candidate opaque is what made
    // a cutout cast the shadow of its bounding triangle: the flag overrides
    // the structure and commits geometry the shader was meant to test.
    rayQueryInitializeEXT(query, sceneTlas,
        gl_RayFlagsTerminateOnFirstHitEXT,
        0xFFu, origin, 0.0, direction, maximumDistance);
    // Looped rather than called once: rayQueryProceedEXT reports whether
    // traversal has more to do, and the spec only guarantees completion once
    // it has returned false. Each pass may offer a candidate on non-opaque
    // geometry, which only becomes an occlusion if it is confirmed.
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) !=
            gl_RayQueryCandidateIntersectionTriangleEXT) {
            continue;
        }
        uint candidateGeometry =
            rayQueryGetIntersectionGeometryIndexEXT(query, false);
        GpuGeometryRecordV1 candidateRecord =
            sceneGeometryObjects.records[
                candidateGeometry < sceneGeometryObjects.records.length()
                    ? candidateGeometry : 0u];
        if (vfCandidateOccludes(candidateRecord,
                rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
                rayQueryGetIntersectionBarycentricsEXT(query, false))) {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) !=
        gl_RayQueryCommittedIntersectionNoneEXT) {
        return 0.0;
    }
    return 1.0;
}
#else
float vfShadowTerm(GpuLightRecordV1 light, vec3 position, vec3 normal)
{
    return 1.0;
}
#endif

// Saturates at the captured maximum rather than at one, or distant geometry
// would vanish into fog the engine never applied.
float vfFog(GpuEnvironmentV1 environment, float distance)
{
    float fogNear = environment.ambientAndFogNear.w;
    float fogFar = environment.sunDirectionAndFogFar.w;
    float fogPower = environment.fogColorAndPower.w;
    float fogMaximum = environment.moonColorAndFogMaximum.w;
    if (distance <= fogNear) return 0.0;
    float span = fogFar - fogNear;
    if (!(span > 0.0)) return 0.0;
    float t = clamp((distance - fogNear) / span, 0.0, 1.0);
    return clamp(pow(t, fogPower) * fogMaximum, 0.0, fogMaximum);
}

// The complete opaque shading result: ambient plus every active light, then
// fogged toward the environment's fog colour.
vec3 vfShadeSurface(
    GpuEnvironmentV1 environment,
    vec3 albedo,
    vec3 position,
    vec3 normal,
    float viewDistance)
{
    // A frame with no captured lighting leaves the albedo alone, exactly as
    // every phase before this one did. Without the declaration a zeroed
    // environment would read as ambient zero and black out the scene.
    if ((environment.flagsAndCount.x & kVfEnvironmentPresent) == 0u) {
        return albedo;
    }
    vec3 lit = environment.ambientAndFogNear.rgb;
    uint count = environment.flagsAndCount.y;
    for (uint index = 0u; index < count; ++index) {
        GpuLightRecordV1 light = sceneLights.records[index];
        vec3 direct = vfDirectLighting(light, position, normal);
        // Ambient is not cast from anywhere, so no ray of it can be blocked.
        // Shadowing it would black out the interior of every shadow instead
        // of leaving the ambient floor the engine shows.
        if (vfLightType(light) != kVfLightTypeAmbient) {
            direct *= vfShadowTerm(light, position, normal);
        }
        lit += direct;
    }
    vec3 shaded = albedo * lit;
    float fog = vfFog(environment, viewDistance);
    return mix(shaded, environment.fogColorAndPower.rgb, fog);
}
