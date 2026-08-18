#include "renderer_host/BackendContract.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

void Log(void*, std::uint32_t, const char*)
{}

vf::renderer::abi::Result Probe(
    void*,
    const vf::renderer::abi::ProbeRequestV1*,
    vf::renderer::abi::CapabilityReportV1*)
{
    return vf::renderer::abi::Result::Success;
}

void Shutdown(void*)
{}

}

TEST_CASE("P04_backend_abi_layout_and_required_mask_are_stable", "[unit][phase04]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiMajor == 1);
    CHECK(kBackendAbiPhase4Minor == 0);
    CHECK(kBackendAbiPhase6Minor == 2);
    CHECK(sizeof(AdapterLuid) == 8);
    CHECK(sizeof(HostCallbacksV1) % 8 == 0);
    CHECK(sizeof(ProbeRequestV1) % 8 == 0);
    CHECK(sizeof(CapabilityReportV1) % 8 == 0);
    CHECK(sizeof(BackendApiV1) % 8 == 0);
    CHECK((kRequiredCapabilities & Capability::AccelerationStructure) != 0);
    CHECK((kRequiredCapabilities & Capability::RayTracingPipeline) != 0);
    CHECK((kRequiredCapabilities & Capability::ExternalMemoryWin32) != 0);
    CHECK((kRequiredCapabilities & Capability::ExternalSemaphoreWin32) != 0);
    CHECK((kRequiredCapabilities & Capability::DebugUtils) == 0);
}

TEST_CASE("P08_backend_ABI_reuses_reserved_frame_words_without_layout_drift", "[phase8][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiPhase8Minor == 3);
    CHECK(offsetof(RasterFrameRequestV1, textureData) == 48);
    CHECK(offsetof(RasterFrameRequestV1, textureSize) == 56);
    CHECK(kRasterFrameRequestV1TextureRequiredSize == 64);
    CHECK(offsetof(RasterFrameRequestV1, materialData) == 64);
}

TEST_CASE("P09_backend_ABI_promotes_material_bundle_without_layout_drift", "[phase9][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiPhase9Minor == 4);
    CHECK(kBackendAbiMinor >= kBackendAbiPhase9Minor);
    CHECK(offsetof(RasterFrameRequestV1, materialData) == 64);
    CHECK(offsetof(RasterFrameRequestV1, materialSize) == 72);
    CHECK(kRasterFrameRequestV1MaterialRequiredSize == 80);
    CHECK(offsetof(RasterFrameRequestV1, frameData) == 80);
}

TEST_CASE("P10_backend_ABI_appends_frame_packet_without_prefix_drift", "[phase10][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiPhase10Minor == 5);
    CHECK(kBackendAbiMinor >= kBackendAbiPhase10Minor);
    CHECK(offsetof(RasterFrameRequestV1, textureData) == 48);
    CHECK(offsetof(RasterFrameRequestV1, materialData) == 64);
    CHECK(offsetof(RasterFrameRequestV1, frameData) == 80);
    CHECK(offsetof(RasterFrameRequestV1, frameSize) == 88);
    CHECK(kRasterFrameRequestV1FrameRequiredSize == 96);
    CHECK(offsetof(RasterFrameRequestV1, sceneData) == 96);
}

TEST_CASE("P11_backend_ABI_appends_scene_and_G_buffer_without_prefix_drift", "[phase11][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiPhase11Minor == 6);
    // Later phases append fields, so the current minor only has to be at
    // least this phase's minor while its offsets stay exact.
    CHECK(kBackendAbiMinor >= kBackendAbiPhase11Minor);
    CHECK(kRasterFrameRequestV1FrameRequiredSize == 96);
    CHECK(offsetof(RasterFrameRequestV1, sceneData) == 96);
    CHECK(offsetof(RasterFrameRequestV1, sceneSize) == 104);
    CHECK(kRasterFrameRequestV1SceneInputRequiredSize == 112);
    CHECK(offsetof(RasterFrameRequestV1, gbufferData) == 112);
    CHECK(offsetof(RasterFrameRequestV1, gbufferCapacity) == 120);
    CHECK(kRasterFrameRequestV1SceneRequiredSize == 128);
    CHECK(offsetof(RasterFrameRequestV1, deformationData) == 128);
}

TEST_CASE("P13_backend_ABI_appends_deformation_without_prefix_drift",
    "[phase13][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiPhase13Minor == 7);
    // Phase 14 appends terrain, so this phase pins its own offsets and only
    // requires the current minor to be at least its own. See phase-14.md.
    CHECK(kBackendAbiMinor >= kBackendAbiPhase13Minor);
    CHECK(kRasterFrameRequestV1SceneRequiredSize == 128);
    CHECK(offsetof(RasterFrameRequestV1, deformationData) == 128);
    CHECK(offsetof(RasterFrameRequestV1, deformationSize) == 136);
    CHECK(kRasterFrameRequestV1DeformationInputRequiredSize == 144);
    CHECK(offsetof(RasterFrameRequestV1, deformationOutputData) == 144);
    CHECK(offsetof(RasterFrameRequestV1, deformationOutputCapacity) == 152);
    CHECK(kRasterFrameRequestV1DeformationRequiredSize == 160);
    CHECK(offsetof(RasterFrameRequestV1, terrainData) == 160);
}

TEST_CASE("P14_backend_ABI_appends_terrain_without_prefix_drift",
    "[phase14][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiPhase14Minor == 8);
    // Phase 16 appends material families and the HDR readback, so this phase
    // pins its own offsets and only requires the current minor to be at
    // least its own. See phase-16.md.
    CHECK(kBackendAbiMinor >= kBackendAbiPhase14Minor);
    // Every earlier phase's prefix survives byte for byte.
    CHECK(kRasterFrameRequestV1RequiredSize == 48);
    CHECK(kRasterFrameRequestV1TextureRequiredSize == 64);
    CHECK(kRasterFrameRequestV1MaterialRequiredSize == 80);
    CHECK(kRasterFrameRequestV1FrameRequiredSize == 96);
    CHECK(kRasterFrameRequestV1SceneInputRequiredSize == 112);
    CHECK(kRasterFrameRequestV1SceneRequiredSize == 128);
    CHECK(kRasterFrameRequestV1DeformationInputRequiredSize == 144);
    CHECK(kRasterFrameRequestV1DeformationRequiredSize == 160);
    CHECK(offsetof(RasterFrameRequestV1, terrainData) == 160);
    CHECK(offsetof(RasterFrameRequestV1, terrainSize) == 168);
    CHECK(kRasterFrameRequestV1TerrainRequiredSize == 176);
    CHECK(offsetof(RasterFrameRequestV1, familyData) == 176);
    CHECK(sizeof(RasterFrameRequestV1) % 8 == 0);
}

TEST_CASE("PM_bridge_request_carries_rendered_pixels_without_prefix_drift",
    "[mirror][contract]")
{
    using namespace vf::renderer::abi;
    // The bridge presents whatever image the backend puts in the shared
    // resource. Carrying rendered pixels lets it present the mirrored scene
    // instead of the built-in test pattern, which is the difference between
    // proving the transport works and proving the renderer works.
    CHECK(offsetof(BridgePatternRequestV1, structSize) == 0);
    CHECK(offsetof(BridgePatternRequestV1, imageIndex) == 4);
    CHECK(offsetof(BridgePatternRequestV1, epoch) == 8);
    CHECK(offsetof(BridgePatternRequestV1, releaseValue) == 16);
    CHECK(offsetof(BridgePatternRequestV1, readyValue) == 24);
    CHECK(offsetof(BridgePatternRequestV1, frameIndex) == 32);
    // Carved out of the former reserved block, so the size is unchanged and
    // a caller that fills only the original prefix still submits a pattern.
    CHECK(offsetof(BridgePatternRequestV1, pixelData) == 40);
    CHECK(offsetof(BridgePatternRequestV1, pixelSize) == 48);
    CHECK(kBridgePatternRequestV1RequiredSize == 40);
    CHECK(kBridgePatternRequestV1PixelRequiredSize == 56);
    CHECK(sizeof(BridgePatternRequestV1) == 72);
    CHECK(sizeof(BridgePatternRequestV1) % 8 == 0);

    // Defaulted, a request is a pattern request; nothing changes for an
    // existing caller.
    const BridgePatternRequestV1 defaulted{};
    CHECK(defaulted.pixelData == 0);
    CHECK(defaulted.pixelSize == 0);
}

TEST_CASE("P16_backend_ABI_appends_families_and_hdr_without_prefix_drift",
    "[phase16][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiPhase16Minor == 9);
    // Phase 17 appends the light payload, so this phase pins its own
    // offsets and only requires the current minor to be at least its own.
    CHECK(kBackendAbiMinor >= kBackendAbiPhase16Minor);
    // Every earlier phase's prefix survives byte for byte.
    CHECK(kRasterFrameRequestV1RequiredSize == 48);
    CHECK(kRasterFrameRequestV1TextureRequiredSize == 64);
    CHECK(kRasterFrameRequestV1MaterialRequiredSize == 80);
    CHECK(kRasterFrameRequestV1FrameRequiredSize == 96);
    CHECK(kRasterFrameRequestV1SceneInputRequiredSize == 112);
    CHECK(kRasterFrameRequestV1SceneRequiredSize == 128);
    CHECK(kRasterFrameRequestV1DeformationInputRequiredSize == 144);
    CHECK(kRasterFrameRequestV1DeformationRequiredSize == 160);
    CHECK(kRasterFrameRequestV1TerrainRequiredSize == 176);
    CHECK(offsetof(RasterFrameRequestV1, familyData) == 176);
    CHECK(offsetof(RasterFrameRequestV1, familySize) == 184);
    CHECK(kRasterFrameRequestV1FamilyRequiredSize == 192);
    // The HDR readback is a separate pair so a caller can supply families
    // without also asking for the float colour target, and vice versa.
    CHECK(offsetof(RasterFrameRequestV1, hdrData) == 192);
    CHECK(offsetof(RasterFrameRequestV1, hdrCapacity) == 200);
    CHECK(kRasterFrameRequestV1HdrRequiredSize == 208);
    CHECK(offsetof(RasterFrameRequestV1, lightData) == 208);
    CHECK(sizeof(RasterFrameRequestV1) % 8 == 0);
}

TEST_CASE("P17_backend_ABI_appends_lighting_without_prefix_drift",
    "[phase17][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiPhase17Minor == 10);
    // Every earlier phase prefix survives byte for byte. These offsets are the
    // contract; the struct's total size is not, because a later phase appends
    // to it. Pinning the size here instead of the prefix would fail on every
    // addition while catching none of the drift the offsets exist to catch.
    CHECK(kRasterFrameRequestV1TerrainRequiredSize == 176);
    CHECK(kRasterFrameRequestV1FamilyRequiredSize == 192);
    CHECK(kRasterFrameRequestV1HdrRequiredSize == 208);
    CHECK(offsetof(RasterFrameRequestV1, lightData) == 208);
    CHECK(offsetof(RasterFrameRequestV1, lightSize) == 216);
    CHECK(kRasterFrameRequestV1LightRequiredSize == 224);
    CHECK(sizeof(RasterFrameRequestV1) >= 224);
    CHECK(sizeof(RasterFrameRequestV1) % 8 == 0);
}

TEST_CASE("P20_backend_ABI_appends_indirect_after_the_lighting_prefix",
    "[phase20][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiPhase20Minor == 11);
    // The temporal pass begins exactly where phase 17 ended, so a backend
    // built before it reads every field it knows at the offset it expects.
    CHECK(offsetof(RasterFrameRequestV1, indirectCurrentData) ==
        kRasterFrameRequestV1LightRequiredSize);
    // At least, not exactly: a later phase appends to this struct, and an
    // exact claim here fails on every addition while catching none of the
    // prefix drift the offset above exists to catch.
    CHECK(kRasterFrameRequestV1IndirectRequiredSize <=
        sizeof(RasterFrameRequestV1));
    CHECK(sizeof(RasterFrameRequestV1) % 8 == 0);
    // A caller that fills only through the lighting fields declares a size
    // below the indirect requirement, which is what lets the backend tell
    // "no history supplied" from "history supplied at a stale offset".
    CHECK(kRasterFrameRequestV1LightRequiredSize <
        kRasterFrameRequestV1IndirectRequiredSize);
}

TEST_CASE("P04_backend_contract_negotiates_size_major_and_optional_minor", "[unit][phase04]")
{
    using namespace vf::renderer;
    using namespace vf::renderer::abi;

    HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = Log;
    CHECK(ValidateHostCallbacks(callbacks) == BackendContractError::None);
    callbacks.structSize =
        static_cast<std::uint32_t>(kHostCallbacksV1RequiredSize - 1);
    CHECK(ValidateHostCallbacks(callbacks) ==
          BackendContractError::HostCallbacksTooSmall);
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = nullptr;
    CHECK(ValidateHostCallbacks(callbacks) ==
          BackendContractError::NullLogCallback);

    BackendApiV1 api{};
    api.structSize = sizeof(api);
    api.abiMajor = kBackendAbiMajor;
    api.abiMinor = 99;
    api.probe = Probe;
    api.shutdown = Shutdown;
    CHECK(ValidateBackendApi(api) == BackendContractError::None);

    api.structSize = static_cast<std::uint32_t>(kBackendApiV1RequiredSize);
    CHECK(ValidateBackendApi(api) == BackendContractError::None);
    --api.structSize;
    CHECK(ValidateBackendApi(api) == BackendContractError::ApiTooSmall);
    api.structSize = sizeof(api);
    ++api.abiMajor;
    CHECK(ValidateBackendApi(api) == BackendContractError::AbiMajorMismatch);
    api.abiMajor = kBackendAbiMajor;
    api.probe = nullptr;
    CHECK(ValidateBackendApi(api) == BackendContractError::NullProbeFunction);
    api.probe = Probe;
    api.shutdown = nullptr;
    CHECK(ValidateBackendApi(api) ==
          BackendContractError::NullShutdownFunction);
}

TEST_CASE("P04_adapter_selection_requires_exact_luid_capabilities_and_queue", "[unit][phase04]")
{
    using namespace vf::renderer;
    using namespace vf::renderer::abi;

    constexpr AdapterLuid wanted{0xAABBCCDDu, 0x12345678};
    const std::array transferOnly{
        QueueFamilySnapshot{QueueTransfer, 1}};
    const std::array validQueues{
        QueueFamilySnapshot{QueueTransfer, 1},
        QueueFamilySnapshot{QueueGraphics, 1},
        QueueFamilySnapshot{QueueGraphics | QueueCompute | QueueTransfer, 2},
    };
    const std::array adapters{
        AdapterSnapshot{
            {0x11111111u, 0},
            kRequiredCapabilities,
            validQueues,
            0x1002,
            1,
            1,
            0,
        },
        AdapterSnapshot{
            wanted,
            kRequiredCapabilities,
            validQueues,
            0x10DE,
            0x2684,
            1,
            0,
        },
    };

    auto selected = SelectAdapter(
        wanted, kRequiredCapabilities, adapters);
    REQUIRE(selected);
    CHECK(selected.adapterIndex == 1);
    CHECK(selected.queueFamilyIndex == 2);
    CHECK(selected.missingCapabilities == 0);

    selected = SelectAdapter(
        AdapterLuid{0xDEADBEEFu, 0},
        kRequiredCapabilities,
        adapters);
    CHECK(selected.error == AdapterSelectionError::AdapterLuidNotFound);

    auto missingAdapter = adapters[1];
    missingAdapter.capabilities &=
        ~static_cast<std::uint64_t>(Capability::RayTracingPipeline);
    selected = SelectAdapter(
        wanted,
        kRequiredCapabilities,
        std::span{&missingAdapter, 1});
    CHECK(selected.error ==
          AdapterSelectionError::MissingRequiredCapabilities);
    CHECK(selected.missingCapabilities ==
          static_cast<std::uint64_t>(Capability::RayTracingPipeline));

    auto queueMissing = adapters[1];
    queueMissing.queues = transferOnly;
    selected = SelectAdapter(
        wanted,
        kRequiredCapabilities,
        std::span{&queueMissing, 1});
    CHECK(selected.error == AdapterSelectionError::QueueFamilyNotFound);

    auto invalidLuid = adapters[1];
    invalidLuid.luidValid = 0;
    selected = SelectAdapter(
        wanted,
        kRequiredCapabilities,
        std::span{&invalidLuid, 1});
    CHECK(selected.error == AdapterSelectionError::AdapterLuidNotFound);
}

TEST_CASE("P23_backend_ABI_appends_the_post_rules_after_the_indirect_prefix",
    "[phase23][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiPhase23Minor == 12);
    // At least, not exactly: the texture library appended after bloom moved
    // the current minor version past phase 23's own. Phase 23's block is
    // still a real, still-valid prefix of the struct -- it just is not the
    // tail any longer, which is what the next test pins.
    CHECK(kBackendAbiMinor >= kBackendAbiPhase23Minor);
    // The post rules begin exactly where the temporal block ended, so a
    // backend built before them reads every field it knows at the offset it
    // expects.
    CHECK(offsetof(RasterFrameRequestV1, bloomThreshold) ==
        kRasterFrameRequestV1IndirectRequiredSize);
    CHECK(kRasterFrameRequestV1BloomRequiredSize <=
        sizeof(RasterFrameRequestV1));
    CHECK(sizeof(RasterFrameRequestV1) % 8 == 0);
    // A caller that fills only through the temporal fields declares a size
    // below the bloom requirement, which is what lets the backend tell "no
    // rules supplied" from "rules supplied at a stale offset".
    CHECK(kRasterFrameRequestV1IndirectRequiredSize <
        kRasterFrameRequestV1BloomRequiredSize);
}

TEST_CASE("PM_backend_ABI_appends_the_texture_library_after_the_bloom_prefix",
    "[material][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiTextureLibraryMinor == 13);
    // The current minor only has to be at least this phase's. Pinning it to
    // equality made every later phase edit this line, which is how a version
    // test stops testing the version and starts testing the last edit.
    CHECK(kBackendAbiMinor >= kBackendAbiTextureLibraryMinor);
    // The texture library begins exactly where the post-chain block ended,
    // so a backend built before it reads every field it knows at the offset
    // it expects.
    CHECK(offsetof(RasterFrameRequestV1, textureLibraryData) ==
        kRasterFrameRequestV1BloomRequiredSize);
    // A prefix of the struct, not the whole of it: later phases append, and
    // this phase's requirement must keep naming this phase's last field.
    CHECK(kRasterFrameRequestV1TextureLibraryRequiredSize <=
        sizeof(RasterFrameRequestV1));
    CHECK(kRasterFrameRequestV1TextureLibraryRequiredSize ==
        offsetof(RasterFrameRequestV1, textureLibrarySize) +
            sizeof(RasterFrameRequestV1::textureLibrarySize));
    CHECK(sizeof(RasterFrameRequestV1) % 8 == 0);
    // A caller that fills only through the bloom fields declares a size
    // below the texture-library requirement, which is what lets the backend
    // tell "no library supplied" from "a library supplied at a stale offset".
    CHECK(kRasterFrameRequestV1BloomRequiredSize <
        kRasterFrameRequestV1TextureLibraryRequiredSize);
}

TEST_CASE("PM_backend_ABI_appends_the_library_generation_after_the_library",
    "[material][contract]")
{
    using namespace vf::renderer::abi;
    CHECK(kBackendAbiLibraryGenerationMinor == 14);
    CHECK(kBackendAbiMinor >= kBackendAbiLibraryGenerationMinor);
    // Appended, so a backend built before the generation existed reads every
    // field it knows at the offset it expects and simply never sees this one.
    CHECK(offsetof(RasterFrameRequestV1, textureLibraryGeneration) ==
        kRasterFrameRequestV1TextureLibraryRequiredSize);
    // Each extension is appended after the last, so the newest requirement is
    // the one that reaches the end of the struct. Pinning *this* field to the
    // end instead made the assertion a statement about which extension came
    // last, which is true until the next one lands and says nothing about the
    // property the test is named for.
    CHECK(kRasterFrameRequestV1LibraryGenerationRequiredSize <=
        sizeof(RasterFrameRequestV1));
    CHECK(offsetof(RasterFrameRequestV1, indirectRaysPerPixel) ==
        kRasterFrameRequestV1LibraryGenerationRequiredSize);
    CHECK(kRasterFrameRequestV1LibraryGenerationRequiredSize <
        kRasterFrameRequestV1IndirectRaysRequiredSize);
    // The bounce count is a policy, and zero is reserved for "the frame
    // declares none" so an older caller traces exactly what it always did.
    CHECK(kDefaultIndirectRaysPerPixel == 8);
    // A caller that fills only through the library declares a size below the
    // generation requirement, which is what lets the backend tell "no
    // generation tracked" from "a generation at a stale offset" and fall back
    // to hashing the bytes rather than trusting a field that is not there.
    CHECK(kRasterFrameRequestV1TextureLibraryRequiredSize <
        kRasterFrameRequestV1LibraryGenerationRequiredSize);
    // Zero is reserved for "not tracked", so it can never match a stored
    // generation and can never suppress an upload.
    RasterFrameRequestV1 request{};
    CHECK(request.textureLibraryGeneration == 0);
}
