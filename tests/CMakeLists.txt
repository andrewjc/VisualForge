add_executable(vf_unit_tests
    unit/EngineMeshTests.cpp
    unit/EngineSuppressionTests.cpp
    unit/EngineMaterialTests.cpp
    unit/EngineMaterialFamilyTests.cpp
    unit/EngineLightingTests.cpp
    unit/EngineAccelerationTests.cpp
    unit/EngineDrawStreamTests.cpp
    unit/EngineImageSpaceTests.cpp
    unit/EngineArchiveTests.cpp
    unit/EngineMaterialFileTests.cpp
    unit/EngineInflateTests.cpp
    unit/EngineIndirectTests.cpp
    unit/EngineTransparencyTests.cpp
    unit/EngineBridgeOrderTests.cpp
    unit/EnginePostChainTests.cpp
    unit/EnginePresentationTests.cpp
    unit/EngineWaterTests.cpp
    unit/EngineReflectionTests.cpp
    unit/EngineDeformationTests.cpp
    unit/EngineSceneTests.cpp
    unit/EngineTakeoverTests.cpp
    unit/EngineTerrainTests.cpp
    unit/EngineVisibilityTests.cpp
    unit/AddressLibraryTests.cpp
    unit/SceneDatabaseTests.cpp
    unit/EngineTextureTests.cpp
    unit/ShaderReflectionTests.cpp
    unit/EngineEnvironmentSourceTests.cpp
    unit/EngineViewTests.cpp
    unit/D3D11TextureContractTests.cpp
    unit/ResourceRegistryTests.cpp
    unit/TextureResidencyTests.cpp
    unit/ResourceManagerContractTests.cpp
    unit/RasterPacketTests.cpp
    unit/BackendContractTests.cpp
    unit/CameraStateScanTests.cpp
    unit/GraphicsStateContractTests.cpp
    unit/CaptureControlTests.cpp
    unit/BackendHostTests.cpp
    unit/BridgeContractTests.cpp
    unit/BridgeStateTests.cpp
    unit/BuildGateTests.cpp
    unit/HookManifestTests.cpp
    unit/HookTransactionTests.cpp
    unit/RendererBootstrapTests.cpp
    unit/RendererHealthTests.cpp
    unit/RendererStateTests.cpp
    unit/StableIdTests.cpp
    unit/TraceCodecTests.cpp
    unit/TraceProtocolTests.cpp
    unit/WindowsBuildProbeTests.cpp
    unit/WindowsBackendModuleTests.cpp
)

target_link_libraries(vf_unit_tests PRIVATE
    vf_core
    vf_platform_win
    Catch2::Catch2WithMain
)

target_compile_definitions(vf_unit_tests PRIVATE
    VF_SHADER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shaders"
)

if(VF_GAME_ROOT)
    target_compile_definitions(vf_unit_tests PRIVATE
        VF_GAME_ROOT="${VF_GAME_ROOT}"
    )
endif()

if(MSVC)
    target_compile_options(vf_unit_tests PRIVATE /W4 /WX)
endif()

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(vf_unit_tests
    TEST_PREFIX "unit::"
    PROPERTIES LABELS unit
)

get_filename_component(VF_MSVC_TOOL_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(VF_DUMPBIN_EXECUTABLE
    NAMES dumpbin.exe dumpbin
    HINTS "${VF_MSVC_TOOL_DIR}"
    REQUIRED
)

add_test(NAME contract.plugin_exports
    COMMAND ${CMAKE_COMMAND}
        -DPLUGIN=$<TARGET_FILE:VisualForge>
        -DDUMPBIN=${VF_DUMPBIN_EXECUTABLE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/contract/VerifyPluginExports.cmake
)
set_tests_properties(contract.plugin_exports PROPERTIES LABELS contract)

if(TARGET VisualForgeRenderer)
    add_test(NAME contract.backend_exports
        COMMAND ${CMAKE_COMMAND}
            -DBACKEND=$<TARGET_FILE:VisualForgeRenderer>
            -DDUMPBIN=${VF_DUMPBIN_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/contract/VerifyBackendExports.cmake
    )
    set_tests_properties(contract.backend_exports PROPERTIES LABELS contract)

    add_test(NAME contract.backend_vulkan_probe
        COMMAND $<TARGET_FILE:vf_backend_probe>
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --validation
    )
    set_tests_properties(contract.backend_vulkan_probe PROPERTIES
        LABELS "contract;gpu"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.bridge_pattern
        COMMAND $<TARGET_FILE:vf_bridge_probe>
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --exchanges 64
    )
    set_tests_properties(contract.bridge_pattern PROPERTIES
        LABELS "contract;gpu"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.raster_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-synthetic
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase06-raster.ppm
            --validation
            --fixtures
    )
    set_tests_properties(contract.raster_frame PROPERTIES
        LABELS "contract;gpu;phase6"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.textured_raster_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-synthetic
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase08-textured-raster.ppm
            --validation
            --texture-fixture
    )
    set_tests_properties(contract.textured_raster_frame PROPERTIES
        LABELS "contract;gpu;phase8"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.material_raster_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-synthetic
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase09-material-raster.ppm
            --validation
            --material-fixture
    )
    set_tests_properties(contract.material_raster_frame PROPERTIES
        LABELS "contract;gpu;phase9"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.view_raster_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-synthetic
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase10-view-raster.ppm
            --frame-output ${CMAKE_CURRENT_BINARY_DIR}/phase10-view.vfframe
            --validation
            --view-fixture
    )
    set_tests_properties(contract.view_raster_frame PROPERTIES
        LABELS "contract;gpu;phase10"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.scene_raster_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-scene
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase11-scene-raster.ppm
            --scene-output ${CMAKE_CURRENT_BINARY_DIR}/phase11-scene.vfscene
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase11-scene.vfgbuf
            --validation
    )
    set_tests_properties(contract.scene_raster_frame PROPERTIES
        LABELS "contract;gpu;phase11"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.instanced_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-instanced-scene
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase12-instanced.ppm
            --scene-output ${CMAKE_CURRENT_BINARY_DIR}/phase12-instanced.vfscene
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase12-instanced.vfgbuf
            --trace-output ${CMAKE_CURRENT_BINARY_DIR}/phase12-registry.vftrace
            --validation
    )
    set_tests_properties(contract.instanced_scene_frame PROPERTIES
        LABELS "contract;gpu;phase12"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.deformed_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-deformed-scene
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase13-deformed.ppm
            --deform-output ${CMAKE_CURRENT_BINARY_DIR}/phase13-deform.vfdeform
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase13-deformed.vfgbuf
            --validation
    )
    set_tests_properties(contract.deformed_scene_frame PROPERTIES
        LABELS "contract;gpu;phase13"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.terrain_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-terrain-scene
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase14-terrain.ppm
            --terrain-output ${CMAKE_CURRENT_BINARY_DIR}/phase14-terrain.vfterrain
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase14-terrain.vfgbuf
            --validation
    )
    set_tests_properties(contract.terrain_scene_frame PROPERTIES
        LABELS "contract;gpu;phase14"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.alpha_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-alpha-scene
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase15-alpha.ppm
            --scene-output ${CMAKE_CURRENT_BINARY_DIR}/phase15-alpha.vfscene
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase15-alpha.vfgbuf
            --validation
    )
    set_tests_properties(contract.alpha_scene_frame PROPERTIES
        LABELS "contract;gpu;phase15"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.family_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-family-scene
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase16-family.ppm
            --scene-output ${CMAKE_CURRENT_BINARY_DIR}/phase16-family.vfscene
            --family-output ${CMAKE_CURRENT_BINARY_DIR}/phase16-family.vffam
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase16-family.vfgbuf
            --validation
    )
    set_tests_properties(contract.family_scene_frame PROPERTIES
        LABELS "contract;gpu;phase16"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.texture_library_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-texture-library
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/texture-library.ppm
            --validation
    )
    set_tests_properties(contract.texture_library_frame PROPERTIES
        LABELS "contract;gpu;phase16"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.lit_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-family-scene
            --lit
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase17-lit.ppm
            --scene-output ${CMAKE_CURRENT_BINARY_DIR}/phase17-lit.vfscene
            --light-output ${CMAKE_CURRENT_BINARY_DIR}/phase17-lit.vffl
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase17-lit.vfgbuf
            --validation
    )
    set_tests_properties(contract.lit_scene_frame PROPERTIES
        LABELS "contract;gpu;phase17"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.shadowed_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-family-scene
            --shadows
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase18-shadowed.ppm
            --scene-output ${CMAKE_CURRENT_BINARY_DIR}/phase18-shadowed.vfscene
            --light-output ${CMAKE_CURRENT_BINARY_DIR}/phase18-shadowed.vffl
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase18-shadowed.vfgbuf
            --validation
    )
    set_tests_properties(contract.shadowed_scene_frame PROPERTIES
        LABELS "contract;gpu;phase18"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.reflected_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-family-scene
            --reflections
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase19-reflected.ppm
            --scene-output ${CMAKE_CURRENT_BINARY_DIR}/phase19-reflected.vfscene
            --light-output ${CMAKE_CURRENT_BINARY_DIR}/phase19-reflected.vffl
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase19-reflected.vfgbuf
            --validation
    )
    set_tests_properties(contract.reflected_scene_frame PROPERTIES
        LABELS "contract;gpu;phase19"
        RUN_SERIAL TRUE
    )

    # Whether the reflection ray finds geometry, which the family fixture
    # could not answer: its reflective plate faced the camera, so the mirror
    # direction pointed at the space between plate and viewer -- necessarily
    # empty, because anything there would occlude the plate. This fixture
    # computes the geometry instead of choosing it, and renders three
    # variants so "the reflection carries the target" and "the traced path
    # differs from the fallback at all" are separate measurements.
    add_test(NAME contract.mirror_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-mirror-scene
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase19-mirror.ppm
            --validation
    )
    set_tests_properties(contract.mirror_scene_frame PROPERTIES
        LABELS "contract;gpu;phase19"
        RUN_SERIAL TRUE
    )

    # The temporal half of phase 20. Compares whole histories rather than a
    # picture: accumulation has no picture of its own, it decides how fast one
    # appears, and a pass that blended a rejected sample would look like a
    # slightly soft frame rather than a trail behind everything that moved.
    add_test(NAME contract.indirect_accumulation
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-indirect-accumulation
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase20-accumulate.ppm
            --validation
    )
    set_tests_properties(contract.indirect_accumulation PROPERTIES
        LABELS "contract;gpu;phase20"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.indirect_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-family-scene
            --indirect
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase20-indirect.ppm
            --scene-output ${CMAKE_CURRENT_BINARY_DIR}/phase20-indirect.vfscene
            --light-output ${CMAKE_CURRENT_BINARY_DIR}/phase20-indirect.vffl
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase20-indirect.vfgbuf
            --validation
    )
    set_tests_properties(contract.indirect_scene_frame PROPERTIES
        LABELS "contract;gpu;phase20"
        RUN_SERIAL TRUE
    )

    add_test(NAME contract.transparent_scene_frame
        COMMAND $<TARGET_FILE:vf_packet_replay>
            --render-family-scene
            --transparency
            --backend $<TARGET_FILE:VisualForgeRenderer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/phase21-transparent.ppm
            --scene-output ${CMAKE_CURRENT_BINARY_DIR}/phase21-transparent.vfscene
            --light-output ${CMAKE_CURRENT_BINARY_DIR}/phase21-transparent.vffl
            --gbuffer-output ${CMAKE_CURRENT_BINARY_DIR}/phase21-transparent.vfgbuf
            --validation
    )
    set_tests_properties(contract.transparent_scene_frame PROPERTIES
        LABELS "contract;gpu;phase21"
        RUN_SERIAL TRUE
    )
endif()

add_test(NAME contract.material_sweep
    COMMAND $<TARGET_FILE:vf_packet_replay>
        --render-material-fixture
        --output ${CMAKE_CURRENT_BINARY_DIR}/phase09-material-sweep.ppm
        --bundle-output ${CMAKE_CURRENT_BINARY_DIR}/phase09-material.vfmat
        --width 256
        --height 192
)
set_tests_properties(contract.material_sweep PROPERTIES
    LABELS "contract;phase9"
)
