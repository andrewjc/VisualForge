if(NOT DEFINED REFLECT OR NOT DEFINED SPIRV OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "GenerateSceneShaderLayout requires REFLECT, SPIRV, and OUTPUT")
endif()

execute_process(
    COMMAND "${REFLECT}" --yaml "${SPIRV}"
    RESULT_VARIABLE VF_REFLECT_RESULT
    OUTPUT_VARIABLE VF_REFLECTION
    ERROR_VARIABLE VF_REFLECT_ERROR)
if(NOT VF_REFLECT_RESULT EQUAL 0)
    message(FATAL_ERROR "SPIR-V reflection failed: ${VF_REFLECT_ERROR}")
endif()
if(NOT VF_REFLECTION MATCHES "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER")
    message(FATAL_ERROR "phase11 records were not reflected as storage buffers")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+7")
    message(FATAL_ERROR "phase11 object records drifted from set 0 binding 7")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+8")
    message(FATAL_ERROR "phase11 G-buffer drifted from set 0 binding 8")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+9")
    message(FATAL_ERROR "phase12 instance records drifted from set 0 binding 9")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+13")
    message(FATAL_ERROR "phase15 visibility records drifted from set 0 binding 13")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+14")
    message(FATAL_ERROR "phase16 family records drifted from set 0 binding 14")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+160")
    message(FATAL_ERROR "phase16 family record drifted from 160 bytes")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+15")
    message(FATAL_ERROR "phase17 light records drifted from set 0 binding 15")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+16")
    message(FATAL_ERROR "phase17 environment drifted from set 0 binding 16")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+80")
    message(FATAL_ERROR "phase17 light record drifted from 80 bytes")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+112")
    message(FATAL_ERROR "phase17 environment record drifted from 112 bytes")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+224")
    message(FATAL_ERROR "phase11 object record drifted from 224 bytes")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+64")
    message(FATAL_ERROR "phase11 G-buffer pixel drifted from 64 bytes")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+160")
    message(FATAL_ERROR "phase12 instance record drifted from 160 bytes")
endif()

file(WRITE "${OUTPUT}" [=[#pragma once

#include <cstddef>
#include <cstdint>

namespace vf::renderer::scene {

// Stable hash of the reflected std430 object/instance/G-buffer contract.
inline constexpr std::uint64_t kPhase11SceneLayoutHash =
    0x4A31'CC72'65F0'91D8ull;
inline constexpr std::uint64_t kPhase12InstanceLayoutHash =
    0x93B7'2E10'4C5D'A806ull;
inline constexpr std::uint64_t kPhase15VisibilityLayoutHash =
    0x2D8F'4B60'71CA'39E4ull;
inline constexpr std::uint64_t kPhase16FamilyLayoutHash =
    0x6C1A'93D7'2E58'F04Bull;
inline constexpr std::uint32_t kSceneObjectDescriptorBinding = 7;
inline constexpr std::uint32_t kSceneInstanceDescriptorBinding = 9;
inline constexpr std::uint32_t kSceneVisibilityDescriptorBinding = 13;
inline constexpr std::uint32_t kSceneFamilyDescriptorBinding = 14;
inline constexpr std::uint32_t kSceneLightDescriptorBinding = 15;
inline constexpr std::uint32_t kSceneEnvironmentDescriptorBinding = 16;
inline constexpr std::uint32_t kSceneTlasDescriptorBinding = 17;
// Maps a bottom-level geometry index back to the object whose records
// describe it. A ray query recovers which geometry it hit but has no vertex
// attributes bound, so without this table a hit cannot be shaded at all.
inline constexpr std::uint32_t kSceneGeometryObjectDescriptorBinding = 18;
// The colour target as it stood before any refractive draw began. A
// refractive surface that samples the live target instead sees whichever
// refractive surfaces were drawn before it, so two panes of glass show each
// other and the image depends on draw order rather than on depth.
inline constexpr std::uint32_t kSceneRefractionDescriptorBinding = 19;
// The frame's material textures, one descriptor per texture-library entry,
// selected per draw through ScenePushConstantsV1::textureIndex. A separate
// binding from the single base texture at 1, which the phase 6, 9 and 16
// shaders each declare and each have a build-time reflection gate over.
inline constexpr std::uint32_t kSceneMaterialTextureDescriptorBinding = 20;
// Fixed, and mirrored in scene_layout.glsl. The index is a push constant and
// therefore dynamically uniform, so the array needs no non-uniform indexing;
// entries past the frame's library count are bound but never sampled, which
// is what descriptorBindingPartiallyBound permits.
inline constexpr std::uint32_t kSceneMaterialTextureCapacity = 256;
// The frame's index and vertex streams, bound so a ray-query hit can recover
// the triangle it found. A ray query returns a geometry index and a pair of
// barycentrics and nothing else -- no vertex attributes -- so a hit could
// only ever be shaded from per-object constants. With these it can read the
// three vertices it actually struck.
inline constexpr std::uint32_t kSceneIndexDescriptorBinding = 21;
inline constexpr std::uint32_t kSceneVertexDescriptorBinding = 22;
inline constexpr std::uint32_t kGpuGeometryRecordSize = 32;

inline constexpr std::uint32_t kGpuOpaqueObjectSize = 224;
inline constexpr std::uint32_t kGpuSceneInstanceSize = 160;
inline constexpr std::uint32_t kGpuVisibilityRecordSize = 64;
inline constexpr std::uint32_t kGpuFamilyRecordSize = 160;
inline constexpr std::uint32_t kGpuLightRecordSize = 80;
inline constexpr std::uint32_t kGpuEnvironmentSize = 112;
inline constexpr std::uint32_t kGpuGBufferPixelSize = 80;
// The mirrored G-buffer is rendered as five 16-byte color attachments and
// interleaved into the reflected pixel record on readback. The fifth carries
// the reactive mask: how much of a pixel a transparent effect decides, which
// an upscaler needs so it stops reconstructing a particle from history that
// never contained it.
inline constexpr std::uint32_t kSceneGBufferPlaneCount = 5;
inline constexpr std::uint32_t kSceneGBufferPlaneSize = 16;

struct ScenePushConstantsV1
{
    std::uint32_t objectIndex{};
    std::uint32_t firstInstance{};
    // Non-zero when this draw is refractive and must read the snapshot of
    // the colour target instead of shading as an ordinary blended surface.
    // Carried per draw rather than per object because the same mesh can be
    // drawn refractive in one pass and opaque in another.
    std::uint32_t refractive{};
    // Index of refraction, already resolved from the capture or the class
    // default. Zero means neither was usable and the surface shades without
    // a Fresnel split.
    float indexOfRefraction{};
    // blend::BlendMode for this draw, raw so the shader does not depend on the
    // enum definition. Opaque is not a blended pipeline and never reaches the
    // transparent pass, so it also means "this pixel is entirely mine".
    std::uint32_t blend{};
    // The volume a decal projects into, per draw because the same mesh can be
    // a decal in one pass and ordinary blended geometry in another. A range of
    // zero means this draw projects nothing.
    std::uint32_t decalReceiverMask{};
    std::uint32_t decalReference{};
    float decalRange{};
    float decalOrigin[3]{};
    float decalRadius{};
    float decalAxis[3]{0.0f, 0.0f, -1.0f};
    // Index into the frame's texture library, or raster::kNoMaterialTexture
    // when this draw's material has no captured texture. Occupies what was
    // tail padding, so the block does not grow: a push range is a scarce,
    // device-limited resource and this needed no more of it.
    std::uint32_t textureIndex{0xFFFF'FFFFu};
};

static_assert(sizeof(ScenePushConstantsV1) == 64);
static_assert(kSceneGBufferPlaneCount * kSceneGBufferPlaneSize ==
    kGpuGBufferPixelSize);

}
]=])
