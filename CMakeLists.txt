cmake_minimum_required(VERSION 3.20)
project(VisualForge CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(EXTERNAL ${CMAKE_CURRENT_SOURCE_DIR}/external)

option(VF_BUILD_TESTS "Build VisualForge automated tests" OFF)
option(VF_BUILD_RENDERER_BACKEND "Build the Vulkan renderer backend DLL" ON)
set(VF_GAME_ROOT "" CACHE PATH "Fallout 4 install used by local contract tests")

find_program(VF_GLSLC_EXECUTABLE
    NAMES glslc.exe glslc
    HINTS "$ENV{VULKAN_SDK}/Bin"
    REQUIRED
)
find_program(VF_SPIRV_REFLECT_EXECUTABLE
    NAMES spirv-reflect.exe spirv-reflect
    HINTS "$ENV{VULKAN_SDK}/Bin"
    REQUIRED
)

# Every shared shader header, listed once and depended on by every shader.
# The per-shader DEPENDS lists were hand-maintained, and a missing include
# does not fail the build -- it silently keeps the previous SPIR-V, so an edit
# to a shared header appears to have no effect and a mutation run against one
# reports a false pass. Depending on all of them rebuilds a little more than
# strictly necessary and cannot be silently wrong, which is the right trade
# for a list nobody remembers to update. CONFIGURE_DEPENDS so a newly added
# header is picked up without a manual reconfigure.
file(GLOB VF_SHADER_INCLUDES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*/*.glsl")

set(VF_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(VF_PHASE6_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase06")
set(VF_PHASE10_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase10")
set(VF_PHASE11_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase11")
set(VF_PHASE6_SHADER_OUTPUTS)
foreach(VF_SHADER IN ITEMS mesh.vert mesh.frag tone_map.vert tone_map.frag)
    string(REPLACE "." "_" VF_SHADER_SYMBOL_SUFFIX "${VF_SHADER}")
    set(VF_SHADER_SPV "${VF_GENERATED_DIR}/${VF_SHADER}.spv")
    set(VF_SHADER_INC "${VF_GENERATED_DIR}/${VF_SHADER}.spv.inc")
    add_custom_command(
        OUTPUT "${VF_SHADER_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VF_GENERATED_DIR}"
        COMMAND "${VF_GLSLC_EXECUTABLE}"
            --target-env=vulkan1.3
            -O
            -I"${VF_PHASE10_SHADER_DIR}"
            "${VF_PHASE6_SHADER_DIR}/${VF_SHADER}"
            -o "${VF_SHADER_SPV}"
        DEPENDS
            "${VF_PHASE6_SHADER_DIR}/${VF_SHADER}"
            ${VF_SHADER_INCLUDES}
            "${VF_PHASE10_SHADER_DIR}/view_transform.glsl"
            # Hand-maintained, and that is a hazard worth naming: an include
            # missing from this list does not fail the build, it silently
            # keeps the previous SPIR-V, so an edit to a shared shader header
            # appears to have no effect and a mutation run against it reports
            # a false pass.
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase23/post.glsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${VF_SHADER_INC}"
        COMMAND ${CMAKE_COMMAND}
            "-DINPUT=${VF_SHADER_SPV}"
            "-DOUTPUT=${VF_SHADER_INC}"
            "-DSYMBOL=kPhase6_${VF_SHADER_SYMBOL_SUFFIX}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS
            "${VF_SHADER_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        VERBATIM
    )
    list(APPEND VF_PHASE6_SHADER_OUTPUTS "${VF_SHADER_INC}")
endforeach()

set(VF_RASTER_LAYOUT_HEADER
    "${VF_GENERATED_DIR}/RasterShaderLayout.generated.h")
add_custom_command(
    OUTPUT "${VF_RASTER_LAYOUT_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        "-DREFLECT=${VF_SPIRV_REFLECT_EXECUTABLE}"
        "-DSPIRV=${VF_GENERATED_DIR}/mesh.frag.spv"
        "-DOUTPUT=${VF_RASTER_LAYOUT_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateRasterShaderLayout.cmake"
    DEPENDS
        "${VF_GENERATED_DIR}/mesh.frag.spv"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateRasterShaderLayout.cmake"
    VERBATIM
)
list(APPEND VF_PHASE6_SHADER_OUTPUTS "${VF_RASTER_LAYOUT_HEADER}")
add_custom_target(vf_phase6_shaders DEPENDS ${VF_PHASE6_SHADER_OUTPUTS})

set(VF_PHASE9_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase09")
set(VF_PHASE9_LAYOUT_SPV "${VF_GENERATED_DIR}/material_layout.comp.spv")
set(VF_MATERIAL_LAYOUT_HEADER
    "${VF_GENERATED_DIR}/MaterialShaderLayout.generated.h")
set(VF_PHASE9_SHADER_OUTPUTS "${VF_MATERIAL_LAYOUT_HEADER}")
add_custom_command(
    OUTPUT "${VF_PHASE9_LAYOUT_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VF_GENERATED_DIR}"
    COMMAND "${VF_GLSLC_EXECUTABLE}"
        --target-env=vulkan1.3
        -O
        "${VF_PHASE9_SHADER_DIR}/material_layout.comp"
        -o "${VF_PHASE9_LAYOUT_SPV}"
    DEPENDS "${VF_PHASE9_SHADER_DIR}/material_layout.comp"
        ${VF_SHADER_INCLUDES}
    VERBATIM
)
add_custom_command(
    OUTPUT "${VF_MATERIAL_LAYOUT_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        "-DREFLECT=${VF_SPIRV_REFLECT_EXECUTABLE}"
        "-DSPIRV=${VF_PHASE9_LAYOUT_SPV}"
        "-DOUTPUT=${VF_MATERIAL_LAYOUT_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateMaterialShaderLayout.cmake"
    DEPENDS
        "${VF_PHASE9_LAYOUT_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateMaterialShaderLayout.cmake"
    VERBATIM
)
set(VF_PHASE9_MATERIAL_SPV "${VF_GENERATED_DIR}/material.frag.spv")
set(VF_PHASE9_MATERIAL_INC "${VF_GENERATED_DIR}/material.frag.spv.inc")
add_custom_command(
    OUTPUT "${VF_PHASE9_MATERIAL_SPV}"
    COMMAND "${VF_GLSLC_EXECUTABLE}"
        --target-env=vulkan1.3
        -O
        -I"${VF_PHASE9_SHADER_DIR}"
        "${VF_PHASE9_SHADER_DIR}/material.frag"
        -o "${VF_PHASE9_MATERIAL_SPV}"
    DEPENDS
        "${VF_PHASE9_SHADER_DIR}/material.frag"
        ${VF_SHADER_INCLUDES}
    VERBATIM
)
add_custom_command(
    OUTPUT "${VF_PHASE9_MATERIAL_INC}"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${VF_PHASE9_MATERIAL_SPV}"
        "-DOUTPUT=${VF_PHASE9_MATERIAL_INC}"
        "-DSYMBOL=kPhase9_material_frag"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS
        "${VF_PHASE9_MATERIAL_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    VERBATIM
)
list(APPEND VF_PHASE9_SHADER_OUTPUTS "${VF_PHASE9_MATERIAL_INC}")
add_custom_target(vf_phase9_shaders DEPENDS ${VF_PHASE9_SHADER_OUTPUTS})

set(VF_PHASE10_LAYOUT_SPV "${VF_GENERATED_DIR}/view_layout.comp.spv")
set(VF_VIEW_LAYOUT_HEADER
    "${VF_GENERATED_DIR}/ViewShaderLayout.generated.h")
add_custom_command(
    OUTPUT "${VF_PHASE10_LAYOUT_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VF_GENERATED_DIR}"
    COMMAND "${VF_GLSLC_EXECUTABLE}"
        --target-env=vulkan1.3
        -O
        -I"${VF_PHASE10_SHADER_DIR}"
        "${VF_PHASE10_SHADER_DIR}/view_layout.comp"
        -o "${VF_PHASE10_LAYOUT_SPV}"
    DEPENDS
        "${VF_PHASE10_SHADER_DIR}/view_layout.comp"
        ${VF_SHADER_INCLUDES}
    VERBATIM
)
add_custom_command(
    OUTPUT "${VF_VIEW_LAYOUT_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        "-DREFLECT=${VF_SPIRV_REFLECT_EXECUTABLE}"
        "-DSPIRV=${VF_PHASE10_LAYOUT_SPV}"
        "-DOUTPUT=${VF_VIEW_LAYOUT_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateViewShaderLayout.cmake"
    DEPENDS
        "${VF_PHASE10_LAYOUT_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateViewShaderLayout.cmake"
    VERBATIM
)
add_custom_target(vf_phase10_shaders DEPENDS "${VF_VIEW_LAYOUT_HEADER}")

set(VF_PHASE11_LAYOUT_SPV "${VF_GENERATED_DIR}/scene_layout.comp.spv")
set(VF_SCENE_LAYOUT_HEADER
    "${VF_GENERATED_DIR}/SceneShaderLayout.generated.h")
set(VF_PHASE11_SHADER_OUTPUTS "${VF_SCENE_LAYOUT_HEADER}")
add_custom_command(
    OUTPUT "${VF_PHASE11_LAYOUT_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VF_GENERATED_DIR}"
    COMMAND "${VF_GLSLC_EXECUTABLE}"
        --target-env=vulkan1.3
        -O
        -I"${VF_PHASE11_SHADER_DIR}"
        "${VF_PHASE11_SHADER_DIR}/scene_layout.comp"
        -o "${VF_PHASE11_LAYOUT_SPV}"
    DEPENDS
        "${VF_PHASE11_SHADER_DIR}/scene_layout.comp"
        ${VF_SHADER_INCLUDES}
    VERBATIM
)
add_custom_command(
    OUTPUT "${VF_SCENE_LAYOUT_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        "-DREFLECT=${VF_SPIRV_REFLECT_EXECUTABLE}"
        "-DSPIRV=${VF_PHASE11_LAYOUT_SPV}"
        "-DOUTPUT=${VF_SCENE_LAYOUT_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSceneShaderLayout.cmake"
    DEPENDS
        "${VF_PHASE11_LAYOUT_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSceneShaderLayout.cmake"
    VERBATIM
)
foreach(VF_SHADER IN ITEMS scene.vert scene.frag)
    string(REPLACE "." "_" VF_SHADER_SYMBOL_SUFFIX "${VF_SHADER}")
    set(VF_SHADER_SPV "${VF_GENERATED_DIR}/${VF_SHADER}.spv")
    set(VF_SHADER_INC "${VF_GENERATED_DIR}/${VF_SHADER}.spv.inc")
    add_custom_command(
        OUTPUT "${VF_SHADER_SPV}"
        COMMAND "${VF_GLSLC_EXECUTABLE}"
            --target-env=vulkan1.3
            -O
            -I"${VF_PHASE11_SHADER_DIR}"
            -I"${VF_PHASE10_SHADER_DIR}"
            "${VF_PHASE11_SHADER_DIR}/${VF_SHADER}"
            -o "${VF_SHADER_SPV}"
        DEPENDS
            "${VF_PHASE11_SHADER_DIR}/${VF_SHADER}"
            ${VF_SHADER_INCLUDES}
            "${VF_PHASE11_SHADER_DIR}/scene_layout.glsl"
            "${VF_PHASE10_SHADER_DIR}/view_transform.glsl"
            # scene.frag sources its stored opacity from the shared coverage
            # rule. Without this the opaque shader would keep a stale copy of
            # a rule the alpha shaders had already moved on from.
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase15/alpha_coverage.glsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${VF_SHADER_INC}"
        COMMAND ${CMAKE_COMMAND}
            "-DINPUT=${VF_SHADER_SPV}"
            "-DOUTPUT=${VF_SHADER_INC}"
            "-DSYMBOL=kPhase11_${VF_SHADER_SYMBOL_SUFFIX}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS
            "${VF_SHADER_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        VERBATIM
    )
    list(APPEND VF_PHASE11_SHADER_OUTPUTS "${VF_SHADER_INC}")
endforeach()
add_custom_target(vf_phase11_shaders DEPENDS ${VF_PHASE11_SHADER_OUTPUTS})

set(VF_PHASE13_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase13")
set(VF_PHASE13_DEFORM_SPV "${VF_GENERATED_DIR}/deform.comp.spv")
set(VF_PHASE13_DEFORM_INC "${VF_GENERATED_DIR}/deform.comp.spv.inc")
set(VF_DEFORM_LAYOUT_HEADER
    "${VF_GENERATED_DIR}/DeformShaderLayout.generated.h")
add_custom_command(
    OUTPUT "${VF_PHASE13_DEFORM_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VF_GENERATED_DIR}"
    COMMAND "${VF_GLSLC_EXECUTABLE}"
        --target-env=vulkan1.3
        -O
        -I"${VF_PHASE13_SHADER_DIR}"
        "${VF_PHASE13_SHADER_DIR}/deform.comp"
        -o "${VF_PHASE13_DEFORM_SPV}"
    DEPENDS
        "${VF_PHASE13_SHADER_DIR}/deform.comp"
        ${VF_SHADER_INCLUDES}
    VERBATIM
)
add_custom_command(
    OUTPUT "${VF_DEFORM_LAYOUT_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        "-DREFLECT=${VF_SPIRV_REFLECT_EXECUTABLE}"
        "-DSPIRV=${VF_PHASE13_DEFORM_SPV}"
        "-DOUTPUT=${VF_DEFORM_LAYOUT_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateDeformShaderLayout.cmake"
    DEPENDS
        "${VF_PHASE13_DEFORM_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateDeformShaderLayout.cmake"
    VERBATIM
)
add_custom_command(
    OUTPUT "${VF_PHASE13_DEFORM_INC}"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${VF_PHASE13_DEFORM_SPV}"
        "-DOUTPUT=${VF_PHASE13_DEFORM_INC}"
        "-DSYMBOL=kPhase13_deform_comp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS
        "${VF_PHASE13_DEFORM_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    VERBATIM
)
add_custom_target(vf_phase13_shaders DEPENDS
    "${VF_DEFORM_LAYOUT_HEADER}"
    "${VF_PHASE13_DEFORM_INC}")

# Phase 20's temporal accumulation. A pass over pixels rather than over
# geometry, so it is compute like phase 13 rather than part of the raster
# pipeline, and it is built the same way.
set(VF_PHASE20_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase20")
set(VF_PHASE20_ACCUMULATE_SPV "${VF_GENERATED_DIR}/accumulate.comp.spv")
set(VF_PHASE20_ACCUMULATE_INC "${VF_GENERATED_DIR}/accumulate.comp.spv.inc")
add_custom_command(
    OUTPUT "${VF_PHASE20_ACCUMULATE_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VF_GENERATED_DIR}"
    COMMAND "${VF_GLSLC_EXECUTABLE}"
        --target-env=vulkan1.3
        -O
        -I"${VF_PHASE20_SHADER_DIR}"
        "${VF_PHASE20_SHADER_DIR}/accumulate.comp"
        -o "${VF_PHASE20_ACCUMULATE_SPV}"
    DEPENDS
        "${VF_PHASE20_SHADER_DIR}/accumulate.comp"
        ${VF_SHADER_INCLUDES}
    VERBATIM
)
add_custom_command(
    OUTPUT "${VF_PHASE20_ACCUMULATE_INC}"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${VF_PHASE20_ACCUMULATE_SPV}"
        "-DOUTPUT=${VF_PHASE20_ACCUMULATE_INC}"
        "-DSYMBOL=kPhase20_accumulate_comp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS
        "${VF_PHASE20_ACCUMULATE_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    VERBATIM
)
add_custom_target(vf_phase20_shaders DEPENDS "${VF_PHASE20_ACCUMULATE_INC}")

set(VF_PHASE14_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase14")
set(VF_PHASE14_LAYOUT_SPV "${VF_GENERATED_DIR}/terrain_layout.comp.spv")
set(VF_TERRAIN_LAYOUT_HEADER
    "${VF_GENERATED_DIR}/TerrainShaderLayout.generated.h")
set(VF_PHASE14_SHADER_OUTPUTS "${VF_TERRAIN_LAYOUT_HEADER}")
add_custom_command(
    OUTPUT "${VF_PHASE14_LAYOUT_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VF_GENERATED_DIR}"
    COMMAND "${VF_GLSLC_EXECUTABLE}"
        --target-env=vulkan1.3
        -O
        -I"${VF_PHASE14_SHADER_DIR}"
        "${VF_PHASE14_SHADER_DIR}/terrain_layout.comp"
        -o "${VF_PHASE14_LAYOUT_SPV}"
    DEPENDS
        "${VF_PHASE14_SHADER_DIR}/terrain_layout.comp"
        ${VF_SHADER_INCLUDES}
    VERBATIM
)
add_custom_command(
    OUTPUT "${VF_TERRAIN_LAYOUT_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        "-DREFLECT=${VF_SPIRV_REFLECT_EXECUTABLE}"
        "-DSPIRV=${VF_PHASE14_LAYOUT_SPV}"
        "-DOUTPUT=${VF_TERRAIN_LAYOUT_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTerrainShaderLayout.cmake"
    DEPENDS
        "${VF_PHASE14_LAYOUT_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTerrainShaderLayout.cmake"
    VERBATIM
)
foreach(VF_SHADER IN ITEMS terrain.vert terrain.frag)
    string(REPLACE "." "_" VF_SHADER_SYMBOL_SUFFIX "${VF_SHADER}")
    set(VF_SHADER_SPV "${VF_GENERATED_DIR}/${VF_SHADER}.spv")
    set(VF_SHADER_INC "${VF_GENERATED_DIR}/${VF_SHADER}.spv.inc")
    add_custom_command(
        OUTPUT "${VF_SHADER_SPV}"
        COMMAND "${VF_GLSLC_EXECUTABLE}"
            --target-env=vulkan1.3
            -O
            -I"${VF_PHASE14_SHADER_DIR}"
            -I"${VF_PHASE10_SHADER_DIR}"
            "${VF_PHASE14_SHADER_DIR}/${VF_SHADER}"
            -o "${VF_SHADER_SPV}"
        DEPENDS
            "${VF_PHASE14_SHADER_DIR}/${VF_SHADER}"
            ${VF_SHADER_INCLUDES}
            "${VF_PHASE14_SHADER_DIR}/terrain_layout.glsl"
            "${VF_PHASE10_SHADER_DIR}/view_transform.glsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${VF_SHADER_INC}"
        COMMAND ${CMAKE_COMMAND}
            "-DINPUT=${VF_SHADER_SPV}"
            "-DOUTPUT=${VF_SHADER_INC}"
            "-DSYMBOL=kPhase14_${VF_SHADER_SYMBOL_SUFFIX}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS
            "${VF_SHADER_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        VERBATIM
    )
    list(APPEND VF_PHASE14_SHADER_OUTPUTS "${VF_SHADER_INC}")
endforeach()
add_custom_target(vf_phase14_shaders DEPENDS ${VF_PHASE14_SHADER_OUTPUTS})

set(VF_PHASE15_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase15")
set(VF_PHASE15_SHADER_OUTPUTS)
foreach(VF_SHADER IN ITEMS alpha_scene.frag alpha_depth.frag)
    string(REPLACE "." "_" VF_SHADER_SYMBOL_SUFFIX "${VF_SHADER}")
    set(VF_SHADER_SPV "${VF_GENERATED_DIR}/${VF_SHADER}.spv")
    set(VF_SHADER_INC "${VF_GENERATED_DIR}/${VF_SHADER}.spv.inc")
    add_custom_command(
        OUTPUT "${VF_SHADER_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VF_GENERATED_DIR}"
        COMMAND "${VF_GLSLC_EXECUTABLE}"
            --target-env=vulkan1.3
            -O
            -I"${VF_PHASE15_SHADER_DIR}"
            -I"${VF_PHASE11_SHADER_DIR}"
            "${VF_PHASE15_SHADER_DIR}/${VF_SHADER}"
            -o "${VF_SHADER_SPV}"
        DEPENDS
            "${VF_PHASE15_SHADER_DIR}/${VF_SHADER}"
            ${VF_SHADER_INCLUDES}
            "${VF_PHASE15_SHADER_DIR}/alpha_coverage.glsl"
            "${VF_PHASE11_SHADER_DIR}/scene_layout.glsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${VF_SHADER_INC}"
        COMMAND ${CMAKE_COMMAND}
            "-DINPUT=${VF_SHADER_SPV}"
            "-DOUTPUT=${VF_SHADER_INC}"
            "-DSYMBOL=kPhase15_${VF_SHADER_SYMBOL_SUFFIX}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS
            "${VF_SHADER_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        VERBATIM
    )
    list(APPEND VF_PHASE15_SHADER_OUTPUTS "${VF_SHADER_INC}")
endforeach()
add_custom_target(vf_phase15_shaders DEPENDS ${VF_PHASE15_SHADER_OUTPUTS})

set(VF_PHASE16_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase16")
set(VF_PHASE16_SHADER_OUTPUTS)
foreach(VF_SHADER IN ITEMS family_scene.frag)
    string(REPLACE "." "_" VF_SHADER_SYMBOL_SUFFIX "${VF_SHADER}")
    set(VF_SHADER_SPV "${VF_GENERATED_DIR}/${VF_SHADER}.spv")
    set(VF_SHADER_INC "${VF_GENERATED_DIR}/${VF_SHADER}.spv.inc")
    add_custom_command(
        OUTPUT "${VF_SHADER_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VF_GENERATED_DIR}"
        COMMAND "${VF_GLSLC_EXECUTABLE}"
            --target-env=vulkan1.3
            -O
            -I"${VF_PHASE16_SHADER_DIR}"
            -I"${VF_PHASE15_SHADER_DIR}"
            -I"${VF_PHASE11_SHADER_DIR}"
            "${VF_PHASE16_SHADER_DIR}/${VF_SHADER}"
            -o "${VF_SHADER_SPV}"
        DEPENDS
            "${VF_PHASE16_SHADER_DIR}/${VF_SHADER}"
            ${VF_SHADER_INCLUDES}
            "${VF_PHASE16_SHADER_DIR}/family_shading.glsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase17/lighting.glsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase19/reflection.glsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase20/indirect.glsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase19/reflection.glsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase20/indirect.glsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase20/indirect.glsl"
            "${VF_PHASE15_SHADER_DIR}/alpha_coverage.glsl"
            "${VF_PHASE11_SHADER_DIR}/scene_layout.glsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${VF_SHADER_INC}"
        COMMAND ${CMAKE_COMMAND}
            "-DINPUT=${VF_SHADER_SPV}"
            "-DOUTPUT=${VF_SHADER_INC}"
            "-DSYMBOL=kPhase16_${VF_SHADER_SYMBOL_SUFFIX}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS
            "${VF_SHADER_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        VERBATIM
    )
    list(APPEND VF_PHASE16_SHADER_OUTPUTS "${VF_SHADER_INC}")
endforeach()
# The ray-query variant of the same source. Ray query is a SPIR-V capability
# of the module, not a runtime branch: a device that never enabled the
# extension cannot create a pipeline from this module at all, so the shader is
# compiled twice and the backend picks by what the device actually enabled.
# SPIR-V 1.4 is the floor for GL_EXT_ray_query.
set(VF_PHASE18_SHADER_SPV "${VF_GENERATED_DIR}/family_scene_rq.frag.spv")
set(VF_PHASE18_SHADER_INC "${VF_GENERATED_DIR}/family_scene_rq.frag.spv.inc")
add_custom_command(
    OUTPUT "${VF_PHASE18_SHADER_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VF_GENERATED_DIR}"
    COMMAND "${VF_GLSLC_EXECUTABLE}"
        --target-env=vulkan1.3
        --target-spv=spv1.4
        -DVF_RAY_QUERY=1
        -O
        -I"${VF_PHASE16_SHADER_DIR}"
        -I"${VF_PHASE15_SHADER_DIR}"
        -I"${VF_PHASE11_SHADER_DIR}"
        "${VF_PHASE16_SHADER_DIR}/family_scene.frag"
        -o "${VF_PHASE18_SHADER_SPV}"
    DEPENDS
        "${VF_PHASE16_SHADER_DIR}/family_scene.frag"
        ${VF_SHADER_INCLUDES}
        "${VF_PHASE15_SHADER_DIR}/alpha_coverage.glsl"
        "${VF_PHASE11_SHADER_DIR}/scene_layout.glsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${VF_PHASE18_SHADER_INC}"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${VF_PHASE18_SHADER_SPV}"
        "-DOUTPUT=${VF_PHASE18_SHADER_INC}"
        "-DSYMBOL=kPhase18_family_scene_rq_frag"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS
        "${VF_PHASE18_SHADER_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    VERBATIM
)
list(APPEND VF_PHASE16_SHADER_OUTPUTS "${VF_PHASE18_SHADER_INC}")

add_custom_target(vf_phase16_shaders DEPENDS ${VF_PHASE16_SHADER_OUTPUTS})

add_library(vf_core STATIC
    src/renderer_core/EngineMesh.cpp
    src/renderer_core/EngineMaterial.cpp
    src/renderer_core/EngineMaterialFamily.cpp
    src/renderer_core/EngineLighting.cpp
    src/renderer_core/EngineAcceleration.cpp
    src/renderer_core/EngineDrawStream.cpp
    src/renderer_core/EngineImageSpace.cpp
    src/renderer_core/EngineArchive.cpp
    src/renderer_core/EngineMaterialFile.cpp
    src/renderer_core/EngineInflate.cpp
    src/renderer_core/EngineIndirect.cpp
    src/renderer_core/EngineTransparency.cpp
    src/renderer_core/EngineBridgeOrder.cpp
    src/renderer_core/EnginePostChain.cpp
    src/renderer_core/EnginePresentation.cpp
    src/renderer_core/EngineWater.cpp
    src/renderer_core/EngineReflection.cpp
    src/renderer_core/CameraStateScan.cpp
    src/renderer_core/EngineDeformation.cpp
    src/renderer_core/EngineScene.cpp
    src/renderer_core/EngineTakeover.cpp
    src/renderer_core/EngineSuppression.cpp
    src/renderer_core/EngineTerrain.cpp
    src/renderer_core/EngineVisibility.cpp
    src/renderer_core/SceneDatabase.cpp
    src/renderer_core/EngineTexture.cpp
    src/renderer_core/ShaderReflection.cpp
    src/renderer_core/EngineEnvironmentSource.cpp
    src/renderer_core/EngineView.cpp
    src/renderer_core/EngineVertex.cpp
    src/renderer_core/ResourceRegistry.cpp
    src/renderer_core/TextureResidency.cpp
    src/renderer_core/FrameGraph.cpp
    src/renderer_core/RasterGolden.cpp
    src/renderer_core/RasterPacket.cpp
    src/renderer_host/AddressLibrary.cpp
    src/renderer_host/BridgeContract.cpp
    src/renderer_host/BridgeState.cpp
    src/renderer_host/BackendContract.cpp
    src/renderer_host/CaptureControl.cpp
    src/renderer_host/GraphicsStateContract.cpp
    src/renderer_host/BackendHost.cpp
    src/renderer_api/StableId.cpp
    src/renderer_trace/Crc32.cpp
    src/renderer_trace/FrameCapture.cpp
    src/renderer_trace/TraceCodec.cpp
    src/renderer_host/BuildGate.cpp
    src/renderer_host/ResourceManagerContract.cpp
    src/renderer_host/HookManifest.cpp
    src/renderer_host/HookTransaction.cpp
    src/renderer_host/RendererBootstrap.cpp
    src/renderer_host/RendererHealth.cpp
    src/renderer_host/RendererState.cpp
)

add_dependencies(vf_core
    vf_phase6_shaders
    vf_phase9_shaders
    vf_phase10_shaders
    vf_phase11_shaders
    vf_phase13_shaders
    vf_phase14_shaders
    vf_phase15_shaders
    vf_phase16_shaders
    vf_phase20_shaders)
target_include_directories(vf_core PUBLIC src "${VF_GENERATED_DIR}")

if(MSVC)
    target_compile_options(vf_core PRIVATE /W4 /WX)
endif()

add_executable(vf_packet_replay
    src/tools/PacketReplayMain.cpp
)
target_link_libraries(vf_packet_replay PRIVATE vf_core)
target_include_directories(vf_packet_replay PRIVATE src)
if(MSVC)
    target_compile_options(vf_packet_replay PRIVATE /W4 /WX)
endif()

# Offline static analysis of the target executable. Reads a file on disk and
# never attaches to a running game.
add_executable(vf_engine_analysis
    src/tools/EngineAnalysisMain.cpp
)
target_include_directories(vf_engine_analysis PRIVATE src)
if(MSVC)
    target_compile_options(vf_engine_analysis PRIVATE /W4 /WX)
endif()

add_library(vf_platform_win STATIC
    src/renderer_host/D3D11TextureContract.cpp
    src/renderer_host/D3D11InteropBridge.cpp
    src/renderer_host/WindowsBackendModule.cpp
    src/renderer_host/WindowsBuildProbe.cpp
    src/renderer_host/WindowsStartupProbe.cpp
)

target_include_directories(vf_platform_win PUBLIC src)
target_link_libraries(vf_platform_win PUBLIC vf_core bcrypt)

target_link_libraries(vf_packet_replay PRIVATE
    vf_platform_win
    d3d11
    dxgi
)

if(MSVC)
    target_compile_options(vf_platform_win PRIVATE /W4 /WX)
endif()

add_executable(vf_backend_probe
    src/tools/BackendProbeMain.cpp
)
target_include_directories(vf_backend_probe PRIVATE src)
target_link_libraries(vf_backend_probe PRIVATE
    vf_core
    vf_platform_win
    d3d11
    dxgi
)
if(MSVC)
    target_compile_options(vf_backend_probe PRIVATE /W4 /WX)
endif()

add_executable(vf_bridge_probe
    src/tools/BridgeProbeMain.cpp
)
target_include_directories(vf_bridge_probe PRIVATE src)
target_link_libraries(vf_bridge_probe PRIVATE
    vf_core
    vf_platform_win
    d3d11
    dxgi
)
if(MSVC)
    target_compile_options(vf_bridge_probe PRIVATE /W4 /WX)
endif()

if(VF_BUILD_RENDERER_BACKEND)
    find_package(Vulkan REQUIRED)
    add_library(VisualForgeRenderer SHARED
        src/renderer_backend/BackendEntry.cpp
        src/renderer_backend/VulkanInteropBridge.cpp
        src/renderer_backend/VulkanProbe.cpp
        src/renderer_backend/VulkanRasterRenderer.cpp
    )
    target_include_directories(VisualForgeRenderer PRIVATE src)
    target_include_directories(VisualForgeRenderer PRIVATE "${VF_GENERATED_DIR}")
    add_dependencies(VisualForgeRenderer
        vf_phase6_shaders vf_phase14_shaders vf_phase15_shaders
        vf_phase16_shaders)
    target_link_libraries(VisualForgeRenderer PRIVATE Vulkan::Vulkan vf_core)
    target_compile_definitions(VisualForgeRenderer PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        VK_USE_PLATFORM_WIN32_KHR
    )
    if(MSVC)
        target_compile_options(VisualForgeRenderer PRIVATE /W4 /WX)
    endif()
    set_target_properties(VisualForgeRenderer PROPERTIES
        OUTPUT_NAME "VisualForgeRenderer"
        PREFIX ""
    )
endif()

add_library(VisualForge SHARED
    src/main.cpp
    src/CaptureRequests.cpp
    src/EngineCameraCapture.cpp
    src/EngineDrawCapture.cpp
    src/EngineMeshExtractor.cpp
    src/EngineMeshCapture.cpp
    src/EngineTextureCapture.cpp
    src/EngineTextureResidency.cpp
    src/Log.cpp
    src/Config.cpp
    src/CrashLog.cpp
    src/PatternScan.cpp
    src/EngineSettings.cpp
    src/IniWriter.cpp
    src/DepthCapture.cpp
    src/EngineWorldSuppression.cpp
    src/IntroSkip.cpp
    src/Lut.cpp
    src/PostProcess.cpp
    src/RendererBackendProbe.cpp
    src/RendererObservation.cpp
    src/D3D11Hook.cpp
    src/Overlay.cpp
    ${EXTERNAL}/minhook/src/buffer.c
    ${EXTERNAL}/minhook/src/hook.c
    ${EXTERNAL}/minhook/src/trampoline.c
    ${EXTERNAL}/minhook/src/hde/hde64.c
    ${EXTERNAL}/imgui/imgui.cpp
    ${EXTERNAL}/imgui/imgui_draw.cpp
    ${EXTERNAL}/imgui/imgui_tables.cpp
    ${EXTERNAL}/imgui/imgui_widgets.cpp
    ${EXTERNAL}/imgui/backends/imgui_impl_dx11.cpp
    ${EXTERNAL}/imgui/backends/imgui_impl_win32.cpp
)

target_include_directories(VisualForge PRIVATE
    src
    ${EXTERNAL}/f4se
    ${EXTERNAL}/minhook/include
    ${EXTERNAL}/imgui
    ${EXTERNAL}/imgui/backends
)

target_compile_definitions(VisualForge PRIVATE
    NOMINMAX
)

target_link_libraries(VisualForge PRIVATE
    vf_core
    vf_platform_win
    d3d11 dxgi d3dcompiler user32 shell32 ole32 gdi32 dwmapi dbghelp psapi
)

set_target_properties(VisualForge PROPERTIES
    OUTPUT_NAME "VisualForge"
    PREFIX ""
)

if(VF_BUILD_TESTS)
    enable_testing()
    set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${EXTERNAL}/Catch2 EXCLUDE_FROM_ALL)
    add_subdirectory(tests)
endif()
