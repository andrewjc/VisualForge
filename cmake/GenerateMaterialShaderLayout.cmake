if(NOT DEFINED REFLECT OR NOT DEFINED SPIRV OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "GenerateMaterialShaderLayout requires REFLECT, SPIRV, and OUTPUT")
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
    message(FATAL_ERROR "phase09 material records were not reflected as storage buffers")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+64")
    message(FATAL_ERROR "phase09 static material record drifted from 64 bytes")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+48")
    message(FATAL_ERROR "phase09 dynamic material record drifted from 48 bytes")
endif()

file(WRITE "${OUTPUT}" [=[#pragma once

#include <cstddef>
#include <cstdint>

namespace vf::renderer::material {

// FNV-1a-64 of the reflected phase09 std430 static/dynamic record contract.
inline constexpr std::uint64_t kPhase9MaterialLayoutHash =
    0xF97A'3578'9BC8'4031ull;
inline constexpr std::uint32_t kPhase9StaticMaterialSize = 64;
inline constexpr std::uint32_t kPhase9DynamicMaterialSize = 48;

struct alignas(16) GpuMaterialStaticV1
{
    std::uint64_t materialId{};
    std::uint32_t staticRevision{};
    std::uint32_t flags{};
    std::uint32_t textureIndices[4]{};
    float uvScale[2]{1.0f, 1.0f};
    float uvOffset[2]{};
    float specularColor[3]{0.04f, 0.04f, 0.04f};
    float reserved{};

    friend bool operator==(const GpuMaterialStaticV1&,
        const GpuMaterialStaticV1&) = default;
};

struct alignas(16) GpuMaterialDynamicV1
{
    std::uint64_t materialId{};
    std::uint32_t materialRevision{};
    std::uint32_t staticRevision{};
    float alpha{1.0f};
    float smoothness{0.5f};
    float specularScale{1.0f};
    float fresnelPower{5.0f};
    float alphaCutoff{0.5f};
    std::uint32_t transferVersion{1};
    std::uint32_t reserved[2]{};

    friend bool operator==(const GpuMaterialDynamicV1&,
        const GpuMaterialDynamicV1&) = default;
};

static_assert(sizeof(GpuMaterialStaticV1) == kPhase9StaticMaterialSize);
static_assert(offsetof(GpuMaterialStaticV1, materialId) == 0);
static_assert(offsetof(GpuMaterialStaticV1, textureIndices) == 16);
static_assert(offsetof(GpuMaterialStaticV1, uvScale) == 32);
static_assert(offsetof(GpuMaterialStaticV1, uvOffset) == 40);
static_assert(offsetof(GpuMaterialStaticV1, specularColor) == 48);
static_assert(sizeof(GpuMaterialDynamicV1) == kPhase9DynamicMaterialSize);
static_assert(offsetof(GpuMaterialDynamicV1, materialRevision) == 8);
static_assert(offsetof(GpuMaterialDynamicV1, alpha) == 16);
static_assert(offsetof(GpuMaterialDynamicV1, alphaCutoff) == 32);

}
]=])
