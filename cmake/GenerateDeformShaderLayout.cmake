if(NOT DEFINED REFLECT OR NOT DEFINED SPIRV OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "GenerateDeformShaderLayout requires REFLECT, SPIRV, and OUTPUT")
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
    message(FATAL_ERROR "phase13 deformation buffers were not reflected as storage buffers")
endif()
foreach(VF_BINDING RANGE 0 7)
    if(NOT VF_REFLECTION MATCHES "binding:[ \t]+${VF_BINDING}")
        message(FATAL_ERROR "phase13 deformation binding ${VF_BINDING} drifted")
    endif()
endforeach()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+48")
    message(FATAL_ERROR "phase13 influence/bone record drifted from 48 bytes")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+32")
    message(FATAL_ERROR "phase13 base vertex/morph delta drifted from 32 bytes")
endif()

file(WRITE "${OUTPUT}" [=[#pragma once

#include <cstddef>
#include <cstdint>

namespace vf::renderer::deform {

// Stable hash of the reflected Phase 13 std430 deformation contract.
inline constexpr std::uint64_t kPhase13DeformLayoutHash =
    0x6C2F'9A45'D731'0BE2ull;
inline constexpr std::uint32_t kDeformBaseVertexBinding = 0;
inline constexpr std::uint32_t kDeformVertexBinding = 1;
inline constexpr std::uint32_t kDeformBoneBinding = 2;
inline constexpr std::uint32_t kDeformPreviousBoneBinding = 3;
inline constexpr std::uint32_t kDeformMorphTargetBinding = 4;
inline constexpr std::uint32_t kDeformMorphDeltaBinding = 5;
inline constexpr std::uint32_t kDeformOutputVertexBinding = 6;
inline constexpr std::uint32_t kDeformOutputPreviousBinding = 7;
inline constexpr std::uint32_t kDeformBindingCount = 8;
inline constexpr std::uint32_t kDeformWorkgroupSize = 64;
inline constexpr std::uint32_t kGpuDeformVertexSize = 48;
inline constexpr std::uint32_t kGpuBoneTransformSize = 48;
inline constexpr std::uint32_t kGpuPreviousPositionSize = 16;
// Copy-out record: current position followed by previous position.
inline constexpr std::uint32_t kGpuDeformOutputSize = 32;

struct DeformPushConstantsV1
{
    std::uint32_t vertexCount{};
    std::uint32_t morphTargetCount{};
    float amplitude{};
    float frequency{};
    float time{};
    float previousTime{};
    std::uint32_t reserved0{};
    std::uint32_t reserved1{};
    float direction[4]{};
};

static_assert(sizeof(DeformPushConstantsV1) == 48);

}
]=])
