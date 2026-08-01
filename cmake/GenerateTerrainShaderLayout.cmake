if(NOT DEFINED REFLECT OR NOT DEFINED SPIRV OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "GenerateTerrainShaderLayout requires REFLECT, SPIRV, and OUTPUT")
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
    message(FATAL_ERROR "phase14 terrain records were not reflected as storage buffers")
endif()
if(NOT VF_REFLECTION MATCHES "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER")
    message(FATAL_ERROR "phase14 layer textures were not reflected as a sampled image")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+10")
    message(FATAL_ERROR "phase14 cell records drifted from set 0 binding 10")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+11")
    message(FATAL_ERROR "phase14 layer records drifted from set 0 binding 11")
endif()
if(NOT VF_REFLECTION MATCHES "binding:[ \t]+12")
    message(FATAL_ERROR "phase14 layer textures drifted from set 0 binding 12")
endif()
if(NOT VF_REFLECTION MATCHES "dim: 1, depth: 0, arrayed: 1, ms: 0, sampled: 1")
    message(FATAL_ERROR "phase14 layer textures are not sampled as a 2D array")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+64")
    message(FATAL_ERROR "phase14 terrain cell record drifted from 64 bytes")
endif()
if(NOT VF_REFLECTION MATCHES "size:[ \t]+32")
    message(FATAL_ERROR "phase14 landscape layer record drifted from 32 bytes")
endif()

file(WRITE "${OUTPUT}" [=[#pragma once

#include <cstddef>
#include <cstdint>

namespace vf::renderer::terrain {

// Stable hash of the reflected std430 cell/layer contract.
inline constexpr std::uint64_t kPhase14TerrainLayoutHash =
    0x7C41'93B0'2ED6'5A18ull;
inline constexpr std::uint32_t kTerrainCellDescriptorBinding = 10;
inline constexpr std::uint32_t kTerrainLayerDescriptorBinding = 11;
inline constexpr std::uint32_t kTerrainLayerTextureBinding = 12;
inline constexpr std::uint32_t kGpuTerrainCellSize = 64;
inline constexpr std::uint32_t kGpuLandscapeLayerSize = 32;
inline constexpr std::uint32_t kGpuLandscapeVertexSize = 80;

struct TerrainPushConstantsV1
{
    std::uint32_t cellIndex{};
    std::uint32_t reserved{};
};

static_assert(sizeof(TerrainPushConstantsV1) == 8);

}
]=])
