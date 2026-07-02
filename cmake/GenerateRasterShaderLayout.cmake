if(NOT DEFINED REFLECT OR NOT DEFINED SPIRV OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "GenerateRasterShaderLayout requires REFLECT, SPIRV, and OUTPUT")
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
    message(FATAL_ERROR "mesh material descriptor was not reflected as a uniform buffer")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t\r\n]+0")
    message(FATAL_ERROR "mesh material descriptor binding 0 was not reflected")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t\r\n]+16")
    message(FATAL_ERROR "mesh material block size drifted from 16 bytes")
endif()
if(NOT VF_REFLECTION MATCHES "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER")
    message(FATAL_ERROR "mesh base texture was not reflected as a combined sampler")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t\r\n]+1")
    message(FATAL_ERROR "mesh base texture descriptor binding 1 was not reflected")
endif()

file(WRITE "${OUTPUT}" [=[#pragma once

#include <cstddef>
#include <cstdint>

namespace vf::renderer::raster {

// FNV-1a-64 of "phase06.material.set0.binding0.std140.vec4.size16".
inline constexpr std::uint64_t kPhase6ShaderLayoutHash =
    0x9A6E'82D4'514B'7C31ull;
inline constexpr std::uint32_t kMaterialDescriptorSet = 0;
inline constexpr std::uint32_t kMaterialDescriptorBinding = 0;
inline constexpr std::uint32_t kBaseTextureDescriptorBinding = 1;
inline constexpr std::uint32_t kMaterialConstantSize = 16;

struct alignas(16) GpuMaterialConstants
{
    float baseColor[4];
};

static_assert(sizeof(GpuMaterialConstants) == kMaterialConstantSize);
static_assert(offsetof(GpuMaterialConstants, baseColor) == 0);

}
]=])
