if(NOT DEFINED REFLECT OR NOT DEFINED SPIRV OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "GenerateViewShaderLayout requires REFLECT, SPIRV, and OUTPUT")
endif()

execute_process(
    COMMAND "${REFLECT}" --yaml "${SPIRV}"
    RESULT_VARIABLE VF_REFLECT_RESULT
    OUTPUT_VARIABLE VF_REFLECTION
    ERROR_VARIABLE VF_REFLECT_ERROR)
if(NOT VF_REFLECT_RESULT EQUAL 0)
    message(FATAL_ERROR "SPIR-V reflection failed: ${VF_REFLECT_ERROR}")
endif()
if(NOT VF_REFLECTION MATCHES "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER")
    message(FATAL_ERROR "phase10 view record was not reflected as a uniform buffer")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+6")
    message(FATAL_ERROR "phase10 view record drifted from set 0 binding 6")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+240")
    message(FATAL_ERROR "phase10 view record drifted from 240 bytes")
endif()

file(WRITE "${OUTPUT}" [=[#pragma once

#include <cstddef>
#include <cstdint>

namespace vf::renderer::view {

// FNV-1a-64 of the reflected phase10 std140 view-record contract.
inline constexpr std::uint64_t kPhase10ViewLayoutHash =
    0xC34D'6F6A'B1E8'B527ull;
inline constexpr std::uint32_t kViewDescriptorBinding = 6;
inline constexpr std::uint32_t kGpuViewConstantSize = 240;
inline constexpr std::uint32_t kGpuViewProjectionEnabled = 1;

struct alignas(16) GpuViewConstantsV1
{
    float viewProjectionRows[16]{};
    float previousViewProjectionRows[16]{};
    float unjitteredViewProjectionRows[16]{};
    float clipAndJitter[4]{};
    float viewport[4]{};
    std::uint32_t identifiers[4]{};
};

static_assert(sizeof(GpuViewConstantsV1) == kGpuViewConstantSize);
static_assert(offsetof(GpuViewConstantsV1, viewProjectionRows) == 0);
static_assert(offsetof(GpuViewConstantsV1,
    previousViewProjectionRows) == 64);
static_assert(offsetof(GpuViewConstantsV1,
    unjitteredViewProjectionRows) == 128);
static_assert(offsetof(GpuViewConstantsV1, clipAndJitter) == 192);
static_assert(offsetof(GpuViewConstantsV1, viewport) == 208);
static_assert(offsetof(GpuViewConstantsV1, identifiers) == 224);

}
]=])
