#include "renderer_api/RasterPacket.h"
#include "renderer_core/EngineMaterial.h"
#include "renderer_core/EngineMesh.h"
#include "renderer_core/EngineDeformation.h"
#include "renderer_core/EngineScene.h"
#include "renderer_core/EnginePostChain.h"
#include "renderer_core/EngineTransparency.h"
#include "renderer_core/EngineTerrain.h"
#include "renderer_core/EngineTexture.h"
#include "renderer_core/SceneDatabase.h"
#include "renderer_core/EngineView.h"
#include "renderer_core/RasterGolden.h"
#include "renderer_host/BackendHost.h"
#include "renderer_host/WindowsBackendModule.h"
#include "renderer_trace/Crc32.h"
#include "renderer_trace/TraceCodec.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace vf::renderer;

// The bloom tuning the replay declares. Deliberately stronger than the
// contract's default: at the default intensity the knee's smoothstep and a
// linear ramp differ by less than one eight-bit code, so the shape of the
// curve would be unobservable in the only comparison this contract can make.
// Both the device and the oracle read these same numbers.
constexpr post::BloomRules kReplayBloomRules{0.15f, 0.12f, 1.5f};

struct RenderOptions
{
    std::filesystem::path backend;
    std::filesystem::path output;
    std::filesystem::path meshInput;
    std::filesystem::path textureInput;
    std::filesystem::path materialInput;
    std::filesystem::path frameInput;
    // A live capture carries every camera the engine had, not one, so the
    // replay has to say which of them it is reconstructing.
    std::uint32_t frameView{};
    std::filesystem::path frameOutput;
    std::uint32_t width{96};
    std::uint32_t height{64};
    raster::IndexType indexType{raster::IndexType::Uint16};
    bool validation{};
    bool fixtures{};
    bool textureFixture{};
    bool materialFixture{};
    bool viewFixture{};
};

struct SceneRenderOptions
{
    std::filesystem::path backend;
    std::filesystem::path output;
    std::filesystem::path sceneOutput;
    std::filesystem::path gbufferOutput;
    std::uint32_t width{96};
    std::uint32_t height{64};
    bool validation{};
};

struct InstancedRenderOptions
{
    std::filesystem::path backend;
    std::filesystem::path output;
    std::filesystem::path sceneOutput;
    std::filesystem::path gbufferOutput;
    std::filesystem::path traceOutput;
    std::uint32_t width{96};
    std::uint32_t height{64};
    bool validation{};
};

struct DeformedRenderOptions
{
    std::filesystem::path backend;
    std::filesystem::path output;
    std::filesystem::path deformOutput;
    std::filesystem::path gbufferOutput;
    std::uint32_t width{96};
    std::uint32_t height{64};
    std::uint32_t frames{6};
    bool validation{};
};

struct AlphaRenderOptions
{
    std::filesystem::path backend;
    std::filesystem::path output;
    std::filesystem::path sceneOutput;
    std::filesystem::path gbufferOutput;
    std::uint32_t width{256};
    std::uint32_t height{192};
    bool validation{};
};

struct FamilyRenderOptions
{
    std::filesystem::path backend;
    std::filesystem::path output;
    std::filesystem::path sceneOutput;
    std::filesystem::path familyOutput;
    std::filesystem::path gbufferOutput;
    std::filesystem::path lightOutput;
    std::uint32_t width{256};
    std::uint32_t height{192};
    bool validation{};
    // Adds the captured light list and environment. The same fixture with
    // and without it is what makes the lighting contribution measurable
    // rather than merely present.
    bool lit{};
    // Phase 18: appends the occluder and the light it blocks. Implies lit.
    bool shadows{};
    // Phase 19: makes the appended object a smooth metal, so it reflects.
    // Implies shadows, because the reflection traces the same structure.
    bool reflections{};
    // Phase 20: one bounce of diffuse indirect over the same geometry.
    bool indirect{};
    // Blended draws composited after the opaque pass, in the contract's own
    // sorted order.
    bool transparency{};
};

struct TerrainRenderOptions
{
    std::filesystem::path backend;
    std::filesystem::path output;
    std::filesystem::path terrainOutput;
    std::filesystem::path gbufferOutput;
    std::uint32_t width{256};
    std::uint32_t height{192};
    bool validation{};
};

constexpr std::uint32_t kSceneObjectCount = 3;
constexpr std::uint64_t kSceneFrameIndex = 1;
constexpr std::uint64_t kSceneViewId = 0xA000'0000'0000'1101ull;
constexpr std::uint64_t kSceneCameraId = 0xA000'0000'0000'2101ull;
constexpr std::uint64_t kSceneObjectIdBase = 0x5100'0000'0000'1101ull;
constexpr std::uint64_t kSceneMaterialIdBase = 0x5200'0000'0000'1101ull;

struct MaterialRenderOptions
{
    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path bundleOutput;
    std::uint32_t width{256};
    std::uint32_t height{192};
    bool fixture{};
};

int InspectFile(const std::filesystem::path& path)
{
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError) {
        std::cerr << "vf_packet_replay: cannot stat input\n";
        return 3;
    }
    if (fileSize > trace::kDefaultMaximumTraceBytes ||
        fileSize > std::numeric_limits<std::size_t>::max()) {
        std::cerr << "vf_packet_replay: input exceeds trace limit\n";
        return 3;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        std::cerr << "vf_packet_replay: cannot open input\n";
        return 3;
    }
    std::vector<std::byte> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(fileSize));
    } catch (...) {
        std::cerr << "vf_packet_replay: input allocation failed\n";
        return 3;
    }
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    if (input.bad() || static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        std::cerr << "vf_packet_replay: input read failed\n";
        return 3;
    }
    const auto inspection = trace::InspectTrace(bytes);
    if (!inspection) {
        std::cerr << "vf_packet_replay: invalid trace at "
                  << inspection.errorOffset << ": "
                  << trace::ToString(inspection.error) << '\n';
        return 4;
    }
    std::cout << trace::FormatTraceSummary(inspection.summary) << '\n';
    return 0;
}

void BackendLog(void*, const std::uint32_t level, const char* message)
{
    std::cout << "raster-backend[" << level << "]: "
              << (message == nullptr ? "" : message) << '\n';
}

// The adapter the replay renders on. Chosen explicitly rather than by letting
// D3D pick a default: a machine with a virtual display driver installed -- a
// headset compositor, a remote desktop monitor -- can have one of those as the
// default adapter, and it has a D3D11 driver but no Vulkan implementation. The
// backend then reports that no device carries the requested LUID, which reads
// as a renderer fault rather than as the tool having asked about the wrong
// adapter. Measured on this machine: a Meta virtual monitor and a Virtual
// Desktop monitor enumerate alongside the discrete GPU.
//
// The largest dedicated video memory, skipping software adapters. A virtual
// display has none of its own, so this picks the real GPU without having to
// recognise any particular vendor or product.
bool QueryDefaultAdapterLuid(abi::AdapterLuid& luid)
{
    IDXGIFactory1* factory{};
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
            reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* candidate{};
        SIZE_T bestMemory = 0;
        auto found = false;
        for (UINT index = 0;
             factory->EnumAdapters1(index, &candidate) != DXGI_ERROR_NOT_FOUND;
             ++index) {
            DXGI_ADAPTER_DESC1 described{};
            if (SUCCEEDED(candidate->GetDesc1(&described))) {
                std::fprintf(stderr, "adapter: dxgi %u luid=%d:%u "
                    "vram=%llu flags=%u\n",
                    index, described.AdapterLuid.HighPart,
                    described.AdapterLuid.LowPart,
                    static_cast<unsigned long long>(
                        described.DedicatedVideoMemory),
                    described.Flags);
            }
            if (SUCCEEDED(candidate->GetDesc1(&described)) &&
                (described.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                described.DedicatedVideoMemory > bestMemory) {
                bestMemory = described.DedicatedVideoMemory;
                luid.lowPart = described.AdapterLuid.LowPart;
                luid.highPart = described.AdapterLuid.HighPart;
                found = true;
            }
            candidate->Release();
        }
        factory->Release();
        if (found) return true;
    }

    // No adapter reported dedicated memory, which is what an integrated-only
    // machine looks like. The default is then the only sensible answer.
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    D3D_FEATURE_LEVEL selected{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            &device, &selected, &context))) {
        return false;
    }
    IDXGIDevice* dxgiDevice{};
    IDXGIAdapter* adapter{};
    DXGI_ADAPTER_DESC description{};
    const auto queried = SUCCEEDED(device->QueryInterface(
        __uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)));
    const auto adapted = queried &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter));
    const auto described = adapted &&
        SUCCEEDED(adapter->GetDesc(&description));
    if (described) {
        luid.lowPart = description.AdapterLuid.LowPart;
        luid.highPart = description.AdapterLuid.HighPart;
    }
    if (adapter != nullptr) {
        adapter->Release();
    }
    if (dxgiDevice != nullptr) {
        dxgiDevice->Release();
    }
    context->Release();
    device->Release();
    return described;
}

bool ParseUnsigned(const std::string_view text, std::uint32_t& value)
{
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool ParseRenderOptions(
    const int argc,
    const char* const* argv,
    const int firstOption,
    RenderOptions& options)
{
    for (int index = firstOption; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--backend" && index + 1 < argc) {
            options.backend = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else if (argument == "--width" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.width)) {
                return false;
            }
        } else if (argument == "--height" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.height)) {
                return false;
            }
        } else if (argument == "--index-width" && index + 1 < argc) {
            const std::string_view width{argv[++index]};
            if (width == "16") {
                options.indexType = raster::IndexType::Uint16;
            } else if (width == "32") {
                options.indexType = raster::IndexType::Uint32;
            } else {
                return false;
            }
        } else if (argument == "--validation") {
            options.validation = true;
        } else if (argument == "--fixtures") {
            options.fixtures = true;
        } else if (argument == "--texture" && index + 1 < argc) {
            options.textureInput = argv[++index];
        } else if (argument == "--texture-fixture") {
            options.textureFixture = true;
        } else if (argument == "--material" && index + 1 < argc) {
            options.materialInput = argv[++index];
        } else if (argument == "--material-fixture") {
            options.materialFixture = true;
        } else if (argument == "--frame" && index + 1 < argc) {
            options.frameInput = argv[++index];
        } else if (argument == "--frame-view" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.frameView)) {
                return false;
            }
        } else if (argument == "--frame-output" && index + 1 < argc) {
            options.frameOutput = argv[++index];
        } else if (argument == "--view-fixture") {
            options.viewFixture = true;
        } else {
            return false;
        }
    }
    const auto hasFrameSource = options.viewFixture ||
        !options.frameInput.empty();
    return !options.backend.empty() && !options.output.empty() &&
        options.width != 0 && options.height != 0 &&
        !(options.viewFixture && !options.frameInput.empty()) &&
        (options.frameOutput.empty() || hasFrameSource) &&
        !(options.fixtures &&
            (hasFrameSource || !options.frameOutput.empty()));
}

texture::CapturedTexture BuildTextureFixture()
{
    texture::CapturedTexture fixture{};
    fixture.resourceId = 0x8000'0000'0000'1001ull;
    fixture.generation = 1;
    fixture.width = 4;
    fixture.height = 4;
    fixture.resourceFormat = texture::TextureFormat::BC1Typeless;
    fixture.viewFormat = texture::TextureFormat::BC1UnormSrgb;
    fixture.sampler.minFilter = texture::TextureFilter::Nearest;
    fixture.sampler.magFilter = texture::TextureFilter::Nearest;
    fixture.sampler.mipFilter = texture::TextureFilter::Nearest;
    texture::TextureSubresource subresource{};
    subresource.width = 4;
    subresource.height = 4;
    subresource.rowPitch = 8;
    subresource.slicePitch = 8;
    subresource.bytes.resize(8);
    const std::uint16_t red = 0xF800u;
    const std::uint16_t green = 0x07E0u;
    std::uint32_t selectors{};
    const std::uint32_t values[]{
        0, 0, 1, 1,
        0, 0, 1, 1,
        2, 2, 3, 3,
        2, 2, 3, 3,
    };
    for (std::size_t index = 0; index < std::size(values); ++index) {
        selectors |= values[index] << (index * 2);
    }
    std::memcpy(subresource.bytes.data(), &red, sizeof(red));
    std::memcpy(subresource.bytes.data() + 2, &green, sizeof(green));
    std::memcpy(subresource.bytes.data() + 4,
        &selectors, sizeof(selectors));
    fixture.subresources.push_back(std::move(subresource));
    return fixture;
}

texture::CapturedTexture BuildBc5Fixture(
    const std::uint64_t resourceId,
    const bool varied)
{
    texture::CapturedTexture fixture{};
    fixture.resourceId = resourceId;
    fixture.generation = 1;
    fixture.width = 4;
    fixture.height = 4;
    fixture.resourceFormat = texture::TextureFormat::BC5Typeless;
    fixture.viewFormat = texture::TextureFormat::BC5Unorm;
    fixture.sampler.minFilter = texture::TextureFilter::Nearest;
    fixture.sampler.magFilter = texture::TextureFilter::Nearest;
    fixture.sampler.mipFilter = texture::TextureFilter::Nearest;
    fixture.sampler.maxLod = 0.0f;
    texture::TextureSubresource subresource{};
    subresource.width = 4;
    subresource.height = 4;
    subresource.rowPitch = 16;
    subresource.slicePitch = 16;
    subresource.bytes.assign(16, std::byte{0});
    if (!varied) {
        subresource.bytes[0] = std::byte{128};
        subresource.bytes[1] = std::byte{128};
        subresource.bytes[8] = std::byte{128};
        subresource.bytes[9] = std::byte{128};
    } else {
        subresource.bytes[0] = std::byte{240};
        subresource.bytes[1] = std::byte{32};
        subresource.bytes[8] = std::byte{240};
        subresource.bytes[9] = std::byte{32};
        std::uint64_t redSelectors{};
        std::uint64_t greenSelectors{};
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const auto texel = y * 4 + x;
                const auto redSelector = x < 2 ? 0ull : 1ull;
                const auto greenSelector = y < 2 ? 0ull : 1ull;
                redSelectors |= redSelector << (texel * 3);
                greenSelectors |= greenSelector << (texel * 3);
            }
        }
        for (std::size_t byte = 0; byte < 6; ++byte) {
            subresource.bytes[2 + byte] = static_cast<std::byte>(
                (redSelectors >> (byte * 8)) & 0xFFu);
            subresource.bytes[10 + byte] = static_cast<std::byte>(
                (greenSelectors >> (byte * 8)) & 0xFFu);
        }
    }
    fixture.subresources.push_back(std::move(subresource));
    return fixture;
}

// A single-texel texture with an exact value. One texel removes any
// disagreement between hardware and oracle filtering rules from the
// comparison, which is the same fixture discipline Phase 14 used, while
// still making a wrong decode a wrong pixel.
texture::CapturedTexture BuildSolidTexture(
    const std::uint64_t resourceId,
    const std::array<std::uint8_t, 4> value,
    // Base colour is authored in sRGB and everything else is data, which the
    // material boundary enforces rather than assumes.
    const texture::TextureFormat viewFormat =
        texture::TextureFormat::R8G8B8A8Unorm)
{
    texture::CapturedTexture fixture{};
    fixture.resourceId = resourceId;
    fixture.generation = 1;
    fixture.width = 1;
    fixture.height = 1;
    // A typeless resource is what lets the view choose sRGB or linear, which
    // is exactly the engine's own arrangement.
    fixture.resourceFormat = texture::TextureFormat::R8G8B8A8Typeless;
    fixture.viewFormat = viewFormat;
    fixture.sampler.minFilter = texture::TextureFilter::Nearest;
    fixture.sampler.magFilter = texture::TextureFilter::Nearest;
    fixture.sampler.mipFilter = texture::TextureFilter::Nearest;
    fixture.sampler.maxLod = 0.0f;
    texture::TextureSubresource level{};
    level.width = 1;
    level.height = 1;
    level.rowPitch = 4;
    level.slicePitch = 4;
    level.bytes = {
        static_cast<std::byte>(value[0]), static_cast<std::byte>(value[1]),
        static_cast<std::byte>(value[2]), static_cast<std::byte>(value[3])};
    fixture.subresources.push_back(std::move(level));
    return fixture;
}

// The normal texel is chosen so the two declared encodings reconstruct
// visibly different vectors from the *same* bytes: the tangent path reads
// two channels, rebuilds Z, and rotates into the surface frame, while the
// model-space path reads three and is already absolute.
//
// Both decodes must also land inside the geometric hemisphere. The fixture's
// surfaces face -Z, so the blue channel is below the midpoint: a texel with
// blue above it decodes to a model-space normal pointing away from the
// surface, the horizon lift then flattens both decodes onto the same plane,
// and the comparison silently measures nothing. That is exactly what an
// earlier version of this fixture did.
constexpr std::array<std::uint8_t, 4> kFamilyNormalTexel{140, 115, 5, 255};

material::MaterialReplayBundle BuildFamilyMaterialFixture()
{
    material::MaterialReplayBundle bundle{};
    bundle.textures[0] = BuildSolidTexture(
        0x8000'0000'0000'1601ull, {255, 255, 255, 255},
        texture::TextureFormat::R8G8B8A8UnormSrgb);
    bundle.textures[1] = BuildSolidTexture(
        0x8000'0000'0000'1602ull, kFamilyNormalTexel);
    bundle.textures[2] = BuildSolidTexture(
        0x8000'0000'0000'1603ull, {128, 200, 0, 255});
    material::MaterialCapture capture{};
    capture.materialId = 0x9000'0000'0000'1601ull;
    capture.generation = 1;
    capture.revision = 1;
    capture.staticRevision = 1;
    capture.textures = {
        {material::MaterialTextureRole::BaseColor,
            bundle.textures[0].resourceId, bundle.textures[0].generation,
            bundle.textures[0].viewFormat,
            material::MaterialProvenance::TextureSet},
        {material::MaterialTextureRole::Normal,
            bundle.textures[1].resourceId, bundle.textures[1].generation,
            bundle.textures[1].viewFormat,
            material::MaterialProvenance::TextureSet},
        {material::MaterialTextureRole::SmoothSpec,
            bundle.textures[2].resourceId, bundle.textures[2].generation,
            bundle.textures[2].viewFormat,
            material::MaterialProvenance::TextureSet},
    };
    capture.smoothness = {
        {1.0f, material::MaterialProvenance::RootMaterial}};
    capture.specularScale = {
        {1.0f, material::MaterialProvenance::RootMaterial}};
    capture.specularColor = {
        {{0.65f, 0.12f, 0.04f},
            material::MaterialProvenance::RootMaterial}};
    if (material::TranslateMaterial(capture, bundle.material) !=
        material::MaterialError::None) {
        return {};
    }
    bundle.transferVersion = material::kMaterialTransferVersion;
    return bundle;
}

material::MaterialReplayBundle BuildMaterialFixture()
{
    material::MaterialReplayBundle bundle{};
    bundle.textures[0] = BuildTextureFixture();
    bundle.textures[0].resourceId = 0x8000'0000'0000'2001ull;
    bundle.textures[1] = BuildBc5Fixture(
        0x8000'0000'0000'2002ull, false);
    bundle.textures[2] = BuildBc5Fixture(
        0x8000'0000'0000'2003ull, true);
    material::MaterialCapture capture{};
    capture.materialId = 0x9000'0000'0000'2001ull;
    capture.generation = 1;
    capture.revision = 1;
    capture.staticRevision = 1;
    capture.textures = {
        {material::MaterialTextureRole::BaseColor,
            bundle.textures[0].resourceId, bundle.textures[0].generation,
            bundle.textures[0].viewFormat,
            material::MaterialProvenance::TextureSet},
        {material::MaterialTextureRole::Normal,
            bundle.textures[1].resourceId, bundle.textures[1].generation,
            bundle.textures[1].viewFormat,
            material::MaterialProvenance::TextureSet},
        {material::MaterialTextureRole::SmoothSpec,
            bundle.textures[2].resourceId, bundle.textures[2].generation,
            bundle.textures[2].viewFormat,
            material::MaterialProvenance::TextureSet},
    };
    capture.smoothness = {
        {1.0f, material::MaterialProvenance::RootMaterial}};
    capture.specularScale = {
        {1.0f, material::MaterialProvenance::RootMaterial}};
    capture.specularColor = {
        {{0.65f, 0.12f, 0.04f},
            material::MaterialProvenance::RootMaterial}};
    if (material::TranslateMaterial(capture, bundle.material) !=
        material::MaterialError::None) {
        return {};
    }
    bundle.transferVersion = material::kMaterialTransferVersion;
    return bundle;
}

bool ReadBinaryFile(
    const std::filesystem::path& path,
    std::uintmax_t maximumBytes,
    std::vector<std::byte>& bytes);

bool LoadMaterial(
    const RenderOptions& options,
    std::vector<std::byte>& bytes,
    material::MaterialReplayBundle& decoded,
    bool& enabled)
{
    enabled = options.materialFixture || !options.materialInput.empty();
    if (!enabled) return true;
    if (options.materialFixture && !options.materialInput.empty()) return false;
    if (options.materialFixture) {
        const auto source = BuildMaterialFixture();
        if (material::EncodeMaterialReplayBundle(source, bytes) !=
            material::MaterialPacketError::None) return false;
    } else {
        constexpr std::uintmax_t kMaximumMaterialBytes =
            192u * 1024u * 1024u;
        if (!ReadBinaryFile(options.materialInput,
                kMaximumMaterialBytes, bytes)) return false;
    }
    return material::DecodeMaterialReplayBundle(bytes, decoded) ==
        material::MaterialPacketError::None;
}

bool LoadTexture(
    const RenderOptions& options,
    std::vector<std::byte>& bytes,
    texture::CapturedTexture& decoded,
    bool& enabled)
{
    enabled = options.textureFixture || !options.textureInput.empty();
    if (!enabled) return true;
    if (options.textureFixture && !options.textureInput.empty()) return false;
    if (options.textureFixture) {
        decoded = BuildTextureFixture();
        return texture::EncodeCapturedTexture(decoded, bytes) ==
            texture::TexturePacketError::None;
    }
    constexpr std::uintmax_t kMaximumTextureBytes = 64u * 1024u * 1024u;
    if (!ReadBinaryFile(options.textureInput,
            kMaximumTextureBytes, bytes)) {
        return false;
    }
    return texture::DecodeCapturedTexture(bytes, decoded) ==
        texture::TexturePacketError::None;
}

bool ReadBinaryFile(
    const std::filesystem::path& path,
    const std::uintmax_t maximumBytes,
    std::vector<std::byte>& bytes)
{
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize > maximumBytes ||
        fileSize > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    try {
        bytes.resize(static_cast<std::size_t>(fileSize));
    } catch (...) {
        return false;
    }
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    return !input.bad() &&
        static_cast<std::size_t>(input.gcount()) == bytes.size();
}

bool WriteBinaryFile(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) return false;
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    return output.good();
}

// Prints every view a captured frame packet carries so a live capture can be
// told apart from a synthetic one, and a world camera from a near-field one.
int InspectFrame(const std::filesystem::path& path)
{
    std::vector<std::byte> bytes;
    if (!ReadBinaryFile(path, 16u * 1024u * 1024u, bytes)) {
        std::cerr << "vf_packet_replay: cannot read frame packet\n";
        return 3;
    }
    view::FramePacket packet;
    const auto decoded = view::DecodeFramePacket(bytes, packet);
    if (decoded != view::FramePacketError::None) {
        std::cerr << "vf_packet_replay: invalid frame packet: "
                  << view::ToString(decoded) << '\n';
        return 4;
    }
    std::cout << "frame packet bytes=" << bytes.size()
              << " frame=" << packet.header.frameId
              << " engine-frame=" << packet.header.engineFrameId
              << " epoch=" << packet.header.historyEpoch
              << " views=" << packet.views.size()
              << " passes=" << packet.passes.size()
              << " thread=" << packet.header.renderThreadId << '\n';
    for (std::size_t index = 0; index < packet.views.size(); ++index) {
        const auto& record = packet.views[index];
        // A world-to-camera matrix stores the translation of the camera in
        // its fourth column; reporting it makes movement between two
        // captures directly comparable.
        std::cout << "  view[" << index << "]"
                  << " id=0x" << std::hex << record.viewId << std::dec
                  << " extent=" << record.outputWidth << 'x'
                  << record.outputHeight
                  << " near=" << record.nearPlane
                  << " far=" << record.farPlane
                  << " fov-deg=" << record.verticalFovRadians * 57.2957795f
                  << " translation=" << record.view.elements[3] << ','
                  << record.view.elements[7] << ','
                  << record.view.elements[11]
                  << " validation="
                  << view::ToString(view::ValidateView(record)) << '\n';
        // The rows of a world-to-camera matrix are the camera basis in world
        // space. A camera that follows the player turns these; a static or
        // secondary camera does not.
        std::cout << "    right=" << record.view.elements[0] << ','
                  << record.view.elements[1] << ','
                  << record.view.elements[2]
                  << " up=" << record.view.elements[4] << ','
                  << record.view.elements[5] << ','
                  << record.view.elements[6]
                  << " forward=" << record.view.elements[8] << ','
                  << record.view.elements[9] << ','
                  << record.view.elements[10] << '\n';
    }
    return 0;
}

view::SourceMatrix4 SourceFromCanonical(const view::Matrix4& matrix)
{
    view::SourceMatrix4 source{};
    source.storage = view::MatrixStorage::RowMajor;
    source.vectors = view::VectorConvention::ColumnVector;
    std::copy(std::begin(matrix.elements), std::end(matrix.elements),
        std::begin(source.elements));
    return source;
}

bool BuildFrameFixture(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t frameIndex,
    view::FramePacket& packet)
{
    packet = {};
    if (width == 0 || height == 0 || frameIndex == 0) return false;

    view::CapturedView captured{};
    captured.viewId = 0xA000'0000'0000'1001ull;
    captured.cameraId = 0xA000'0000'0000'2001ull;
    captured.projectionMode = view::ProjectionMode::Perspective;
    captured.handedness = view::Handedness::LeftHanded;
    captured.flags = view::ViewCameraRelative | view::ViewUsesJitter;
    captured.renderMode = 0;
    captured.targetId = 2;
    captured.outputWidth = width;
    captured.outputHeight = height;
    captured.aaMode = 1;
    captured.renderScale = 1.0f;
    captured.nearPlane = 0.1f;
    captured.farPlane = 100.0f;
    captured.verticalFovRadians = 1.0471975512f;
    captured.jitterNdc = {
        0.375f / static_cast<float>(width),
        -0.25f / static_cast<float>(height)};
    captured.previousJitterNdc = captured.jitterNdc;
    captured.cameraRelativeOrigin = {
        1'000'000.0, -2'000'000.0, 512.0};
    captured.previousCameraRelativeOrigin =
        captured.cameraRelativeOrigin;
    captured.viewport = {0.0f, 0.0f,
        static_cast<float>(width), static_cast<float>(height),
        0.0f, 1.0f};
    captured.scissor = {0, 0, width, height};
    const auto identity = view::IdentityMatrix();
    const auto projection = view::BuildPerspectiveProjection(
        captured.verticalFovRadians,
        static_cast<float>(width) / static_cast<float>(height),
        captured.nearPlane, captured.farPlane,
        captured.handedness, captured.jitterNdc);
    captured.view = SourceFromCanonical(identity);
    captured.projection = SourceFromCanonical(projection);
    captured.previousView = SourceFromCanonical(identity);
    captured.previousProjection = SourceFromCanonical(projection);

    view::ViewRecordV1 translated{};
    if (view::TranslateCapturedView(captured, translated) !=
        view::ViewError::None) {
        return false;
    }

    packet.header.frameId = frameIndex;
    packet.header.engineFrameId =
        0xE000'0000'0000'0000ull | frameIndex;
    packet.header.historyEpoch = 1;
    packet.header.captureSequence =
        0xC000'0000'0000'0000ull | frameIndex;
    packet.header.captureThreadId = 1;
    packet.header.renderThreadId = 1;
    packet.views.push_back(translated);

    view::PassRecordV1 pass{};
    pass.sequence = 1;
    pass.viewId = translated.viewId;
    pass.domain = view::ShaderDomain::Lighting;
    pass.technique = 0x1234;
    pass.renderMode = captured.renderMode;
    pass.targetId = captured.targetId;
    pass.flags = view::PassWritesWorldTarget;
    pass.category = view::ClassifyPass(
        pass.domain, pass.renderMode, pass.flags);
    packet.passes.push_back(pass);
    return true;
}

bool LoadFrame(
    const RenderOptions& options,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t frameIndex,
    std::vector<std::byte>& bytes,
    view::FramePacket& decoded,
    bool& enabled)
{
    constexpr std::uintmax_t kMaximumFrameBytes = 16u * 1024u * 1024u;
    enabled = options.viewFixture || !options.frameInput.empty();
    bytes.clear();
    decoded = {};
    if (!enabled) return options.frameOutput.empty();
    if (options.viewFixture && !options.frameInput.empty()) return false;

    if (options.viewFixture) {
        view::FramePacket source;
        if (!BuildFrameFixture(width, height, frameIndex, source) ||
            view::EncodeFramePacket(source, bytes) !=
                view::FramePacketError::None) {
            return false;
        }
    } else if (!ReadBinaryFile(
            options.frameInput, kMaximumFrameBytes, bytes)) {
        return false;
    }

    if (view::DecodeFramePacket(bytes, decoded) !=
            view::FramePacketError::None ||
        decoded.views.empty() ||
        options.frameView >= decoded.views.size() ||
        decoded.header.frameId != frameIndex) {
        return false;
    }
    // A live capture carries every camera the engine held. The replay
    // reconstructs one of them, so the packet is narrowed to the selected
    // view and the passes that belong to it. A single-view capture with the
    // default index is unchanged, byte for byte.
    if (decoded.views.size() > 1) {
        const auto selected = decoded.views[options.frameView];
        view::FramePacket single{};
        single.header = decoded.header;
        try {
            single.views.push_back(selected);
            for (const auto& pass : decoded.passes) {
                if (pass.viewId == selected.viewId) {
                    single.passes.push_back(pass);
                }
            }
        } catch (...) {
            return false;
        }
        if (single.passes.empty()) return false;
        std::vector<std::byte> narrowed;
        if (view::EncodeFramePacket(single, narrowed) !=
            view::FramePacketError::None) {
            return false;
        }
        bytes = std::move(narrowed);
        decoded = std::move(single);
    }
    return options.frameOutput.empty() ||
        WriteBinaryFile(options.frameOutput, bytes);
}

bool BuildViewSpacePacket(
    const raster::DecodedPacket& desiredNdc,
    const view::ViewRecordV1& capturedView,
    std::vector<std::byte>& bytes,
    raster::DecodedPacket& source,
    bool& sourceDiffersFromNdc)
{
    sourceDiffersFromNdc = false;
    try {
        source = desiredNdc;
        for (std::size_t vertexIndex = 0;
             vertexIndex < source.vertices.size(); ++vertexIndex) {
            const auto& desired = desiredNdc.vertices[vertexIndex];
            double homogeneous[4]{};
            const double ndc[]{desired.position[0], desired.position[1],
                desired.position[2], 1.0};
            for (std::size_t row = 0; row < 4; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    homogeneous[row] += static_cast<double>(
                        capturedView.inverseViewProjection.elements[
                            row * 4 + column]) * ndc[column];
                }
            }
            if (!std::isfinite(homogeneous[0]) ||
                !std::isfinite(homogeneous[1]) ||
                !std::isfinite(homogeneous[2]) ||
                !std::isfinite(homogeneous[3]) ||
                std::abs(homogeneous[3]) <= 1.0e-12) {
                return false;
            }
            for (std::size_t component = 0; component < 3; ++component) {
                const auto value = homogeneous[component] / homogeneous[3];
                if (!std::isfinite(value) ||
                    std::abs(value) >
                        std::numeric_limits<float>::max()) {
                    return false;
                }
                const auto raw = static_cast<float>(value);
                sourceDiffersFromNdc = sourceDiffersFromNdc ||
                    std::abs(raw - desired.position[component]) > 1.0e-4f;
                source.vertices[vertexIndex].position[component] = raw;
            }
        }
        const auto encoded = raster::EncodePacket(source, bytes);
        if (!encoded) return false;
        raster::DecodedPacket verified;
        if (!raster::DecodePacket(bytes, verified)) return false;
        source = std::move(verified);
        return true;
    } catch (...) {
        bytes.clear();
        source = {};
        return false;
    }
}

bool WritePpm(
    const std::filesystem::path& path,
    const raster::RasterImage& image)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (const auto pixel : image.pixels) {
        const char rgb[]{
            static_cast<char>(pixel.r),
            static_cast<char>(pixel.g),
            static_cast<char>(pixel.b),
        };
        output.write(rgb, sizeof(rgb));
    }
    return output.good();
}

std::array<float, 3> Normalize3(std::array<float, 3> value)
{
    const auto length = std::sqrt(value[0] * value[0] +
        value[1] * value[1] + value[2] * value[2]);
    if (!std::isfinite(length) || length <= 1.0e-10f) {
        return {0.0f, 0.0f, 1.0f};
    }
    for (auto& component : value) component /= length;
    return value;
}

bool ParseSceneRenderOptions(
    const int argc,
    const char* const* argv,
    const int firstOption,
    SceneRenderOptions& options)
{
    for (int index = firstOption; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--backend" && index + 1 < argc) {
            options.backend = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else if (argument == "--scene-output" && index + 1 < argc) {
            options.sceneOutput = argv[++index];
        } else if (argument == "--gbuffer-output" && index + 1 < argc) {
            options.gbufferOutput = argv[++index];
        } else if (argument == "--width" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.width)) return false;
        } else if (argument == "--height" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.height)) return false;
        } else if (argument == "--validation") {
            options.validation = true;
        } else {
            return false;
        }
    }
    return !options.backend.empty() && !options.output.empty() &&
        options.width >= 32 && options.height >= 32 &&
        options.width <= raster::kMaximumExtent &&
        options.height <= raster::kMaximumExtent;
}

view::PassRecordV1 BuildScenePass(
    const std::uint64_t sequence,
    const view::ShaderDomain domain,
    const std::uint32_t flags)
{
    view::PassRecordV1 pass{};
    pass.sequence = sequence;
    pass.viewId = kSceneViewId;
    pass.domain = domain;
    pass.technique = 0x11'0000u | static_cast<std::uint32_t>(sequence);
    pass.renderMode = 0;
    pass.targetId = 2;
    pass.flags = flags;
    pass.category = view::ClassifyPass(
        pass.domain, pass.renderMode, pass.flags);
    return pass;
}

bool BuildSceneFrame(
    const std::uint32_t width,
    const std::uint32_t height,
    const bool includeUnclassifiedWorldWriter,
    view::FramePacket& packet,
    std::vector<std::byte>& bytes,
    // A detached cell stops emitting its opaque pass, so the mirrored frame
    // must stop claiming it as well.
    const bool includeDistantTreePass = true,
    // Terrain cells are thousands of units across, so the terrain fixture
    // needs clip planes that can actually contain one.
    const float farPlane = 100.0f,
    const float nearPlane = 0.1f)
{
    packet = {};
    bytes.clear();
    if (width == 0 || height == 0) return false;

    view::CapturedView captured{};
    captured.viewId = kSceneViewId;
    captured.cameraId = kSceneCameraId;
    captured.projectionMode = view::ProjectionMode::Perspective;
    captured.handedness = view::Handedness::LeftHanded;
    captured.flags = view::ViewCameraRelative | view::ViewUsesJitter;
    captured.renderMode = 0;
    captured.targetId = 2;
    captured.outputWidth = width;
    captured.outputHeight = height;
    captured.aaMode = 1;
    captured.renderScale = 1.0f;
    captured.nearPlane = nearPlane;
    captured.farPlane = farPlane;
    captured.verticalFovRadians = 1.0471975512f;
    captured.jitterNdc = {
        0.25f / static_cast<float>(width),
        -0.125f / static_cast<float>(height)};
    captured.previousJitterNdc = captured.jitterNdc;
    captured.cameraRelativeOrigin = {2'000'000.0, 512.0, -1'000'000.0};
    captured.previousCameraRelativeOrigin = captured.cameraRelativeOrigin;
    captured.viewport = {0.0f, 0.0f,
        static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    captured.scissor = {0, 0, width, height};
    const auto identity = view::IdentityMatrix();
    const auto projection = view::BuildPerspectiveProjection(
        captured.verticalFovRadians,
        static_cast<float>(width) / static_cast<float>(height),
        captured.nearPlane, captured.farPlane,
        captured.handedness, captured.jitterNdc);
    captured.view = SourceFromCanonical(identity);
    captured.projection = SourceFromCanonical(projection);
    captured.previousView = SourceFromCanonical(identity);
    captured.previousProjection = SourceFromCanonical(projection);

    view::ViewRecordV1 translated{};
    if (view::TranslateCapturedView(captured, translated) !=
        view::ViewError::None) {
        return false;
    }
    packet.header.frameId = kSceneFrameIndex;
    packet.header.engineFrameId = 0xE000'0000'0000'1100ull | kSceneFrameIndex;
    packet.header.historyEpoch = 1;
    packet.header.captureSequence =
        0xC000'0000'0000'1100ull | kSceneFrameIndex;
    packet.header.captureThreadId = 1;
    packet.header.renderThreadId = 1;
    packet.views.push_back(translated);
    // Two opaque world passes are mirrored. The sky pass is a declared
    // unsupported class that vanilla still owns.
    packet.passes.push_back(BuildScenePass(1, view::ShaderDomain::Lighting,
        view::PassWritesWorldTarget));
    if (includeDistantTreePass) {
        packet.passes.push_back(BuildScenePass(2,
            view::ShaderDomain::DistantTree, view::PassWritesWorldTarget));
    }
    packet.passes.push_back(BuildScenePass(3, view::ShaderDomain::Sky,
        view::PassWritesWorldTarget));
    if (includeUnclassifiedWorldWriter) {
        packet.passes.push_back(BuildScenePass(4,
            view::ShaderDomain::Effect, view::PassWritesWorldTarget));
    }
    return view::EncodeFramePacket(packet, bytes) ==
        view::FramePacketError::None;
}

bool BuildSceneSource(
    const std::uint32_t width,
    const std::uint32_t height,
    raster::DecodedPacket& source,
    std::vector<std::byte>& bytes)
{
    source = {};
    bytes.clear();
    // Local-space triangles. The captured view negates Y, so this
    // clockwise local order projects to counter-clockwise packet NDC.
    const std::array<std::array<float, 3>, 3> localPositions{{
        {-0.80f, -0.70f, 0.0f},
        {0.0f, 0.90f, 0.0f},
        {0.80f, -0.70f, 0.0f},
    }};
    const std::array<std::array<float, 2>, 3> localTexCoords{{
        {0.0f, 1.0f},
        {0.5f, 0.0f},
        {1.0f, 1.0f},
    }};
    // Object 0 interpolates vertex color; objects 1 and 2 are flat so the
    // rotated object cannot confuse perspective-correct interpolation with
    // the oracle's screen-space interpolation.
    const std::array<std::array<std::array<float, 3>, 3>, 3> colors{{
        {{{0.90f, 0.25f, 0.15f}, {0.20f, 0.85f, 0.30f},
          {0.15f, 0.30f, 0.95f}}},
        {{{1.00f, 0.00f, 1.00f}, {1.00f, 0.00f, 1.00f},
          {1.00f, 0.00f, 1.00f}}},
        {{{0.35f, 0.75f, 0.85f}, {0.35f, 0.75f, 0.85f},
          {0.35f, 0.75f, 0.85f}}},
    }};
    const std::array<std::array<float, 4>, 3> baseColors{{
        {1.00f, 0.95f, 0.90f, 1.0f},
        {0.50f, 0.50f, 0.50f, 1.0f},
        {0.80f, 0.90f, 1.00f, 1.0f},
    }};

    source.header.frameIndex = kSceneFrameIndex;
    source.header.width = width;
    source.header.height = height;
    source.header.viewportWidth = static_cast<float>(width);
    source.header.viewportHeight = static_cast<float>(height);
    source.header.viewportMaxDepth = 1.0f;
    source.header.scissorWidth = width;
    source.header.scissorHeight = height;
    source.header.indexType = raster::IndexType::Uint16;
    for (std::uint32_t object = 0; object < kSceneObjectCount; ++object) {
        for (std::uint32_t vertex = 0; vertex < 3; ++vertex) {
            raster::RasterVertexV3 value{};
            std::copy(localPositions[vertex].begin(),
                localPositions[vertex].end(), value.position);
            std::copy(colors[object][vertex].begin(),
                colors[object][vertex].end(), value.color);
            std::copy(localTexCoords[vertex].begin(),
                localTexCoords[vertex].end(), value.texCoord);
            // The source triangles face -Z in local space, so this is the
            // local form of the same normal the object record carries: the
            // vertex stage rotates it by the model and arrives at exactly
            // that value. Leaving it at the +Z default would point every
            // surface away from the one the fixture describes.
            value.normal[0] = 0.0f;
            value.normal[1] = 0.0f;
            value.normal[2] = -1.0f;
            source.vertices.push_back(value);
            source.indices.push_back(object * 3 + vertex);
        }
        const auto materialId = kSceneMaterialIdBase + object;
        raster::RasterMaterialV1 material{};
        material.resourceId = materialId;
        std::copy(baseColors[object].begin(), baseColors[object].end(),
            material.baseColor);
        source.materials.push_back(material);
        source.draws.push_back({materialId, object * 3, 3, 0,
            raster::FrontFace::CounterClockwise,
            raster::DepthCompare::Less, 0});
    }
    source.header.vertexCount =
        static_cast<std::uint32_t>(source.vertices.size());
    source.header.indexCount =
        static_cast<std::uint32_t>(source.indices.size());
    source.header.drawCount =
        static_cast<std::uint32_t>(source.draws.size());
    source.header.materialCount =
        static_cast<std::uint32_t>(source.materials.size());
    if (!raster::EncodePacket(source, bytes)) return false;
    raster::DecodedPacket verified;
    if (!raster::DecodePacket(bytes, verified)) return false;
    source = std::move(verified);
    return true;
}

void SetSceneModel(
    scene::OpaqueObjectV1& object,
    const float scale,
    const float yawRadians,
    const std::array<float, 3>& translation)
{
    const auto cosine = std::cos(yawRadians);
    const auto sine = std::sin(yawRadians);
    std::fill(std::begin(object.model), std::end(object.model), 0.0f);
    object.model[0] = cosine * scale;
    object.model[2] = sine * scale;
    object.model[3] = translation[0];
    object.model[5] = scale;
    object.model[7] = translation[1];
    object.model[8] = -sine * scale;
    object.model[10] = cosine * scale;
    object.model[11] = translation[2];
    object.model[15] = 1.0f;
    std::copy(std::begin(object.model), std::end(object.model),
        std::begin(object.previousModel));
    // The source triangles face -Z in local space; rotation carries the
    // geometric normal, and the shading normal is deliberately distinct.
    const std::array<float, 3> geometric{-sine, 0.0f, -cosine};
    const auto shading = Normalize3({geometric[0], geometric[1] + 0.15f,
        geometric[2]});
    for (std::size_t axis = 0; axis < 3; ++axis) {
        object.geometricNormal[axis] = geometric[axis];
        object.shadingNormal[axis] = shading[axis];
    }
}

bool BuildSceneObjects(scene::ScenePacket& packet)
{
    packet = {};
    packet.header.frameId = kSceneFrameIndex;
    packet.header.viewId = kSceneViewId;
    packet.header.captureSequence =
        0xC000'0000'0000'1100ull | kSceneFrameIndex;
    packet.header.captureThreadId = 1;
    packet.header.renderThreadId = 1;
    const std::array<float, kSceneObjectCount> roughness{
        0.15f, 0.45f, 0.80f};
    for (std::uint32_t index = 0; index < kSceneObjectCount; ++index) {
        scene::OpaqueObjectV1 object{};
        object.objectId = kSceneObjectIdBase + index;
        object.materialId = kSceneMaterialIdBase + index;
        object.drawIndex = index;
        object.passSequence = index == 2 ? 2 : 1;
        object.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
        object.roughness = roughness[index];
        object.boundsMinimum[0] = -0.80f;
        object.boundsMinimum[1] = -0.70f;
        object.boundsMaximum[0] = 0.80f;
        object.boundsMaximum[1] = 0.90f;
        packet.objects.push_back(object);
    }
    // Object 1 projects strictly inside object 0 but four units away, so it
    // must never survive the depth test. Object 2 is rotated and overlaps
    // object 0's right edge from behind.
    SetSceneModel(packet.objects[0], 1.0f, 0.0f, {-0.35f, 0.05f, 2.0f});
    SetSceneModel(packet.objects[1], 1.9f, 0.0f, {-0.70f, 0.10f, 4.0f});
    SetSceneModel(packet.objects[2], 1.0f, 0.4363323130f,
        {0.30f, -0.10f, 2.60f});
    return scene::ValidateScenePacket(packet) ==
        scene::ScenePacketError::None;
}

std::uint64_t CountObjectPixels(
    const std::span<const scene::GBufferPixelV1> pixels,
    const std::uint64_t objectId)
{
    return static_cast<std::uint64_t>(std::count_if(
        pixels.begin(), pixels.end(),
        [objectId](const scene::GBufferPixelV1& pixel) {
            return (static_cast<std::uint64_t>(pixel.objectId[0]) |
                (static_cast<std::uint64_t>(pixel.objectId[1]) << 32)) ==
                objectId;
        }));
}

struct InteriorComparison
{
    std::uint64_t interiorPixels{};
    std::uint64_t mismatchedPixels{};
    float maximumInteriorError{};
};

// Silhouette pixels legitimately differ between the oracle's screen-space
// coverage rule and hardware rasterization. Every pixel whose whole 3x3
// neighbourhood belongs to one object must agree exactly.
InteriorComparison CompareInteriorPixels(
    const scene::GBufferImage& expected,
    const std::span<const scene::GBufferPixelV1> actual,
    const float tolerance)
{
    InteriorComparison comparison{};
    if (expected.pixels.size() != actual.size() ||
        expected.width < 3 || expected.height < 3) {
        comparison.mismatchedPixels = expected.pixels.size();
        comparison.maximumInteriorError =
            std::numeric_limits<float>::infinity();
        return comparison;
    }
    const auto identity = [](const scene::GBufferPixelV1& pixel) {
        return std::array<std::uint32_t, 4>{pixel.objectId[0],
            pixel.objectId[1], pixel.materialId[0], pixel.materialId[1]};
    };
    for (std::uint32_t y = 1; y + 1 < expected.height; ++y) {
        for (std::uint32_t x = 1; x + 1 < expected.width; ++x) {
            const auto& center = expected.At(x, y);
            const auto centerIdentity = identity(center);
            bool interior = true;
            for (std::int32_t dy = -1; dy <= 1 && interior; ++dy) {
                for (std::int32_t dx = -1; dx <= 1 && interior; ++dx) {
                    interior = identity(expected.At(
                        static_cast<std::uint32_t>(
                            static_cast<std::int32_t>(x) + dx),
                        static_cast<std::uint32_t>(
                            static_cast<std::int32_t>(y) + dy))) ==
                        centerIdentity;
                }
            }
            if (!interior) continue;
            ++comparison.interiorPixels;
            const auto& observed = actual[
                static_cast<std::size_t>(y) * expected.width + x];
            bool mismatched = identity(observed) != centerIdentity;
            const float* expectedChannels[]{center.albedo,
                center.geometricNormalRoughness, center.shadingNormalDepth};
            const float* actualChannels[]{observed.albedo,
                observed.geometricNormalRoughness,
                observed.shadingNormalDepth};
            for (std::size_t group = 0; group < 3; ++group) {
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    const auto error = std::abs(
                        expectedChannels[group][channel] -
                        actualChannels[group][channel]);
                    if (!std::isfinite(error)) {
                        comparison.maximumInteriorError =
                            std::numeric_limits<float>::infinity();
                        mismatched = true;
                        continue;
                    }
                    comparison.maximumInteriorError = std::max(
                        comparison.maximumInteriorError, error);
                    mismatched = mismatched || error > tolerance;
                }
            }
            if (mismatched) ++comparison.mismatchedPixels;
        }
    }
    return comparison;
}

// The Phase 15 cutout fixture. Alpha comes from the base-colour texture, so a
// checkerboard of opaque and clear texels produces a real silhouette rather
// than an all-or-nothing object. Nearest filtering keeps the GPU and the
// oracle on the same sampling rule; the remaining disagreement is confined to
// cutout edges, which the interior comparison already excludes.
texture::CapturedTexture BuildAlphaCutoutTexture()
{
    texture::CapturedTexture fixture{};
    fixture.resourceId = 0x8000'0000'0000'15C1ull;
    fixture.generation = 1;
    fixture.width = 4;
    fixture.height = 4;
    fixture.resourceFormat = texture::TextureFormat::R8G8B8A8Unorm;
    fixture.viewFormat = texture::TextureFormat::R8G8B8A8Unorm;
    fixture.sampler.minFilter = texture::TextureFilter::Nearest;
    fixture.sampler.magFilter = texture::TextureFilter::Nearest;
    fixture.sampler.mipFilter = texture::TextureFilter::Nearest;
    fixture.sampler.maxLod = 0.0f;
    texture::TextureSubresource level{};
    level.width = 4;
    level.height = 4;
    level.rowPitch = 16;
    level.slicePitch = 64;
    level.bytes.resize(64);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const auto texel = (y * 4 + x) * 4;
            // A 2x2 block checker: coarse enough that whole pixel
            // neighbourhoods fall inside one cell of the pattern.
            const auto opaque = (((x / 2) + (y / 2)) % 2) == 0;
            level.bytes[texel + 0] = std::byte{255};
            level.bytes[texel + 1] = std::byte{255};
            level.bytes[texel + 2] = std::byte{255};
            level.bytes[texel + 3] = opaque ? std::byte{255} : std::byte{0};
        }
    }
    fixture.subresources.push_back(std::move(level));
    return fixture;
}

// Phase 11's objects with visibility records attached: object 0 is the
// cutout, object 2 is two-sided, and object 1 stays occluded as it always
// was so the earlier fixtures' geometry is unchanged.
// Each visible object carries a different family so one frame exercises
// every check the phase gate names. Object 1 is deliberately occluded by the
// Phase 11 fixture and has no coverage in any render, so it is left as the
// ordinary lit surface and proves the implicit-record path instead.
// One directional light, one point light near the visible geometry, ambient,
// and fog. Two light types rather than one, because a fixture with only a
// directional light cannot tell an attenuation bug from a working renderer.
// The occluder set the reference traces against: every projected triangle,
// at the camera-relative positions the shader shades in. This is the same
// geometry the backend's bottom level is built from, which is the point --
// a reference tracing a different set would disagree with the mirror for
// reasons that have nothing to do with the shadow rule.
std::vector<accel::ShadowTriangle> BuildOccluders(
    const raster::DecodedPacket& projected,
    const std::span<const std::array<float, 3>> vertexPositions,
    const std::span<const std::uint32_t> blendedDraws = {})
{
    std::vector<accel::ShadowTriangle> occluders;
    if (vertexPositions.size() != projected.vertices.size()) return occluders;
    occluders.reserve(projected.indices.size() / 3);
    // Walked per draw, because an index is relative to its draw's vertex
    // offset. Reading the index buffer as one flat stream addresses the wrong
    // vertices for every draw after the first, and the triangles that come
    // out are somewhere else entirely -- which reads as "nothing occludes".
    for (std::size_t drawSlot = 0; drawSlot < projected.draws.size();
        ++drawSlot) {
        const auto& draw = projected.draws[drawSlot];
        // Blended geometry is not an occluder. A particle or a pane of glass
        // casting a hard opaque shadow is wrong, and it reads as the shadow
        // pass being broken rather than as the wrong geometry being in the
        // acceleration structure.
        if (std::find(blendedDraws.begin(), blendedDraws.end(),
                static_cast<std::uint32_t>(drawSlot)) != blendedDraws.end()) {
            continue;
        }
        for (std::uint32_t offset = 0; offset + 2 < draw.indexCount;
             offset += 3) {
            const auto first =
                static_cast<std::size_t>(draw.firstIndex) + offset;
            if (first + 2 >= projected.indices.size()) break;
            const auto resolve =
                [&](const std::size_t slot) -> std::size_t {
                    return static_cast<std::size_t>(
                        static_cast<std::int64_t>(projected.indices[slot]) +
                        draw.vertexOffset);
                };
            const auto a = resolve(first);
            const auto b = resolve(first + 1);
            const auto c = resolve(first + 2);
            if (a >= vertexPositions.size() || b >= vertexPositions.size() ||
                c >= vertexPositions.size()) {
                continue;
            }
            accel::ShadowTriangle triangle{};
            triangle.a = vertexPositions[a];
            triangle.b = vertexPositions[b];
            triangle.c = vertexPositions[c];
            // Opaque, matching VK_GEOMETRY_OPAQUE_BIT_KHR on the geometry the
            // backend builds.
            triangle.opacity = accel::GeometryOpacity::Opaque;
            occluders.push_back(triangle);
        }
    }
    return occluders;
}

// The same triangles the occluder set holds, plus what a reflection hit needs
// to be shaded: the object's own geometric normal and the albedo the shader
// reads at a hit. The ray query recovers which geometry it struck but has no
// vertex attributes bound, so both sides read the per-object family record or
// they cannot agree about what the reflection shows.
std::vector<reflect::ReflectionTriangle> BuildReflectionGeometry(
    const raster::DecodedPacket& projected,
    const std::span<const std::array<float, 3>> vertexPositions,
    const scene::ScenePacket& scenePacket,
    const material::FamilyPacket& families)
{
    std::vector<reflect::ReflectionTriangle> geometry;
    if (vertexPositions.size() != projected.vertices.size()) return geometry;
    for (std::size_t objectIndex = 0;
         objectIndex < scenePacket.objects.size(); ++objectIndex) {
        const auto& object = scenePacket.objects[objectIndex];
        if (object.drawIndex >= projected.draws.size()) continue;
        const auto& draw = projected.draws[object.drawIndex];
        const auto record = material::BuildFamilyGpuRecord(
            material::ResolveFamilyRecord(families, object.objectId));
        for (std::uint32_t offset = 0; offset + 2 < draw.indexCount;
             offset += 3) {
            const auto first =
                static_cast<std::size_t>(draw.firstIndex) + offset;
            if (first + 2 >= projected.indices.size()) break;
            const auto resolve = [&](const std::size_t slot) -> std::size_t {
                return static_cast<std::size_t>(
                    static_cast<std::int64_t>(projected.indices[slot]) +
                    draw.vertexOffset);
            };
            const auto a = resolve(first);
            const auto b = resolve(first + 1);
            const auto c = resolve(first + 2);
            if (a >= vertexPositions.size() || b >= vertexPositions.size() ||
                c >= vertexPositions.size()) {
                continue;
            }
            reflect::ReflectionTriangle triangle{};
            triangle.a = vertexPositions[a];
            triangle.b = vertexPositions[b];
            triangle.c = vertexPositions[c];
            triangle.normal = {object.geometricNormal[0],
                object.geometricNormal[1], object.geometricNormal[2]};
            triangle.albedo = {record.tintColor[0], record.tintColor[1],
                record.tintColor[2]};
            // The instance disables triangle culling, so a reflection sees
            // the back of a surface exactly as the ray query does.
            triangle.twoSided = true;
            geometry.push_back(triangle);
        }
    }
    return geometry;
}

bool BuildFixtureLights(
    const scene::ScenePacket& scene,
    lighting::LightPacket& lights)
{
    lights = {};
    lights.header.frameId = scene.header.frameId;
    lights.header.viewId = scene.header.viewId;

    lighting::LightCapture sun{};
    sun.lightId = 0x1700'0000'0000'0001ull;
    sun.type = lighting::LightType::Directional;
    sun.diffuse = {1.0f, 0.94f, 0.86f};
    sun.dimmer = 1.6f;
    sun.direction = {0.35f, 0.25f, -1.0f};
    lighting::LightRecordV1 sunRecord{};
    if (lighting::TranslateLight(sun, sunRecord) !=
        lighting::LightError::None) {
        return false;
    }
    lights.lights.push_back(sunRecord);

    lighting::LightCapture lamp{};
    lamp.lightId = 0x1700'0000'0000'0002ull;
    lamp.type = lighting::LightType::Point;
    lamp.diffuse = {0.35f, 0.55f, 1.0f};
    lamp.dimmer = 3.0f;
    lamp.radius = 6.0f;
    lamp.constantAttenuation = 1.0f;
    lamp.linearAttenuation = 0.25f;
    lamp.quadraticAttenuation = 0.05f;
    // Placed beside the fixture geometry so its falloff is visible across
    // the frame rather than uniform.
    lamp.position = {-1.5, 0.5, 2.0};
    lighting::LightRecordV1 lampRecord{};
    if (lighting::TranslateLight(lamp, lampRecord) !=
        lighting::LightError::None) {
        return false;
    }
    lights.lights.push_back(lampRecord);

    lighting::EnvironmentCapture environment{};
    environment.ambient = {0.12f, 0.13f, 0.18f};
    environment.sunDirection = sun.direction;
    environment.sunColor = sun.diffuse;
    environment.sunIntensity = sun.dimmer;
    environment.fog.nearDistance = 2.0f;
    environment.fog.farDistance = 12.0f;
    environment.fog.color = {0.45f, 0.50f, 0.60f};
    environment.fog.power = 1.0f;
    environment.fog.maximum = 0.85f;
    if (lighting::TranslateEnvironment(environment, lights.environment) !=
        lighting::LightError::None) {
        return false;
    }
    return lighting::ValidateLightPacket(lights) ==
        lighting::LightPacketError::None;
}

bool BuildFamilySceneObjects(
    scene::ScenePacket& scene,
    material::FamilyPacket& families)
{
    if (!BuildSceneObjects(scene)) return false;
    families = {};
    families.header.frameId = scene.header.frameId;
    families.header.viewId = scene.header.viewId;

    const auto describe = [&scene, &families](
        const std::size_t index,
        const material::MaterialFamily family,
        const std::uint64_t flags,
        const std::array<float, 3> tint,
        const std::array<float, 3> emit,
        const float emitScale) {
        material::FamilyCapture capture{};
        capture.materialId = scene.objects[index].materialId;
        capture.generation = 1;
        capture.revision = 1;
        capture.staticRevision = 1;
        capture.featureId = material::FeatureIdOf(family);
        capture.propertyFlags = flags;
        capture.baseTechniqueId = 0x1600;
        capture.tintColor = tint;
        capture.emitColor = emit;
        capture.emitScale = emitScale;
        capture.subsurfaceRolloff = 0.4f;
        capture.rimPower = 2.0f;
        capture.backlightPower = 1.5f;
        // Base colour and the normal map are the two slots this fixture
        // binds; the rest stay unauthored so a family that required one
        // would be refused rather than silently rendered flat.
        capture.slots[0].resourceId = 0x8000'0000'0000'1601ull;
        capture.slots[0].generation = 1;
        capture.slots[0].authored = true;
        capture.slots[1].resourceId = 0x8000'0000'0000'1602ull;
        capture.slots[1].generation = 1;
        capture.slots[1].authored = true;
        capture.slots[7].resourceId = 0x8000'0000'0000'1603ull;
        capture.slots[7].generation = 1;
        capture.slots[7].authored = true;
        material::FamilyDescriptor descriptor;
        if (material::TranslateMaterialFamily(capture, descriptor) !=
            material::FamilyError::None) {
            return false;
        }
        families.records.push_back(material::MakeFamilyRecord(
            descriptor, scene.objects[index].objectId));
        return true;
    };

    // Object 0: a tinted, anisotropic hair surface reading model-space
    // normals. It carries the tint, lobe, and model-space normal checks.
    if (!describe(0, material::MaterialFamily::HairTint,
            material::PropertyFlag::HairTint |
                material::PropertyFlag::AnisotropicLighting |
                material::PropertyFlag::ModelSpaceNormals,
            {0.35f, 0.85f, 0.55f}, {0.0f, 0.0f, 0.0f}, 1.0f)) {
        return false;
    }
    // Object 2: an ordinary surface that declares its own emission and reads
    // tangent-space normals. It carries the emission and tangent normal
    // checks, and its bright emit colour is only emissive because the flag
    // says so.
    if (!describe(2, material::MaterialFamily::Default,
            material::PropertyFlag::OwnEmit,
            {1.0f, 1.0f, 1.0f}, {2.0f, 1.0f, 0.5f}, 3.0f)) {
        return false;
    }
    return material::ValidateFamilyPacket(families) ==
        material::FamilyPacketError::None &&
        scene::ValidateScenePacket(scene) == scene::ScenePacketError::None;
}

// Appends the phase 18 shadow fixture on top of the phase 16/17 scene: a
// small occluder and the light it blocks. Appending rather than editing the
// shared builders is deliberate -- phase 17's artifacts are recorded by hash,
// and changing its scene would re-baseline them for a reason unrelated to it.
//
// Geometry that decides the test: the source triangles face -Z, so a light in
// front of the objects lights them. The lamp sits at z 0.6, the occluder at
// z 1.3, and object 0 receives at z 2.0, all offset in X so the occluder
// shades object 0's middle while sitting beside it on screen rather than in
// front of it.
// Two blended quads in front of the opaque scene, in different modes and at
// different depths. Two is the minimum that can show an order at all, and the
// modes have to differ because two additive layers commute -- a fixture built
// from those would pass with the sort reversed, which is exactly what the
// first version of this contract did.
bool AppendTransparencyFixture(
    raster::DecodedPacket& source,
    scene::ScenePacket& scenePacket,
    material::FamilyPacket& families)
{
    struct Layer
    {
        blend::BlendMode mode;
        blend::EffectDomain domain;
        float sortDepth;
        float roughness;
        std::array<float, 3> offset;
        std::array<float, 3> tint;
        // Whether the layer carries radiance of its own. The reactive mask's
        // additive rule is a maximum of the alpha and the radiance, so one
        // layer can only ever exercise whichever of the two is larger.
        bool emits;
        // The alpha the draw declares. Never read by additive blending, which
        // takes source and destination at full weight, so it changes the mask
        // and nothing about the composite.
        float alpha;
        // The volume a decal projects into. Zero for a layer that is not a
        // decal, which is every layer that composites as ordinary blended
        // geometry.
        float decalRange;
        float decalRadius;
    };
    // The near one is straight alpha, so it must be composited last for the
    // far additive layer to show through it. Reversing the order changes the
    // result, which is what makes the sort observable.
    const std::array<Layer, 4> layers{{
        // Concentric and both in front of every opaque object (the nearest
        // sits at z=1.30), so they genuinely overlap on screen and both
        // survive the depth test. Two layers are necessary for an order to
        // be observable but not sufficient: apart on screen there is no
        // overlap for the order to matter in, and behind the opaque geometry
        // only one of them draws.
        {blend::BlendMode::Additive, blend::EffectDomain::Fire, 6.0f, 0.30f,
            {0.20f, 0.10f, 0.90f}, {0.90f, 0.35f, 0.10f}, true, 0.20f,
            0.0f, 0.0f},
        // Refractive, so it reads the snapshot of what is behind it rather
        // than compositing straight over the live target.
        // Premultiplied, so the hardware blend contributes nothing from the
        // destination: with alpha at one the only route from the geometry
        // behind this quad to its pixels is the snapshot. Straight alpha
        // cannot discriminate, because dst(1-a) carries the background
        // through whether or not the shader reads anything.
        {blend::BlendMode::Premultiplied, blend::EffectDomain::Refractive,
            3.0f, 0.85f,
            {-0.10f, -0.05f, 0.60f}, {0.15f, 0.45f, 0.95f}, false, 1.0f,
            0.0f, 0.0f},
        // A dim additive layer with an alpha that dominates its radiance,
        // off to the side so it overlaps neither of the others: additive only
        // ever brightens, and away from the pair it cannot disturb the order
        // the two of them exist to make observable. It is the only draw in the
        // fixture whose reactive mask comes from its alpha rather than its
        // radiance, which is the half of the maximum the bright layer above
        // can never reach.
        {blend::BlendMode::Additive, blend::EffectDomain::Particle, 9.0f,
            0.40f, {-0.95f, 0.55f, 1.10f}, {0.05f, 0.05f, 0.06f}, false,
            0.60f, 0.0f, 0.0f},
        // A decal. Its radius is a third of the quad it is evaluated over, so
        // the projection has to clip the quad down to a disc -- a volume that
        // reached every fragment would prove the shader ran and nothing more.
        // Multiply is the mode the engine uses for marks that darken rather
        // than cover, which is what a scorch mark is.
        {blend::BlendMode::Multiply, blend::EffectDomain::Decal, 12.0f, 0.55f,
            {0.95f, -0.45f, 1.05f}, {0.35f, 0.30f, 0.28f}, false, 1.0f,
            // A range shorter than the quad measures along the tilted axis, so
            // both ends of it fall outside and the range rejects them. The
            // radius is generous on purpose: with both tight, whichever rule
            // is checked first would be the only one ever observed.
            0.30f, 0.50f},
    }};

    for (const auto& layer : layers) {
        const auto objectIndex =
            static_cast<std::uint32_t>(scenePacket.objects.size());
        const auto materialId = kSceneMaterialIdBase + 0x40 + objectIndex;
        const auto objectId = kSceneObjectIdBase + 0x40 + objectIndex;
        const std::array<std::array<float, 3>, 3> localPositions{{
            {-0.55f, -0.50f, 0.0f},
            {0.0f, 0.65f, 0.0f},
            {0.55f, -0.50f, 0.0f},
        }};
        const std::array<std::array<float, 2>, 3> localTexCoords{{
            {0.0f, 1.0f}, {0.5f, 0.0f}, {1.0f, 1.0f},
        }};
        const auto firstIndex =
            static_cast<std::uint32_t>(source.indices.size());
        for (std::uint32_t vertex = 0; vertex < 3; ++vertex) {
            raster::RasterVertexV3 value{};
            std::copy(localPositions[vertex].begin(),
                localPositions[vertex].end(), value.position);
            std::copy(layer.tint.begin(), layer.tint.end(), value.color);
            std::copy(localTexCoords[vertex].begin(),
                localTexCoords[vertex].end(), value.texCoord);
            value.normal[0] = 0.0f;
            value.normal[1] = 0.0f;
            value.normal[2] = -1.0f;
            source.vertices.push_back(value);
            source.indices.push_back(
                static_cast<std::uint32_t>(source.vertices.size() - 1));
        }
        raster::RasterMaterialV1 material{};
        material.resourceId = materialId;
        // The additive layer is deliberately dim in alpha and bright in
        // radiance, which is the only shape that makes the reactive mask's
        // additive rule observable: additive blending takes source and
        // destination at full weight and never reads this alpha, so lowering
        // it leaves the composited image untouched while making a mask that
        // returned the alpha differ from one that weighs the radiance. With
        // alpha at one the two agree and the rule cannot be tested at all.
        const std::array<float, 4> baseColor{
            layer.tint[0], layer.tint[1], layer.tint[2], layer.alpha};
        std::copy(baseColor.begin(), baseColor.end(), material.baseColor);
        source.materials.push_back(material);
        source.draws.push_back({materialId, firstIndex, 3, 0,
            raster::FrontFace::CounterClockwise,
            raster::DepthCompare::Less, 0});

        scene::OpaqueObjectV1 object{};
        object.objectId = objectId;
        object.materialId = materialId;
        object.drawIndex = static_cast<std::uint32_t>(
            source.draws.size() - 1);
        object.passSequence = 1;
        object.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
        // Deliberately unlike the surfaces these sit over. A blended draw
        // whose surface data matches what it covers cannot show whether the
        // pass wrote the G-buffer it was supposed to leave alone.
        object.roughness = layer.roughness;
        object.boundsMinimum[0] = -0.55f;
        object.boundsMinimum[1] = -0.50f;
        object.boundsMaximum[0] = 0.55f;
        object.boundsMaximum[1] = 0.65f;
        scenePacket.objects.push_back(object);
        SetSceneModel(scenePacket.objects.back(), 0.45f, 0.0f, layer.offset);

        material::FamilyCapture capture{};
        capture.materialId = materialId;
        capture.generation = 1;
        capture.revision = 1;
        capture.staticRevision = 1;
        capture.featureId =
            material::FeatureIdOf(material::MaterialFamily::Default);
        capture.baseTechniqueId = 0x2100;
        capture.tintColor = layer.tint;
        // The fire layer emits. A spark that carries no radiance of its own is
        // not the case the additive blend exists for, and it is also the case
        // where the reactive mask cannot be told from the alpha: the rule
        // weighs radiance precisely because an additive draw contributes it
        // whatever its alpha says.
        const auto emissive = layer.emits;
        capture.emitColor = emissive ? layer.tint
            : std::array<float, 3>{0.0f, 0.0f, 0.0f};
        capture.emitScale = emissive ? 2.0f : 1.0f;
        if (emissive) {
            // Emission is enabled by the property the engine sets, not by a
            // colour being present: a capture carrying a colour without the
            // flag describes a material that does not glow.
            capture.propertyFlags |= static_cast<std::uint64_t>(
                material::PropertyFlag::OwnEmit);
        }
        capture.subsurfaceRolloff = 0.4f;
        capture.rimPower = 2.0f;
        capture.backlightPower = 1.5f;
        // Base and normal are required of every family; without them the
        // translation refuses the capture rather than binding whatever
        // happens to be resident.
        capture.slots[0].resourceId = 0x8000'0000'0000'2101ull;
        capture.slots[0].generation = 1;
        capture.slots[0].authored = true;
        capture.slots[1].resourceId = 0x8000'0000'0000'2102ull;
        capture.slots[1].generation = 1;
        capture.slots[1].authored = true;
        capture.slots[7].resourceId = 0x8000'0000'0000'2103ull;
        capture.slots[7].generation = 1;
        capture.slots[7].authored = true;
        material::FamilyDescriptor descriptor;
        const auto familyTranslated =
            material::TranslateMaterialFamily(capture, descriptor);
        if (familyTranslated != material::FamilyError::None) {
            std::cerr << "transparency-fixture: family error="
                      << material::ToString(familyTranslated) << (char)10;
            return false;
        }
        families.records.push_back(
            material::MakeFamilyRecord(descriptor, objectId));

        scene::TransparentDrawRecordV1 blended{};
        blended.drawId = 0x2100'0000'0000'0001ull + objectIndex;
        blended.materialId = materialId;
        blended.objectIndex = objectIndex;
        blended.blend = static_cast<std::uint32_t>(layer.mode);
        blended.domain = static_cast<std::uint32_t>(layer.domain);
        blended.sortDepth = layer.sortDepth;
        // The volume a decal projects into. Sized so it clips its own quad
        // rather than covering it: a projection that happens to reach every
        // fragment of the geometry it is evaluated over proves only that the
        // shader ran, not that it projected anything.
        if (layer.decalRadius > 0.0f) {
            // Tilted relative to the quad rather than perpendicular to it. A
            // projection whose axis is normal to the surface reaches every
            // fragment at the same distance along it, so the range never
            // decides anything and a fixture built that way cannot tell a
            // renderer that honours the range from one that ignores it.
            const std::array<float, 3> axis{0.8f, 0.0f, 0.6f};
            const std::array<float, 3> origin{
                layer.offset[0] - axis[0] * layer.decalRange * 0.5f,
                layer.offset[1] - axis[1] * layer.decalRange * 0.5f,
                layer.offset[2] - axis[2] * layer.decalRange * 0.5f};
            std::copy(origin.begin(), origin.end(), blended.decalOrigin);
            std::copy(axis.begin(), axis.end(), blended.decalAxis);
            blended.decalRange = layer.decalRange;
            blended.decalRadius = layer.decalRadius;
        }
        scenePacket.transparent.push_back(blended);
    }

    source.header.vertexCount =
        static_cast<std::uint32_t>(source.vertices.size());
    source.header.indexCount =
        static_cast<std::uint32_t>(source.indices.size());
    source.header.drawCount =
        static_cast<std::uint32_t>(source.draws.size());
    source.header.materialCount =
        static_cast<std::uint32_t>(source.materials.size());
    const auto sceneValid = scene::ValidateScenePacket(scenePacket);
    if (sceneValid != scene::ScenePacketError::None) {
        std::cerr << "transparency-fixture: scene invalid error="
                  << scene::ToString(sceneValid) << " draws=";
        for (const auto& entry : scenePacket.objects) {
            std::cerr << entry.drawIndex << " ";
        }
        std::cerr << (char)10;
        return false;
    }
    return true;
}

// A scene built for one purpose: to show whether the device's reflection ray
// finds geometry. Two objects and nothing else, because every other thing in
// a frame -- a second light, a shadow caster, the diffuse bounce -- reaches
// the mirror as well and drowns the term being measured. Four attempts to
// isolate a reflection inside the family fixture failed for exactly that
// reason, each one measuring a different confound.
//
// The geometry is computed rather than chosen. The camera sits at the origin
// looking down +Z. A mirror at (0, 0, 3) turned by 45 degrees has a surface
// normal of (-sin45, 0, -cos45), and for a view direction of (0, 0, -1) the
// reflected direction is (-sin90, 0, -cos90) = (-1, 0, 0) exactly. The target
// therefore goes at (-1.5, 0, 3): squarely along that ray, and still inside
// the frustum so the render can be looked at.
bool BuildMirrorFixture(
    const std::uint32_t width,
    const std::uint32_t height,
    const bool includeTarget,
    const float mirrorRoughness,
    raster::DecodedPacket& source,
    scene::ScenePacket& scenePacket)
{
    source = {};
    scenePacket = {};
    source.header.frameIndex = kSceneFrameIndex;
    source.header.width = width;
    source.header.height = height;
    source.header.viewportWidth = static_cast<float>(width);
    source.header.viewportHeight = static_cast<float>(height);
    source.header.viewportMaxDepth = 1.0f;
    source.header.scissorWidth = width;
    source.header.scissorHeight = height;
    source.header.indexType = raster::IndexType::Uint16;

    scenePacket.header.frameId = kSceneFrameIndex;
    scenePacket.header.viewId = kSceneViewId;
    scenePacket.header.captureSequence =
        0xC000'0000'0000'1900ull | kSceneFrameIndex;
    scenePacket.header.captureThreadId = 1;
    scenePacket.header.renderThreadId = 1;

    struct Piece
    {
        float scale;
        float yaw;
        std::array<float, 3> translation;
        float roughness;
        std::array<float, 3> colour;
    };
    std::vector<Piece> pieces{
        // The mirror: smooth enough to trace, turned so its reflection points
        // along -X rather than back at the camera. A mirror facing the viewer
        // can only reflect what lies between them, and anything there would
        // occlude it -- which is why the family fixture's reflective plate
        // never had anything to find.
        {1.4f, 0.7853982f, {0.0f, 0.0f, 3.0f}, mirrorRoughness,
            {0.55f, 0.57f, 0.60f}},
    };
    if (includeTarget) {
        // Squarely along the reflected direction, and bright enough that a
        // reflection carrying even a few per cent of it clears the noise.
        // Turned to face the mirror. The reflected ray travels along -X in
        // the plane z = 3, so a target lying in that same plane is parallel
        // to the ray and can never be intersected however brightly it is
        // lit -- a quad facing +X is perpendicular to it instead.
        pieces.push_back(
            {3.0f, -1.5707963f, {-1.5f, 0.0f, 3.0f}, 0.85f,
                {6.0f, 0.35f, 0.10f}});
    }

    const std::array<std::array<float, 3>, 3> localPositions{{
        {-1.0f, -1.0f, 0.0f}, {0.0f, 1.2f, 0.0f}, {1.0f, -1.0f, 0.0f},
    }};
    const std::array<std::array<float, 2>, 3> localTexCoords{{
        {0.0f, 1.0f}, {0.5f, 0.0f}, {1.0f, 1.0f},
    }};
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const auto& piece = pieces[index];
        const auto materialId =
            kSceneMaterialIdBase + 0x60 + static_cast<std::uint32_t>(index);
        const auto firstIndex =
            static_cast<std::uint32_t>(source.indices.size());
        for (std::uint32_t vertex = 0; vertex < 3; ++vertex) {
            raster::RasterVertexV3 value{};
            std::copy(localPositions[vertex].begin(),
                localPositions[vertex].end(), value.position);
            std::copy(piece.colour.begin(), piece.colour.end(), value.color);
            std::copy(localTexCoords[vertex].begin(),
                localTexCoords[vertex].end(), value.texCoord);
            value.normal[2] = -1.0f;
            source.vertices.push_back(value);
            source.indices.push_back(
                static_cast<std::uint32_t>(source.vertices.size() - 1));
        }
        raster::RasterMaterialV1 material{};
        material.resourceId = materialId;
        const std::array<float, 4> baseColor{
            piece.colour[0], piece.colour[1], piece.colour[2], 1.0f};
        std::copy(baseColor.begin(), baseColor.end(), material.baseColor);
        source.materials.push_back(material);
        source.draws.push_back({materialId, firstIndex, 3, 0,
            raster::FrontFace::CounterClockwise,
            raster::DepthCompare::Less, 0});

        scene::OpaqueObjectV1 object{};
        object.objectId =
            kSceneObjectIdBase + 0x60 + static_cast<std::uint32_t>(index);
        object.materialId = materialId;
        object.drawIndex = static_cast<std::uint32_t>(index);
        object.passSequence = 1;
        object.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
        object.roughness = piece.roughness;
        object.boundsMinimum[0] = -1.0f;
        object.boundsMinimum[1] = -1.0f;
        object.boundsMaximum[0] = 1.0f;
        object.boundsMaximum[1] = 1.2f;
        scenePacket.objects.push_back(object);
        SetSceneModel(scenePacket.objects.back(), piece.scale, piece.yaw,
            piece.translation);
    }
    source.header.vertexCount =
        static_cast<std::uint32_t>(source.vertices.size());
    source.header.indexCount =
        static_cast<std::uint32_t>(source.indices.size());
    source.header.drawCount =
        static_cast<std::uint32_t>(source.draws.size());
    source.header.materialCount =
        static_cast<std::uint32_t>(source.materials.size());
    return scene::ValidateScenePacket(scenePacket) ==
        scene::ScenePacketError::None;
}

bool AppendShadowFixture(
    raster::DecodedPacket& source,
    scene::ScenePacket& scenePacket,
    material::FamilyPacket& families,
    lighting::LightPacket& lights,
    const std::array<double, 3>& cameraOrigin,
    const bool reflective)
{
    const auto objectIndex = static_cast<std::uint32_t>(kSceneObjectCount);
    const auto materialId = kSceneMaterialIdBase + objectIndex;
    const auto objectId = kSceneObjectIdBase + objectIndex;
    const std::array<std::array<float, 3>, 3> localPositions{{
        {-0.80f, -0.70f, 0.0f},
        {0.0f, 0.90f, 0.0f},
        {0.80f, -0.70f, 0.0f},
    }};
    const std::array<std::array<float, 2>, 3> localTexCoords{{
        {0.0f, 1.0f}, {0.5f, 0.0f}, {1.0f, 1.0f},
    }};
    const auto firstIndex =
        static_cast<std::uint32_t>(source.indices.size());
    for (std::uint32_t vertex = 0; vertex < 3; ++vertex) {
        raster::RasterVertexV3 value{};
        std::copy(localPositions[vertex].begin(),
            localPositions[vertex].end(), value.position);
        const std::array<float, 3> flat{0.70f, 0.70f, 0.72f};
        std::copy(flat.begin(), flat.end(), value.color);
        std::copy(localTexCoords[vertex].begin(),
            localTexCoords[vertex].end(), value.texCoord);
        // Local -Z, the same convention every fixture here uses, so the
        // rotated vertex normal reproduces the object record exactly.
        value.normal[0] = 0.0f;
        value.normal[1] = 0.0f;
        value.normal[2] = -1.0f;
        source.vertices.push_back(value);
        source.indices.push_back(
            static_cast<std::uint32_t>(source.vertices.size() - 1));
    }
    raster::RasterMaterialV1 material{};
    material.resourceId = materialId;
    const std::array<float, 4> baseColor{0.85f, 0.85f, 0.88f, 1.0f};
    std::copy(baseColor.begin(), baseColor.end(), material.baseColor);
    source.materials.push_back(material);
    source.draws.push_back({materialId, firstIndex, 3, 0,
        raster::FrontFace::CounterClockwise,
        raster::DepthCompare::Less, 0});
    source.header.vertexCount =
        static_cast<std::uint32_t>(source.vertices.size());
    source.header.indexCount =
        static_cast<std::uint32_t>(source.indices.size());
    source.header.drawCount =
        static_cast<std::uint32_t>(source.draws.size());
    source.header.materialCount =
        static_cast<std::uint32_t>(source.materials.size());

    scene::OpaqueObjectV1 occluder{};
    occluder.objectId = objectId;
    occluder.materialId = materialId;
    occluder.drawIndex = objectIndex;
    occluder.passSequence = 1;
    occluder.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
    // A smooth metal when the reflection fixture asks for one: the same
    // object then carries a lobe narrow enough to trace and an F0 high
    // enough to see, which is what makes the reflection measurable at all.
    occluder.roughness = reflective ? 0.05f : 0.60f;
    occluder.boundsMinimum[0] = -0.80f;
    occluder.boundsMinimum[1] = -0.70f;
    occluder.boundsMaximum[0] = 0.80f;
    occluder.boundsMaximum[1] = 0.90f;
    scenePacket.objects.push_back(occluder);
    SetSceneModel(scenePacket.objects.back(), 0.35f, 0.0f,
        {-0.95f, 0.05f, 1.30f});
    const auto sceneValid = scene::ValidateScenePacket(scenePacket);
    if (sceneValid != scene::ScenePacketError::None) {
        std::cerr << "shadow-fixture: scene invalid error="
                  << scene::ToString(sceneValid) << " ids=";
        for (const auto& entry : scenePacket.objects) {
            std::cerr << std::hex << entry.objectId << ":"
                      << entry.drawIndex << " ";
        }
        std::cerr << std::dec << (char)10;
        return false;
    }

    material::FamilyCapture capture{};
    capture.materialId = materialId;
    capture.generation = 1;
    capture.revision = 1;
    capture.staticRevision = 1;
    capture.featureId = material::FeatureIdOf(reflective
        ? material::MaterialFamily::EnvironmentMap
        : material::MaterialFamily::Default);
    capture.baseTechniqueId = 0x1600;
    capture.tintColor = {1.0f, 1.0f, 1.0f};
    capture.emitColor = {0.0f, 0.0f, 0.0f};
    capture.emitScale = 1.0f;
    capture.subsurfaceRolloff = 0.4f;
    capture.rimPower = 2.0f;
    capture.backlightPower = 1.5f;
    capture.slots[0].resourceId = 0x8000'0000'0000'1601ull;
    capture.slots[0].generation = 1;
    capture.slots[0].authored = true;
    capture.slots[1].resourceId = 0x8000'0000'0000'1602ull;
    capture.slots[1].generation = 1;
    capture.slots[1].authored = true;
    capture.slots[7].resourceId = 0x8000'0000'0000'1603ull;
    capture.slots[7].generation = 1;
    capture.slots[7].authored = true;
    material::FamilyDescriptor descriptor;
    const auto familyTranslated =
        material::TranslateMaterialFamily(capture, descriptor);
    if (familyTranslated != material::FamilyError::None) {
        std::cerr << "shadow-fixture: family error="
                  << material::ToString(familyTranslated) << (char)10;
        return false;
    }
    families.records.push_back(
        material::MakeFamilyRecord(descriptor, objectId));

    lighting::LightCapture blocked{};
    blocked.lightId = 0x1800'0000'0000'0003ull;
    blocked.type = lighting::LightType::Point;
    blocked.diffuse = {1.0f, 0.85f, 0.60f};
    blocked.dimmer = 4.0f;
    blocked.radius = 8.0f;
    blocked.constantAttenuation = 1.0f;
    blocked.linearAttenuation = 0.10f;
    blocked.quadraticAttenuation = 0.02f;
    // In front of the objects, off to the same side as the occluder. Authored
    // in world space because BuildGpuLight narrows a light against the
    // camera origin: a position written camera-relative lands the whole
    // camera origin away and falls outside its own radius, which is how the
    // two lights this fixture inherited came to contribute nothing at all.
    blocked.position = {cameraOrigin[0] - 1.60, cameraOrigin[1] + 0.05,
        cameraOrigin[2] + 0.60};
    lighting::LightRecordV1 blockedRecord{};
    const auto lightTranslated =
        lighting::TranslateLight(blocked, blockedRecord);
    if (lightTranslated != lighting::LightError::None) {
        std::cerr << "shadow-fixture: light error="
                  << lighting::ToString(lightTranslated) << (char)10;
        return false;
    }
    lights.lights.push_back(blockedRecord);
    return lighting::ValidateLightPacket(lights) ==
        lighting::LightPacketError::None;
}

bool BuildAlphaSceneObjects(scene::ScenePacket& packet)
{
    if (!BuildSceneObjects(packet)) return false;
    for (std::size_t index = 0; index < packet.objects.size(); ++index) {
        visibility::VisibilityRecordV1 record{};
        record.objectId = packet.objects[index].objectId;
        record.materialId = packet.objects[index].materialId;
        record.alpha.constantAlpha = 1.0f;
        record.alpha.fade = 1.0f;
        record.modelDeterminant = 1.0f;
        record.faceMode = visibility::FaceMode::FrontOnly;
        if (index == 0) {
            record.alpha.classification = visibility::AlphaClass::Tested;
            record.alpha.source = visibility::AlphaSource::BaseColorTexture;
            record.alpha.reference = 0.5f;
        } else {
            record.alpha.classification = visibility::AlphaClass::Opaque;
            record.alpha.source = visibility::AlphaSource::None;
        }
        if (index == 2) record.faceMode = visibility::FaceMode::TwoSided;
        packet.visibility.push_back(record);
    }
    return scene::ValidateScenePacket(packet) ==
        scene::ScenePacketError::None;
}

// The Phase 14 exterior fixture. The camera sits at a Commonwealth-scale
// world origin so cell placement has to survive the double-to-float
// narrowing, and the two cells share an edge so a crack would show up as a
// seam of uncovered pixels between them.
// Two thousand units in front of the frame's camera height. The far/near
// ratio stays at the same 1000:1 the other fixtures use, because a captured
// view only round-trips its clip planes through float within that margin.
constexpr double kTerrainCellHeightOrigin = -998'000.0;
constexpr float kTerrainNearPlane = 4.0f;
constexpr float kTerrainFarPlane = 4'000.0f;
constexpr std::uint32_t kTerrainLayerCount = 3;
constexpr std::uint64_t kTerrainLayerTextureIdBase =
    0x7000'0000'0000'0001ull;

texture::CapturedTexture BuildTerrainLayerArray()
{
    texture::CapturedTexture fixture{};
    fixture.resourceId = 0x8000'0000'0000'14A1ull;
    fixture.generation = 1;
    fixture.dimension = texture::TextureDimension::Texture2DArray;
    fixture.width = 1;
    fixture.height = 1;
    fixture.arrayLayers = kTerrainLayerCount;
    fixture.resourceFormat = texture::TextureFormat::R8G8B8A8Unorm;
    fixture.viewFormat = texture::TextureFormat::R8G8B8A8Unorm;
    fixture.sampler.minFilter = texture::TextureFilter::Nearest;
    fixture.sampler.magFilter = texture::TextureFilter::Nearest;
    fixture.sampler.mipFilter = texture::TextureFilter::Nearest;
    fixture.sampler.maxLod = 0.0f;
    // One flat colour per slice. A wrong slice selection is then a wrong
    // pixel rather than a subtle shading difference, and a single texel per
    // slice keeps the GPU and the oracle on identical sampling rules.
    const std::array<std::array<std::uint8_t, 4>, kTerrainLayerCount>
        sliceColors{{
            {{224, 64, 32, 255}},
            {{48, 208, 96, 255}},
            {{64, 96, 240, 255}}}};
    for (std::uint32_t slice = 0; slice < kTerrainLayerCount; ++slice) {
        texture::TextureSubresource subresource{};
        subresource.arrayLayer = slice;
        subresource.width = 1;
        subresource.height = 1;
        subresource.rowPitch = 4;
        subresource.slicePitch = 4;
        subresource.bytes.resize(4);
        for (std::size_t channel = 0; channel < 4; ++channel) {
            subresource.bytes[channel] =
                static_cast<std::byte>(sliceColors[slice][channel]);
        }
        fixture.subresources.push_back(std::move(subresource));
    }
    return fixture;
}

terrain::TerrainPacket BuildTerrainFixture()
{
    terrain::TerrainPacket packet{};
    packet.header.frameId = kSceneFrameIndex;
    packet.header.viewId = kSceneViewId;
    packet.header.captureSequence =
        0xC000'0000'0000'1400ull | kSceneFrameIndex;
    packet.header.captureThreadId = 1;
    packet.header.renderThreadId = 1;
    const std::array<float, kTerrainLayerCount> roughness{
        0.40f, 0.50f, 0.60f};
    for (std::uint32_t index = 0; index < kTerrainLayerCount; ++index) {
        terrain::LandscapeLayerV1 layer{};
        layer.textureId = kTerrainLayerTextureIdBase + index;
        layer.arraySlice = index;
        layer.flags = index == 0 ? terrain::LayerIsBase : 0u;
        layer.uvScale[0] = 1.0f + static_cast<float>(index);
        layer.uvScale[1] = 1.0f + static_cast<float>(index);
        layer.roughness = roughness[index];
        layer.normalStrength = 1.0f;
        packet.layers.push_back(layer);
    }

    constexpr float kSize = static_cast<float>(terrain::kCellWorldSize);
    // Grid 487/488 straddles the camera's world X, so the shared edge lands
    // inside the viewport where a crack would actually be visible.
    const std::array<std::int32_t, 2> gridX{487, 488};
    for (std::uint32_t cellIndex = 0; cellIndex < gridX.size(); ++cellIndex) {
        terrain::TerrainCellV1 cell{};
        cell.gridX = gridX[cellIndex];
        cell.gridY = 0;
        cell.cellId = (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(cell.gridX)) << 32) |
            static_cast<std::uint32_t>(cell.gridY);
        cell.lodLevel = cellIndex;
        cell.firstVertex = cellIndex * 4;
        cell.vertexCount = 4;
        cell.firstIndex = cellIndex * 6;
        cell.indexCount = 6;
        cell.originX =
            static_cast<double>(cell.gridX) * terrain::kCellWorldSize;
        cell.originY = 0.0;
        cell.originZ = kTerrainCellHeightOrigin;
        // The far cell drops to LOD 1, so the shared edge is a near/far
        // transition rather than two cells at the same detail level.
        cell.lodMorphStart = cellIndex == 0 ? 4096.0f : 8192.0f;
        cell.lodMorphEnd = cellIndex == 0 ? 8192.0f : 16384.0f;
        cell.flags = terrain::CellWritesWorldTarget;
        cell.layerSlotCount = kTerrainLayerCount;
        // The second cell permutes its slots, so a shader that ignores the
        // per-cell mapping renders the wrong layer colours.
        for (std::uint32_t slot = 0; slot < kTerrainLayerCount; ++slot) {
            cell.layerSlots[slot] = cellIndex == 0
                ? slot : (kTerrainLayerCount - 1 - slot);
        }
        cell.boundsMinimumZ = 0.0f;
        cell.boundsMaximumZ = 64.0f;
        packet.cells.push_back(cell);
    }

    const std::array<std::array<float, terrain::kLandChannelsPerVertex>, 4>
        channels{{
            {{1.00f, 0.00f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {{0.50f, 0.30f, 0.20f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {{0.20f, 0.60f, 0.20f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {{0.00f, 0.00f, 1.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        }};
    // Corner heights. The shared edge repeats identical heights so the seam
    // is watertight; a crack would need them to disagree.
    const std::array<std::array<float, 4>, 2> heights{{
        {{12.0f, 26.0f, 34.0f, 48.0f}},
        {{26.0f, 18.0f, 48.0f, 40.0f}},
    }};
    for (std::uint32_t cellIndex = 0; cellIndex < 2; ++cellIndex) {
        const std::array<std::array<float, 2>, 4> locals{{
            {{0.0f, 0.0f}}, {{kSize, 0.0f}},
            {{0.0f, kSize}}, {{kSize, kSize}}}};
        for (std::uint32_t corner = 0; corner < 4; ++corner) {
            terrain::LandscapeVertexV1 vertex{};
            vertex.position[0] = locals[corner][0];
            vertex.position[1] = locals[corner][1];
            vertex.position[2] = heights[cellIndex][corner];
            vertex.normal[2] = 1.0f;
            vertex.color[0] = 0.85f + 0.05f * static_cast<float>(cellIndex);
            vertex.color[1] = 0.90f;
            vertex.color[2] = 0.95f - 0.05f * static_cast<float>(cellIndex);
            vertex.color[3] = 1.0f;
            std::copy(channels[corner].begin(), channels[corner].end(),
                std::begin(vertex.channels));
            packet.vertices.push_back(vertex);
        }
        const std::uint32_t base = cellIndex * 4;
        for (const auto offset : {0u, 1u, 2u, 1u, 3u, 2u}) {
            packet.indices.push_back(base + offset);
        }
    }
    return packet;
}

// Scene objects for the terrain frame are small and tucked into one corner so
// the terrain still owns most of the comparison area while the shared depth
// buffer is still exercised.
bool BuildTerrainSceneObjects(scene::ScenePacket& packet)
{
    packet = {};
    packet.header.frameId = kSceneFrameIndex;
    packet.header.viewId = kSceneViewId;
    packet.header.captureSequence =
        0xC000'0000'0000'1100ull | kSceneFrameIndex;
    packet.header.captureThreadId = 1;
    packet.header.renderThreadId = 1;
    const std::array<float, kSceneObjectCount> roughness{
        0.15f, 0.45f, 0.80f};
    for (std::uint32_t index = 0; index < kSceneObjectCount; ++index) {
        scene::OpaqueObjectV1 object{};
        object.objectId = kSceneObjectIdBase + index;
        object.materialId = kSceneMaterialIdBase + index;
        object.drawIndex = index;
        object.passSequence = index == 2 ? 2 : 1;
        object.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
        object.roughness = roughness[index];
        object.boundsMinimum[0] = -0.80f;
        object.boundsMinimum[1] = -0.70f;
        object.boundsMaximum[0] = 0.80f;
        object.boundsMaximum[1] = 0.90f;
        packet.objects.push_back(object);
    }
    // Twenty times closer than the terrain, so the shared depth buffer has to
    // order the two passes correctly.
    SetSceneModel(packet.objects[0], 8.0f, 0.0f, {-58.0f, -40.0f, 100.0f});
    SetSceneModel(packet.objects[1], 8.0f, 0.0f, {-44.0f, -40.0f, 100.0f});
    SetSceneModel(packet.objects[2], 8.0f, 0.4363323130f,
        {-51.0f, -26.0f, 100.0f});
    return scene::ValidateScenePacket(packet) ==
        scene::ScenePacketError::None;
}

struct InstancedMesh
{
    std::uint64_t contentHash{};
    std::uint64_t materialId{};
    std::uint32_t passSequence{};
    std::array<float, 4> baseColor{};
    std::array<std::array<float, 3>, 3> colors{};
    float roughness{};
};

struct InstancedPlacement
{
    std::uint64_t geometryAddress{};
    std::uint32_t meshIndex{};
    std::uint32_t groupId{};
    float scale{};
    std::array<float, 3> translation{};
    std::array<float, 4> parameters{};
};

// Two meshes, one of which is submitted from two distinct engine addresses
// with identical content so deduplication has to fold them together.
const std::array<InstancedMesh, 2>& InstancedMeshes()
{
    static const std::array<InstancedMesh, 2> meshes{{
        {0xA100'0000'0000'0001ull, kSceneMaterialIdBase, 1,
            {1.00f, 0.95f, 0.90f, 1.0f},
            {{{0.90f, 0.25f, 0.15f}, {0.20f, 0.85f, 0.30f},
              {0.15f, 0.30f, 0.95f}}},
            0.15f},
        {0xB200'0000'0000'0002ull, kSceneMaterialIdBase + 1, 2,
            {0.80f, 0.90f, 1.00f, 1.0f},
            {{{0.35f, 0.75f, 0.85f}, {0.35f, 0.75f, 0.85f},
              {0.35f, 0.75f, 0.85f}}},
            0.55f},
    }};
    return meshes;
}

const std::array<InstancedPlacement, 5>& InstancedPlacements()
{
    static const std::array<InstancedPlacement, 5> placements{{
        {0x7000'0001, 0, 1, 0.45f, {-1.20f, 0.0f, 2.20f},
            {1.0f, 1.0f, 1.0f, 1.0f}},
        {0x7000'0002, 0, 1, 0.45f, {-0.40f, 0.0f, 2.20f},
            {0.6f, 1.0f, 0.6f, 1.0f}},
        {0x7000'0003, 0, 1, 0.45f, {0.40f, 0.0f, 2.20f},
            {1.0f, 0.6f, 0.6f, 1.0f}},
        {0x7000'0004, 1, 2, 0.45f, {1.20f, 0.0f, 2.60f},
            {0.7f, 0.7f, 1.0f, 1.0f}},
        {0x7000'0005, 1, 2, 0.45f, {1.80f, 0.0f, 2.60f},
            {1.0f, 1.0f, 0.5f, 1.0f}},
    }};
    return placements;
}

std::array<float, 16> InstancedModel(
    const float scale,
    const std::array<float, 3>& translation)
{
    std::array<float, 16> model{};
    model[0] = scale;
    model[5] = scale;
    model[10] = scale;
    model[3] = translation[0];
    model[7] = translation[1];
    model[11] = translation[2];
    model[15] = 1.0f;
    return model;
}

bool BuildInstancedSource(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::vector<std::uint32_t>& meshOrder,
    raster::DecodedPacket& source,
    std::vector<std::byte>& bytes)
{
    source = {};
    bytes.clear();
    if (meshOrder.empty()) return false;
    const std::array<std::array<float, 3>, 3> localPositions{{
        {-0.80f, -0.70f, 0.0f},
        {0.0f, 0.90f, 0.0f},
        {0.80f, -0.70f, 0.0f},
    }};
    const std::array<std::array<float, 2>, 3> localTexCoords{{
        {0.0f, 1.0f},
        {0.5f, 0.0f},
        {1.0f, 1.0f},
    }};
    source.header.frameIndex = kSceneFrameIndex;
    source.header.width = width;
    source.header.height = height;
    source.header.viewportWidth = static_cast<float>(width);
    source.header.viewportHeight = static_cast<float>(height);
    source.header.viewportMaxDepth = 1.0f;
    source.header.scissorWidth = width;
    source.header.scissorHeight = height;
    source.header.indexType = raster::IndexType::Uint16;
    for (std::uint32_t slot = 0; slot < meshOrder.size(); ++slot) {
        const auto& mesh = InstancedMeshes()[meshOrder[slot]];
        for (std::uint32_t vertex = 0; vertex < 3; ++vertex) {
            raster::RasterVertexV3 value{};
            std::copy(localPositions[vertex].begin(),
                localPositions[vertex].end(), value.position);
            std::copy(mesh.colors[vertex].begin(), mesh.colors[vertex].end(),
                value.color);
            std::copy(localTexCoords[vertex].begin(),
                localTexCoords[vertex].end(), value.texCoord);
            // Local -Z, matching every other fixture here.
            value.normal[0] = 0.0f;
            value.normal[1] = 0.0f;
            value.normal[2] = -1.0f;
            source.vertices.push_back(value);
            source.indices.push_back(slot * 3 + vertex);
        }
        raster::RasterMaterialV1 material{};
        material.resourceId = mesh.materialId;
        std::copy(mesh.baseColor.begin(), mesh.baseColor.end(),
            material.baseColor);
        source.materials.push_back(material);
        source.draws.push_back({mesh.materialId, slot * 3, 3, 0,
            raster::FrontFace::CounterClockwise,
            raster::DepthCompare::Less, 0});
    }
    source.header.vertexCount =
        static_cast<std::uint32_t>(source.vertices.size());
    source.header.indexCount =
        static_cast<std::uint32_t>(source.indices.size());
    source.header.drawCount =
        static_cast<std::uint32_t>(source.draws.size());
    source.header.materialCount =
        static_cast<std::uint32_t>(source.materials.size());
    if (!raster::EncodePacket(source, bytes)) return false;
    raster::DecodedPacket verified;
    if (!raster::DecodePacket(bytes, verified)) return false;
    source = std::move(verified);
    return true;
}

// Translates the mirrored database into one object per surviving mesh and a
// contiguous instance run per object, exactly as the engine seam will.
bool BuildInstancedScene(
    const scene::SceneDatabase& database,
    const std::vector<scene::InstanceHandle>& handles,
    std::vector<std::uint32_t>& meshOrder,
    scene::ScenePacket& packet)
{
    packet = {};
    meshOrder.clear();
    packet.header.frameId = kSceneFrameIndex;
    packet.header.viewId = kSceneViewId;
    packet.header.captureSequence =
        0xC000'0000'0000'1200ull | kSceneFrameIndex;
    packet.header.captureThreadId = 1;
    packet.header.renderThreadId = 1;

    std::vector<scene::InstanceRecord> records;
    for (const auto handle : handles) {
        const auto record = database.Lookup(handle);
        if (!record.has_value() || record->retiring) continue;
        records.push_back(*record);
    }
    std::sort(records.begin(), records.end(),
        [](const scene::InstanceRecord& left,
           const scene::InstanceRecord& right) {
            return left.contentHash != right.contentHash
                ? left.contentHash < right.contentHash
                : left.objectId < right.objectId;
        });
    if (records.empty()) return false;

    for (const auto& record : records) {
        const auto mesh = std::find_if(
            InstancedMeshes().begin(), InstancedMeshes().end(),
            [&record](const InstancedMesh& candidate) {
                return candidate.contentHash == record.contentHash;
            });
        if (mesh == InstancedMeshes().end()) return false;
        const auto meshIndex = static_cast<std::uint32_t>(
            std::distance(InstancedMeshes().begin(), mesh));
        if (meshOrder.empty() || meshOrder.back() != meshIndex) {
            if (std::find(meshOrder.begin(), meshOrder.end(), meshIndex) !=
                meshOrder.end()) {
                return false;
            }
            meshOrder.push_back(meshIndex);
            scene::OpaqueObjectV1 object{};
            object.objectId = mesh->contentHash;
            object.materialId = mesh->materialId;
            object.drawIndex = static_cast<std::uint32_t>(
                meshOrder.size() - 1);
            object.passSequence = mesh->passSequence;
            object.flags =
                scene::ObjectWritesWorldTarget | scene::ObjectStatic;
            object.roughness = mesh->roughness;
            object.boundsMinimum[0] = -0.80f;
            object.boundsMinimum[1] = -0.70f;
            object.boundsMaximum[0] = 0.80f;
            object.boundsMaximum[1] = 0.90f;
            object.model[0] = 1.0f;
            object.model[5] = 1.0f;
            object.model[10] = 1.0f;
            object.model[15] = 1.0f;
            std::copy(std::begin(object.model), std::end(object.model),
                std::begin(object.previousModel));
            object.geometricNormal[2] = -1.0f;
            object.shadingNormal[2] = -1.0f;
            packet.objects.push_back(object);
        }
        scene::InstanceV1 instance{};
        instance.objectId = record.objectId;
        instance.objectIndex = static_cast<std::uint32_t>(
            packet.objects.size() - 1);
        instance.flags = scene::InstanceStatic;
        std::copy(record.model.begin(), record.model.end(),
            std::begin(instance.model));
        std::copy(record.previousModel.begin(), record.previousModel.end(),
            std::begin(instance.previousModel));
        std::copy(record.parameters.begin(), record.parameters.end(),
            std::begin(instance.parameters));
        packet.instances.push_back(instance);
    }
    return scene::ValidateScenePacket(packet) ==
        scene::ScenePacketError::None;
}

const std::array<std::array<float, 3>, 3>& DeformBaseTriangle()
{
    static const std::array<std::array<float, 3>, 3> positions{{
        {-0.80f, -0.70f, 0.0f},
        {0.0f, 0.50f, 0.0f},
        {0.80f, -0.70f, 0.0f},
    }};
    return positions;
}

// One bone holds the base, a second lifts the apex, a morph slides the third
// vertex, and wind flexes only the apex. Every input has a distinct
// signature so a missing stage cannot hide behind another.
deform::DeformationPacket BuildDeformFixture(
    const std::uint32_t frameIndex,
    const std::uint32_t vertexCount = 3,
    const std::uint32_t generation = 1)
{
    deform::DeformationPacket packet{};
    packet.header.topologyId = 0xD13'0000'0000'0001ull;
    packet.header.generation = generation;
    packet.header.captureSequence = 0xC130'0000ull + frameIndex;
    packet.header.captureThreadId = 1;
    packet.header.renderThreadId = 1;
    const auto step = 0.05f * static_cast<float>(frameIndex);
    for (std::uint32_t index = 0; index < vertexCount; ++index) {
        deform::DeformVertexV1 vertex{};
        if (index == 0) {
            vertex.bones[0] = 0;
            vertex.weights[0] = 1.0f;
        } else if (index == 1) {
            vertex.bones[0] = 1;
            vertex.weights[0] = 1.0f;
            vertex.flexibility = 1.0f;
        } else {
            vertex.bones[0] = 0;
            vertex.bones[1] = 1;
            vertex.weights[0] = 0.5f;
            vertex.weights[1] = 0.5f;
        }
        packet.vertices.push_back(vertex);
    }
    deform::BoneTransformV1 identity{};
    identity.rows[0] = 1.0f;
    identity.rows[5] = 1.0f;
    identity.rows[10] = 1.0f;
    auto lift = identity;
    lift.rows[7] = 0.15f + step;
    auto previousLift = identity;
    previousLift.rows[7] = 0.05f + step;
    packet.bones.push_back(identity);
    packet.bones.push_back(lift);
    packet.previousBones.push_back(identity);
    packet.previousBones.push_back(previousLift);

    if (vertexCount >= 3) {
        deform::MorphTargetV1 target{};
        target.firstDelta = 0;
        target.deltaCount = 1;
        target.weight = 0.5f;
        target.previousWeight = 0.25f;
        packet.morphTargets.push_back(target);
        deform::MorphDeltaV1 delta{};
        delta.vertexIndex = 2;
        delta.delta[0] = 0.30f;
        packet.morphDeltas.push_back(delta);
    }
    packet.header.wind.direction[0] = 1.0f;
    packet.header.wind.amplitude = 0.05f;
    packet.header.wind.frequency = 2.0f;
    packet.header.wind.time = 0.50f + step;
    packet.header.wind.previousTime = 0.25f + step;
    return packet;
}

bool BuildDeformSource(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::span<const std::array<float, 3>> positions,
    raster::DecodedPacket& source,
    std::vector<std::byte>& bytes)
{
    source = {};
    bytes.clear();
    if (positions.size() < 3) return false;
    const std::array<std::array<float, 3>, 3> colors{{
        {0.90f, 0.30f, 0.20f},
        {0.25f, 0.85f, 0.35f},
        {0.20f, 0.35f, 0.95f},
    }};
    const std::array<std::array<float, 2>, 3> texCoords{{
        {0.0f, 1.0f},
        {0.5f, 0.0f},
        {1.0f, 1.0f},
    }};
    source.header.frameIndex = kSceneFrameIndex;
    source.header.width = width;
    source.header.height = height;
    source.header.viewportWidth = static_cast<float>(width);
    source.header.viewportHeight = static_cast<float>(height);
    source.header.viewportMaxDepth = 1.0f;
    source.header.scissorWidth = width;
    source.header.scissorHeight = height;
    source.header.indexType = raster::IndexType::Uint16;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        raster::RasterVertexV3 value{};
        std::copy(positions[index].begin(), positions[index].end(),
            value.position);
        std::copy(colors[index % colors.size()].begin(),
            colors[index % colors.size()].end(), value.color);
        std::copy(texCoords[index % texCoords.size()].begin(),
            texCoords[index % texCoords.size()].end(), value.texCoord);
        // Local -Z, the same convention every fixture here uses, so the
        // rotated vertex normal reproduces the object record exactly.
        value.normal[0] = 0.0f;
        value.normal[1] = 0.0f;
        value.normal[2] = -1.0f;
        source.vertices.push_back(value);
    }
    source.indices = {0, 1, 2};
    raster::RasterMaterialV1 material{};
    material.resourceId = kSceneMaterialIdBase;
    material.baseColor[0] = 1.0f;
    material.baseColor[1] = 0.95f;
    material.baseColor[2] = 0.90f;
    material.baseColor[3] = 1.0f;
    source.materials.push_back(material);
    source.draws.push_back({material.resourceId, 0, 3, 0,
        raster::FrontFace::CounterClockwise,
        raster::DepthCompare::Less, 0});
    source.header.vertexCount =
        static_cast<std::uint32_t>(source.vertices.size());
    source.header.indexCount =
        static_cast<std::uint32_t>(source.indices.size());
    source.header.drawCount = 1;
    source.header.materialCount = 1;
    if (!raster::EncodePacket(source, bytes)) return false;
    raster::DecodedPacket verified;
    if (!raster::DecodePacket(bytes, verified)) return false;
    source = std::move(verified);
    return true;
}

bool BuildDeformScene(scene::ScenePacket& packet)
{
    packet = {};
    packet.header.frameId = kSceneFrameIndex;
    packet.header.viewId = kSceneViewId;
    packet.header.captureSequence =
        0xC000'0000'0000'1300ull | kSceneFrameIndex;
    packet.header.captureThreadId = 1;
    packet.header.renderThreadId = 1;
    scene::OpaqueObjectV1 object{};
    object.objectId = 0x5130'0000'0000'0001ull;
    object.materialId = kSceneMaterialIdBase;
    object.drawIndex = 0;
    object.passSequence = 1;
    object.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
    object.roughness = 0.35f;
    object.boundsMinimum[0] = -1.20f;
    object.boundsMinimum[1] = -1.20f;
    object.boundsMaximum[0] = 1.20f;
    object.boundsMaximum[1] = 1.60f;
    object.model[0] = 1.0f;
    object.model[5] = 1.0f;
    object.model[10] = 1.0f;
    object.model[11] = 2.20f;
    object.model[15] = 1.0f;
    std::copy(std::begin(object.model), std::end(object.model),
        std::begin(object.previousModel));
    object.geometricNormal[2] = -1.0f;
    object.shadingNormal[2] = -1.0f;
    packet.objects.push_back(object);
    return scene::ValidateScenePacket(packet) ==
        scene::ScenePacketError::None;
}

struct DeformOutputRecord
{
    float current[4]{};
    float previous[4]{};
};

bool WriteGBuffer(
    const std::filesystem::path& path,
    const std::span<const scene::GBufferPixelV1> pixels)
{
    std::vector<std::byte> bytes;
    try {
        bytes.resize(pixels.size() * sizeof(scene::GBufferPixelV1));
    } catch (...) {
        return false;
    }
    if (!pixels.empty()) {
        std::memcpy(bytes.data(), pixels.data(), bytes.size());
    }
    return WriteBinaryFile(path, bytes);
}

bool ParseMaterialRenderOptions(
    const int argc,
    const char* const* argv,
    const int firstOption,
    MaterialRenderOptions& options)
{
    for (int index = firstOption; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else if (argument == "--bundle-output" && index + 1 < argc) {
            options.bundleOutput = argv[++index];
        } else if (argument == "--width" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.width)) return false;
        } else if (argument == "--height" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.height)) return false;
        } else {
            return false;
        }
    }
    return !options.output.empty() && options.width >= 64 &&
        options.height >= 64 && options.width <= 4096 &&
        options.height <= 4096;
}

std::array<float, 3> Cross3(
    const std::array<float, 3>& first,
    const std::array<float, 3>& second)
{
    return {
        first[1] * second[2] - first[2] * second[1],
        first[2] * second[0] - first[0] * second[2],
        first[0] * second[1] - first[1] * second[0],
    };
}

std::uint8_t ToneByte(const float linear)
{
    const auto mapped = std::max(0.0f, linear) /
        (1.0f + std::max(0.0f, linear));
    const auto srgb = mapped <= 0.0031308f
        ? mapped * 12.92f
        : 1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(
        std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
}

int RenderMaterial(const MaterialRenderOptions& options)
{
    constexpr std::uintmax_t kMaximumMaterialBytes =
        192u * 1024u * 1024u;
    std::vector<std::byte> bytes;
    material::MaterialReplayBundle bundle;
    if (options.fixture) {
        const auto source = BuildMaterialFixture();
        const auto encoded = material::EncodeMaterialReplayBundle(
            source, bytes);
        if (encoded != material::MaterialPacketError::None) {
            std::cerr << "material-replay: fixture encode failed="
                      << material::ToString(encoded) << '\n';
            return 17;
        }
    } else if (!ReadBinaryFile(
            options.input, kMaximumMaterialBytes, bytes)) {
        std::cerr << "material-replay: input read failed or exceeds limit\n";
        return 17;
    }
    const auto decoded = material::DecodeMaterialReplayBundle(bytes, bundle);
    if (decoded != material::MaterialPacketError::None) {
        std::cerr << "material-replay: bundle decode failed="
                  << material::ToString(decoded) << '\n';
        return 18;
    }
    if (!options.bundleOutput.empty() &&
        !WriteBinaryFile(options.bundleOutput, bytes)) {
        std::cerr << "material-replay: bundle output failed\n";
        return 19;
    }
    const auto transfer = material::MakeDefaultMaterialTransferLut();
    if (bundle.transferVersion != transfer.version) {
        std::cerr << "material-replay: unsupported transfer version\n";
        return 20;
    }

    raster::RasterImage image;
    image.width = options.width;
    image.height = options.height;
    try {
        image.pixels.assign(
            static_cast<std::size_t>(options.width) * options.height,
            raster::Rgba8{12, 17, 25, 255});
    } catch (...) {
        std::cerr << "material-replay: image allocation failed\n";
        return 20;
    }
    const auto centerX = (static_cast<float>(options.width) - 1.0f) * 0.5f;
    const auto centerY = (static_cast<float>(options.height) - 1.0f) * 0.5f;
    const auto radius = 0.43f * static_cast<float>(
        std::min(options.width, options.height));
    const auto light = Normalize3({-0.45f, 0.55f, 1.0f});
    const std::array<float, 3> view{0.0f, 0.0f, 1.0f};
    const std::array<float, 3> radiance{4.0f, 4.0f, 4.0f};
    float minimumSmoothness = 1.0f;
    float maximumSmoothness = 0.0f;
    float minimumSpecular = 1.0f;
    float maximumSpecular = 0.0f;
    std::uint64_t shadedPixels{};
    for (std::uint32_t y = 0; y < options.height; ++y) {
        for (std::uint32_t x = 0; x < options.width; ++x) {
            const auto sphereX =
                (static_cast<float>(x) - centerX) / radius;
            const auto sphereY =
                (centerY - static_cast<float>(y)) / radius;
            const auto radialSquared =
                sphereX * sphereX + sphereY * sphereY;
            if (radialSquared > 1.0f) continue;
            const auto sphereZ = std::sqrt(
                std::max(0.0f, 1.0f - radialSquared));
            const std::array<float, 3> geometricNormal{
                sphereX, sphereY, sphereZ};
            const auto uv = material::TransformMaterialUv(
                bundle.material,
                {0.5f * (sphereX + 1.0f),
                 0.5f * (1.0f - sphereY)});
            material::MaterialSurfaceInput input{};
            if (texture::SampleTexture2D(bundle.textures[0],
                    uv[0], uv[1], 0.0f, input.baseColor) !=
                    texture::TexturePacketError::None ||
                texture::SampleTexture2D(bundle.textures[1],
                    uv[0], uv[1], 0.0f, input.normal) !=
                    texture::TexturePacketError::None ||
                texture::SampleTexture2D(bundle.textures[2],
                    uv[0], uv[1], 0.0f, input.smoothSpec) !=
                    texture::TexturePacketError::None) {
                std::cerr << "material-replay: texture sampling failed\n";
                return 21;
            }
            input.geometricNormal = geometricNormal;
            input.tangent = Normalize3({sphereZ, 0.0f, -sphereX});
            input.bitangent = Normalize3(
                Cross3(geometricNormal, input.tangent));
            material::MaterialSurface surface{};
            const auto evaluated = material::EvaluateMaterialSurface(
                bundle.material, input, transfer, surface);
            if (evaluated != material::MaterialError::None) {
                std::cerr << "material-replay: evaluation failed="
                          << material::ToString(evaluated) << '\n';
                return 21;
            }
            const auto lighting = material::EvaluateGgxDirect(
                surface, view, light, radiance);
            std::array<float, 3> output{};
            for (std::size_t channel = 0; channel < 3; ++channel) {
                output[channel] = lighting.combined[channel] +
                    surface.baseColor[channel] * 0.035f;
            }
            image.pixels[static_cast<std::size_t>(y) * options.width + x] = {
                ToneByte(output[0]), ToneByte(output[1]),
                ToneByte(output[2]), 255};
            minimumSmoothness = std::min(
                minimumSmoothness, surface.smoothness);
            maximumSmoothness = std::max(
                maximumSmoothness, surface.smoothness);
            const auto specular = material::DecodeSmoothSpec(
                input.smoothSpec).specularWeight;
            minimumSpecular = std::min(minimumSpecular, specular);
            maximumSpecular = std::max(maximumSpecular, specular);
            ++shadedPixels;
        }
    }
    const auto semanticRangePass = shadedPixels != 0 &&
        maximumSmoothness - minimumSmoothness > 0.7f &&
        maximumSpecular - minimumSpecular > 0.7f;
    const auto wrote = WritePpm(options.output, image);
    const auto passed = semanticRangePass && wrote;
    std::cout << "material-replay material=" << bundle.material.materialId
              << " revision=" << bundle.material.revision
              << " static-revision=" << bundle.material.staticRevision
              << " transfer=" << bundle.transferVersion
              << " shaded-pixels=" << shadedPixels
              << " smoothness-range=" << minimumSmoothness << ".."
              << maximumSmoothness
              << " specular-range=" << minimumSpecular << ".."
              << maximumSpecular
              << " provenance=authored-semantic-slots"
              << " output=" << options.output.string()
              << " bundle="
              << (options.bundleOutput.empty()
                    ? options.input.string()
                    : options.bundleOutput.string())
              << " result=" << (passed ? "pass" : "fail") << '\n';
    return passed ? 0 : 22;
}

std::uint32_t PixelMaximumError(
    const raster::Rgba8 expected,
    const raster::Rgba8 actual)
{
    const std::uint8_t expectedChannels[]{
        expected.r, expected.g, expected.b, expected.a};
    const std::uint8_t actualChannels[]{
        actual.r, actual.g, actual.b, actual.a};
    std::uint32_t maximum{};
    for (std::size_t index = 0; index < 4; ++index) {
        maximum = std::max(maximum,
            static_cast<std::uint32_t>(std::abs(
                static_cast<int>(expectedChannels[index]) -
                static_cast<int>(actualChannels[index]))));
    }
    return maximum;
}

struct RenderedFixture
{
    raster::RasterImage image;
    abi::RasterStatusV1 status{};
    raster::RasterComparison comparison{};
};

bool RenderFixture(
    BackendHost& host,
    const raster::SyntheticPacketOptions& options,
    const char* name,
    const std::vector<std::byte>* textureBytes,
    const texture::CapturedTexture* sampledTexture,
    const std::vector<std::byte>* materialBytes,
    const material::MaterialReplayBundle* sampledMaterial,
    const std::vector<std::byte>* frameBytes,
    const view::ViewRecordV1* transformedView,
    RenderedFixture& rendered)
{
    if ((frameBytes == nullptr) != (transformedView == nullptr)) {
        std::cerr << "raster-replay: fixture " << name
                  << " has incomplete frame/view input\n";
        return false;
    }
    auto bytes = raster::BuildSyntheticPacket(options);
    raster::DecodedPacket decoded;
    const auto decodedResult = raster::DecodePacket(bytes, decoded);
    if (!decodedResult) {
        std::cerr << "raster-replay: fixture " << name
                  << " decode failed=" << raster::ToString(decodedResult.error)
                  << '\n';
        return false;
    }
    raster::DecodedPacket projected;
    bool sourceDiffersFromNdc{};
    const raster::DecodedPacket* referencePacket = &decoded;
    if (transformedView != nullptr) {
        const auto desiredNdc = decoded;
        if (!BuildViewSpacePacket(desiredNdc, *transformedView,
                bytes, decoded, sourceDiffersFromNdc) ||
            raster::ProjectPacketForView(
                decoded, *transformedView, projected) !=
                raster::ReferenceRasterError::None) {
            std::cerr << "raster-replay: fixture " << name
                      << " view-space preparation failed\n";
            return false;
        }
        referencePacket = &projected;
    }
    raster::RasterImage expected;
    const auto referenceResult = sampledMaterial != nullptr
        ? raster::RenderReferenceMaterial(
            *referencePacket, *sampledMaterial, expected)
        : sampledTexture != nullptr
            ? raster::RenderReferenceTextured(
                *referencePacket, *sampledTexture, expected)
            : raster::RenderReference(*referencePacket, expected);
    if (referenceResult != raster::ReferenceRasterError::None) {
        std::cerr << "raster-replay: fixture " << name
                  << " reference render failed\n";
        return false;
    }
    rendered.image.width = options.width;
    rendered.image.height = options.height;
    rendered.image.pixels.resize(
        static_cast<std::size_t>(options.width) * options.height);
    abi::RasterFrameRequestV1 request{};
    request.structSize = sizeof(request);
    request.packetData = reinterpret_cast<std::uintptr_t>(bytes.data());
    request.packetSize = bytes.size();
    request.outputData = reinterpret_cast<std::uintptr_t>(
        rendered.image.pixels.data());
    request.outputRowPitch = options.width * sizeof(raster::Rgba8);
    request.outputCapacity = request.outputRowPitch * options.height;
    if (textureBytes != nullptr) {
        request.textureData = reinterpret_cast<std::uintptr_t>(
            textureBytes->data());
        request.textureSize = textureBytes->size();
    }
    if (materialBytes != nullptr) {
        request.materialData = reinterpret_cast<std::uintptr_t>(
            materialBytes->data());
        request.materialSize = materialBytes->size();
    }
    if (frameBytes != nullptr) {
        request.frameData = reinterpret_cast<std::uintptr_t>(
            frameBytes->data());
        request.frameSize = frameBytes->size();
    }
    rendered.status = {};
    rendered.status.structSize = sizeof(rendered.status);
    const auto result = host.RenderRasterFrame(request, rendered.status);
    if (!result) {
        std::cerr << "raster-replay: fixture " << name
                  << " render failed host="
                  << static_cast<unsigned>(result.error)
                  << " backend=" << static_cast<unsigned>(result.backendResult)
                  << " diagnostic=" << rendered.status.diagnostic << '\n';
        return false;
    }
    rendered.comparison = raster::CompareRaster(
        expected.pixels, rendered.image.pixels);
    const auto sampled = sampledTexture != nullptr ||
        sampledMaterial != nullptr;
    const auto maximumDifferences = static_cast<std::uint64_t>(
        expected.pixels.size() /
            (sampled ? 2u : 10u));
    const auto probesPass =
        PixelMaximumError(expected.At(0, 0), rendered.image.At(0, 0)) == 0 &&
        PixelMaximumError(expected.At(options.width - 1, 0),
            rendered.image.At(options.width - 1, 0)) == 0 &&
        PixelMaximumError(expected.At(0, options.height - 1),
            rendered.image.At(0, options.height - 1)) == 0 &&
        PixelMaximumError(expected.At(options.width / 2, options.height / 2),
            rendered.image.At(options.width / 2, options.height / 2)) <= 8;
    const auto tolerancePass = !sampled
        ? rendered.comparison.Within(255, 2.0, maximumDifferences)
        : sampledMaterial != nullptr
            ? rendered.comparison.Within(8, 0.5, maximumDifferences)
            : rendered.comparison.Within(4, 0.2, maximumDifferences);
    std::cout << "raster-fixture name=" << name
              << " extent=" << options.width << 'x' << options.height
              << " index="
              << (options.indexType == raster::IndexType::Uint16 ? 16 : 32)
              << " submission=" << rendered.status.submissionCount
              << " extent-generation=" << rendered.status.extentGeneration
              << " differing=" << rendered.comparison.differingPixels
              << " max-error=" << rendered.comparison.maximumChannelError
              << " mean-error=" << rendered.comparison.meanAbsoluteError
              << " probes=" << (probesPass ? "pass" : "fail")
              << " tolerance=" << (tolerancePass ? "pass" : "fail")
              << " view="
              << (transformedView != nullptr ? "captured" : "identity")
              << " source="
              << (sourceDiffersFromNdc ? "camera-relative" : "ndc")
              << '\n';
    return probesPass && tolerancePass;
}

int RenderSynthetic(const RenderOptions& options)
{
    std::vector<std::byte> textureBytes;
    texture::CapturedTexture sampledTexture;
    bool textureEnabled{};
    if (!LoadTexture(options, textureBytes,
            sampledTexture, textureEnabled)) {
        std::cerr << "raster-replay: texture input/fixture failed\n";
        return 5;
    }
    std::vector<std::byte> materialBytes;
    material::MaterialReplayBundle sampledMaterial;
    bool materialEnabled{};
    if (!LoadMaterial(options, materialBytes,
            sampledMaterial, materialEnabled) ||
        (textureEnabled && materialEnabled)) {
        std::cerr << "raster-replay: material input/fixture failed or conflicts "
                     "with texture input\n";
        return 5;
    }
    std::vector<std::byte> frameBytes;
    view::FramePacket capturedFrame;
    bool frameEnabled{};
    if (!LoadFrame(options, options.width, options.height, 1,
            frameBytes, capturedFrame, frameEnabled)) {
        std::cerr << "raster-replay: frame input/fixture/output failed\n";
        return 5;
    }
    abi::AdapterLuid luid{};
    if (!QueryDefaultAdapterLuid(luid)) {
        std::cerr << "raster-replay: D3D adapter query failed\n";
        return 5;
    }
    WindowsBackendModule module{options.backend};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    const auto loaded = host.Load(module, callbacks);
    if (!loaded || !host.RasterAvailable()) {
        std::cerr << "raster-replay: backend load/API failed host="
                  << static_cast<unsigned>(loaded.error)
                  << " backend=" << static_cast<unsigned>(loaded.backendResult)
                  << " win32=" << module.LastErrorCode() << '\n';
        return 6;
    }
    abi::RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.flags = abi::RasterCreateAnyAdapter |
        (options.validation ? abi::RasterCreateValidation : 0u);
    create.adapterLuid = luid;
    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    const auto created = host.CreateRaster(create, status);
    if (!created) {
        std::cerr << "raster-replay: create failed host="
                  << static_cast<unsigned>(created.error)
                  << " backend=" << static_cast<unsigned>(created.backendResult)
                  << " diagnostic=" << status.diagnostic << '\n';
        return 7;
    }

    bool passed = true;
    if (options.fixtures) {
        auto malformed = raster::BuildSyntheticPacket();
        auto badMagic = std::uint32_t{};
        std::memcpy(malformed.data(), &badMagic, sizeof(badMagic));
        std::vector<raster::Rgba8> output(96 * 64);
        abi::RasterFrameRequestV1 frame{};
        frame.structSize = sizeof(frame);
        frame.packetData = reinterpret_cast<std::uintptr_t>(malformed.data());
        frame.packetSize = malformed.size();
        frame.outputData = reinterpret_cast<std::uintptr_t>(output.data());
        frame.outputRowPitch = 96 * sizeof(raster::Rgba8);
        frame.outputCapacity = output.size() * sizeof(raster::Rgba8);
        status = {};
        status.structSize = sizeof(status);
        const auto rejected = host.RenderRasterFrame(frame, status);
        const auto rejectedBeforeSubmit =
            rejected.error == BackendHostError::RasterRenderFailed &&
            rejected.backendResult == abi::Result::RasterInvalidPacket &&
            status.submissionCount == 0 &&
            status.packetError ==
                static_cast<std::uint32_t>(raster::PacketError::BadMagic);
        std::cout << "raster-malformed result="
                  << (rejectedBeforeSubmit ? "rejected" : "unexpected")
                  << " submission=" << status.submissionCount
                  << " diagnostic=" << status.diagnostic << '\n';
        passed = passed && rejectedBeforeSubmit;
    }

    std::vector<std::pair<std::string, raster::SyntheticPacketOptions>> fixtures;
    raster::SyntheticPacketOptions base;
    base.width = options.width;
    base.height = options.height;
    base.indexType = options.indexType;
    fixtures.emplace_back("base", base);
    if (options.fixtures) {
        auto wideIndex = base;
        wideIndex.frameIndex = 2;
        wideIndex.indexType = raster::IndexType::Uint32;
        fixtures.emplace_back("uint32", wideIndex);
        auto depth = base;
        depth.frameIndex = 3;
        depth.includeOccludedTriangle = true;
        fixtures.emplace_back("depth-occlusion", depth);
        auto clockwise = base;
        clockwise.frameIndex = 4;
        clockwise.reverseWinding = true;
        clockwise.frontFace = raster::FrontFace::Clockwise;
        fixtures.emplace_back("clockwise", clockwise);
        auto resized = base;
        resized.frameIndex = 5;
        resized.width = std::min(
            base.width + 16, raster::kMaximumExtent);
        resized.height = std::min(
            base.height + 8, raster::kMaximumExtent);
        fixtures.emplace_back("resize", resized);
        auto resizedStable = resized;
        resizedStable.frameIndex = 6;
        resizedStable.indexType = raster::IndexType::Uint32;
        fixtures.emplace_back("resize-stable", resizedStable);
        auto restored = base;
        restored.frameIndex = 7;
        fixtures.emplace_back("resize-restore", restored);
    }

    RenderedFixture last;
    for (const auto& [name, fixture] : fixtures) {
        RenderedFixture current;
        const auto fixturePassed = RenderFixture(
            host, fixture, name.c_str(),
            textureEnabled ? &textureBytes : nullptr,
            textureEnabled ? &sampledTexture : nullptr,
            materialEnabled ? &materialBytes : nullptr,
            materialEnabled ? &sampledMaterial : nullptr,
            frameEnabled ? &frameBytes : nullptr,
            frameEnabled ? &capturedFrame.views.front() : nullptr,
            current);
        passed = passed && fixturePassed;
        last = std::move(current);
    }
    if (options.fixtures) {
        const auto lifecyclePass =
            last.status.submissionCount == fixtures.size() &&
            last.status.extentGeneration == 3;
        std::cout << "raster-lifecycle submissions="
                  << last.status.submissionCount
                  << " extent-generation=" << last.status.extentGeneration
                  << " expected-generation=3 result="
                  << (lifecyclePass ? "pass" : "fail") << '\n';
        passed = passed && lifecyclePass;
    }
    const auto wrote = !last.image.pixels.empty() &&
        WritePpm(options.output, last.image);
    if (!wrote) {
        std::cerr << "raster-replay: output write failed\n";
        passed = false;
    }

    status = {};
    status.structSize = sizeof(status);
    const auto destroyed = host.DestroyRaster(status);
    const auto validationPass =
        status.validationErrorCount == 0;
    const auto shutdown = host.RequestShutdown();
    const auto lifecycleClosed = destroyed && validationPass &&
        shutdown.error == BackendHostError::ShutdownDeferred;
    passed = passed && lifecycleClosed;
    std::cout << "raster-replay output=" << options.output.string()
              << " fixtures=" << fixtures.size()
              << " texture=" << (textureEnabled ? "enabled" : "fallback")
              << " material=" << (materialEnabled ? "enabled" : "legacy")
              << " frame="
              << (options.viewFixture
                    ? "fixture"
                    : frameEnabled
                        ? options.frameInput.string()
                        : "identity")
              << " validation-errors=" << status.validationErrorCount
              << " unload=deferred"
              << " result=" << (passed ? "pass" : "fail") << '\n';
    return passed ? 0 : 8;
}

int RenderCapturedMesh(const RenderOptions& options)
{
    if (options.viewFixture || !options.frameInput.empty() ||
        !options.frameOutput.empty()) {
        std::cerr << "mesh-replay: captured-frame transforms are not yet "
                     "supported for normalized mesh translation\n";
        return 9;
    }
    constexpr std::uintmax_t kMaximumMeshBytes = 64u * 1024u * 1024u;
    std::vector<std::byte> meshBytes;
    if (!ReadBinaryFile(options.meshInput, kMaximumMeshBytes, meshBytes)) {
        std::cerr << "mesh-replay: input read failed or exceeds limit\n";
        return 9;
    }

    mesh::CapturedMesh captured;
    const auto meshResult = mesh::DecodeCapturedMesh(meshBytes, captured);
    if (meshResult != mesh::MeshPacketError::None) {
        std::cerr << "mesh-replay: mesh decode failed="
                  << mesh::ToString(meshResult) << '\n';
        return 10;
    }

    std::vector<std::byte> packetBytes;
    mesh::MeshTranslationReport translation;
    const auto translated = mesh::TranslateCapturedMesh(
        captured, options.width, options.height, packetBytes, translation);
    if (translated != mesh::MeshPacketError::None) {
        std::cerr << "mesh-replay: translation failed="
                  << mesh::ToString(translated) << '\n';
        return 11;
    }

    raster::DecodedPacket decoded;
    const auto packetResult = raster::DecodePacket(packetBytes, decoded);
    std::vector<std::byte> textureBytes;
    texture::CapturedTexture sampledTexture;
    bool textureEnabled{};
    if (!LoadTexture(options, textureBytes,
            sampledTexture, textureEnabled)) {
        std::cerr << "mesh-replay: texture input/fixture failed\n";
        return 12;
    }
    std::vector<std::byte> materialBytes;
    material::MaterialReplayBundle sampledMaterial;
    bool materialEnabled{};
    if (!LoadMaterial(options, materialBytes,
            sampledMaterial, materialEnabled) ||
        (textureEnabled && materialEnabled)) {
        std::cerr << "mesh-replay: material input/fixture failed or conflicts "
                     "with texture input\n";
        return 12;
    }
    raster::RasterImage expected;
    const auto referenceResult = !packetResult
        ? raster::ReferenceRasterError::InvalidPacket
        : materialEnabled
            ? raster::RenderReferenceMaterial(
                decoded, sampledMaterial, expected)
        : textureEnabled
            ? raster::RenderReferenceTextured(
                decoded, sampledTexture, expected)
            : raster::RenderReference(decoded, expected);
    if (!packetResult || referenceResult !=
            raster::ReferenceRasterError::None) {
        std::cerr << "mesh-replay: translated packet/reference failed\n";
        return 12;
    }

    abi::AdapterLuid luid{};
    if (!QueryDefaultAdapterLuid(luid)) {
        std::cerr << "mesh-replay: D3D adapter query failed\n";
        return 13;
    }
    WindowsBackendModule module{options.backend};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    const auto loaded = host.Load(module, callbacks);
    if (!loaded || !host.RasterAvailable()) {
        std::cerr << "mesh-replay: backend load/API failed host="
                  << static_cast<unsigned>(loaded.error)
                  << " backend=" << static_cast<unsigned>(loaded.backendResult)
                  << " win32=" << module.LastErrorCode() << '\n';
        return 14;
    }

    abi::RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.flags = abi::RasterCreateAnyAdapter |
        (options.validation ? abi::RasterCreateValidation : 0u);
    create.adapterLuid = luid;
    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    const auto created = host.CreateRaster(create, status);
    if (!created) {
        std::cerr << "mesh-replay: create failed host="
                  << static_cast<unsigned>(created.error)
                  << " backend=" << static_cast<unsigned>(created.backendResult)
                  << " diagnostic=" << status.diagnostic << '\n';
        return 15;
    }

    raster::RasterImage actual;
    actual.width = options.width;
    actual.height = options.height;
    actual.pixels.resize(
        static_cast<std::size_t>(options.width) * options.height);
    abi::RasterFrameRequestV1 frame{};
    frame.structSize = sizeof(frame);
    frame.packetData = reinterpret_cast<std::uintptr_t>(packetBytes.data());
    frame.packetSize = packetBytes.size();
    frame.outputData = reinterpret_cast<std::uintptr_t>(actual.pixels.data());
    frame.outputRowPitch = options.width * sizeof(raster::Rgba8);
    frame.outputCapacity = frame.outputRowPitch * options.height;
    if (textureEnabled) {
        frame.textureData = reinterpret_cast<std::uintptr_t>(
            textureBytes.data());
        frame.textureSize = textureBytes.size();
    }
    if (materialEnabled) {
        frame.materialData = reinterpret_cast<std::uintptr_t>(
            materialBytes.data());
        frame.materialSize = materialBytes.size();
    }
    status = {};
    status.structSize = sizeof(status);
    const auto rendered = host.RenderRasterFrame(frame, status);

    const auto comparison = raster::CompareRaster(
        expected.pixels, actual.pixels);
    const auto maximumDifferences = static_cast<std::uint64_t>(
        expected.pixels.size() /
            (textureEnabled || materialEnabled ? 2u : 10u));
    const auto comparisonPass = rendered &&
        (textureEnabled || materialEnabled
            ? comparison.Within(8, 0.5, maximumDifferences)
            : comparison.Within(255, 2.0, maximumDifferences));
    const auto wrote = rendered && WritePpm(options.output, actual);

    const auto submissionCount = status.submissionCount;
    status = {};
    status.structSize = sizeof(status);
    const auto destroyed = host.DestroyRaster(status);
    const auto validationErrors = status.validationErrorCount;
    const auto shutdown = host.RequestShutdown();
    const auto lifecyclePass = destroyed && validationErrors == 0 &&
        shutdown.error == BackendHostError::ShutdownDeferred;
    const auto passed = comparisonPass && wrote && lifecyclePass;

    std::cout << "mesh-replay input=" << options.meshInput.string()
              << " resource=" << captured.resourceId
              << " generation=" << captured.generation
              << " descriptor=0x" << std::hex << captured.vertexDesc
              << std::dec
              << " source-vertices=" << captured.vertexCount
              << " source-indices=" << captured.indexCount
              << " attributes=" << translation.sourceAttributeCount
              << " bounds-min=" << translation.sourceBounds.minimum[0] << ','
              << translation.sourceBounds.minimum[1] << ','
              << translation.sourceBounds.minimum[2]
              << " bounds-max=" << translation.sourceBounds.maximum[0] << ','
              << translation.sourceBounds.maximum[1] << ','
              << translation.sourceBounds.maximum[2]
              << " translated-vertices=" << translation.translatedVertexCount
              << " translated-indices=" << translation.translatedIndexCount
              << " winding="
              << (translation.clockwise ? "clockwise" : "counter-clockwise")
              << " submission=" << submissionCount
              << " differing=" << comparison.differingPixels
              << " max-error=" << comparison.maximumChannelError
              << " mean-error=" << comparison.meanAbsoluteError
             
              << " validation-errors=" << validationErrors
              << " texture="
              << (options.textureFixture
                    ? "fixture"
                    : textureEnabled
                        ? options.textureInput.string()
                        : "fallback")
              << " material="
              << (options.materialFixture
                    ? "fixture"
                    : materialEnabled
                        ? options.materialInput.string()
                        : "legacy")
              << " output=" << options.output.string()
              << " result=" << (passed ? "pass" : "fail") << '\n';
    return passed ? 0 : 16;
}

struct SceneSubmission
{
    std::vector<scene::GBufferPixelV1> gbuffer;
    raster::RasterImage image;
    abi::RasterStatusV1 status{};
    bool submitted{};
};

bool SubmitScene(
    BackendHost& host,
    const std::vector<std::byte>& packetBytes,
    const std::vector<std::byte>& frameBytes,
    const std::vector<std::byte>* sceneBytes,
    const std::uint32_t width,
    const std::uint32_t height,
    const bool shrinkGBuffer,
    SceneSubmission& submission)
{
    submission = {};
    const auto pixelCount = static_cast<std::size_t>(width) * height;
    try {
        submission.gbuffer.resize(pixelCount);
        submission.image.width = width;
        submission.image.height = height;
        submission.image.pixels.resize(pixelCount);
    } catch (...) {
        return false;
    }
    abi::RasterFrameRequestV1 request{};
    request.structSize = sizeof(request);
    request.packetData = reinterpret_cast<std::uintptr_t>(packetBytes.data());
    request.packetSize = packetBytes.size();
    request.outputData = reinterpret_cast<std::uintptr_t>(
        submission.image.pixels.data());
    request.outputRowPitch = width * sizeof(raster::Rgba8);
    request.outputCapacity =
        static_cast<std::uint64_t>(request.outputRowPitch) * height;
    if (!frameBytes.empty()) {
        request.frameData = reinterpret_cast<std::uintptr_t>(
            frameBytes.data());
        request.frameSize = frameBytes.size();
    }
    if (sceneBytes != nullptr) {
        request.sceneData = reinterpret_cast<std::uintptr_t>(
            sceneBytes->data());
        request.sceneSize = sceneBytes->size();
        request.gbufferData = reinterpret_cast<std::uintptr_t>(
            submission.gbuffer.data());
        request.gbufferCapacity =
            submission.gbuffer.size() * sizeof(scene::GBufferPixelV1);
        if (shrinkGBuffer) {
            request.gbufferCapacity -= sizeof(scene::GBufferPixelV1);
        }
    }
    submission.status.structSize = sizeof(submission.status);
    const auto result = host.RenderRasterFrame(request, submission.status);
    submission.submitted = static_cast<bool>(result);
    return submission.submitted;
}

int RenderScene(const SceneRenderOptions& options)
{
    view::FramePacket frame;
    std::vector<std::byte> frameBytes;
    raster::DecodedPacket source;
    std::vector<std::byte> packetBytes;
    scene::ScenePacket sceneSource;
    std::vector<std::byte> sceneBytes;
    if (!BuildSceneFrame(options.width, options.height, false,
            frame, frameBytes) ||
        !BuildSceneSource(options.width, options.height,
            source, packetBytes) ||
        !BuildSceneObjects(sceneSource) ||
        scene::EncodeScenePacket(sceneSource, sceneBytes) !=
            scene::ScenePacketError::None) {
        std::cerr << "scene-replay: fixture construction failed\n";
        return 5;
    }
    scene::SceneCoverage coverage{};
    const auto accounting = scene::ValidateSceneAgainstFrame(
        sceneSource, frame, coverage);
    if (accounting != scene::ScenePacketError::None ||
        !coverage.MirrorEligible()) {
        std::cerr << "scene-replay: pass accounting rejected the fixture: "
                  << scene::ToString(accounting) << '\n';
        return 5;
    }

    raster::DecodedPacket projected;
    scene::GBufferImage expectedGBuffer;
    raster::RasterImage expectedColor;
    const auto projection = scene::ProjectScenePacket(
        source, frame.views.front(), sceneSource, projected);
    if (projection != scene::ScenePacketError::None ||
        scene::RenderReferenceGBuffer(projected, sceneSource,
            expectedGBuffer) != scene::ScenePacketError::None ||
        raster::RenderReference(projected, expectedColor) !=
            raster::ReferenceRasterError::None) {
        std::cerr << "scene-replay: reference render failed: "
                  << scene::ToString(projection) << '\n';
        return 5;
    }

    abi::AdapterLuid luid{};
    if (!QueryDefaultAdapterLuid(luid)) {
        std::cerr << "scene-replay: D3D adapter query failed\n";
        return 5;
    }
    WindowsBackendModule module{options.backend};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    const auto loaded = host.Load(module, callbacks);
    if (!loaded || !host.RasterAvailable()) {
        std::cerr << "scene-replay: backend load/API failed host="
                  << static_cast<unsigned>(loaded.error)
                  << " backend=" << static_cast<unsigned>(loaded.backendResult)
                  << " win32=" << module.LastErrorCode() << '\n';
        return 6;
    }
    abi::RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.flags = abi::RasterCreateAnyAdapter |
        (options.validation ? abi::RasterCreateValidation : 0u);
    create.adapterLuid = luid;
    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    const auto created = host.CreateRaster(create, status);
    if (!created) {
        std::cerr << "scene-replay: create failed host="
                  << static_cast<unsigned>(created.error)
                  << " backend=" << static_cast<unsigned>(created.backendResult)
                  << " diagnostic=" << status.diagnostic << '\n';
        return 7;
    }

    SceneSubmission ordered;
    auto passed = SubmitScene(host, packetBytes, frameBytes, &sceneBytes,
        options.width, options.height, false, ordered);
    if (!passed) {
        std::cerr << "scene-replay: ordered submission failed diagnostic="
                  << ordered.status.diagnostic << '\n';
    }

    const auto gbufferComparison = scene::CompareGBuffer(
        expectedGBuffer.pixels, ordered.gbuffer);
    const auto interior = CompareInteriorPixels(
        expectedGBuffer, ordered.gbuffer, 1.0e-4f);
    const auto colorComparison = raster::CompareRaster(
        expectedColor.pixels, ordered.image.pixels);
    const auto pixelCount = static_cast<std::uint64_t>(options.width) *
        options.height;
    std::uint64_t expectedCounts[kSceneObjectCount]{};
    std::uint64_t actualCounts[kSceneObjectCount]{};
    std::uint64_t coveredPixels{};
    for (std::uint32_t index = 0; index < kSceneObjectCount; ++index) {
        expectedCounts[index] = CountObjectPixels(
            expectedGBuffer.pixels, kSceneObjectIdBase + index);
        actualCounts[index] = CountObjectPixels(
            ordered.gbuffer, kSceneObjectIdBase + index);
        coveredPixels += actualCounts[index];
    }
    // Covered pixels may differ by interpolation rounding only; uncovered
    // pixels must reproduce the cleared world state exactly, no pixel may
    // disagree on identity, and every non-silhouette pixel must match.
    const auto gbufferPass =
        gbufferComparison.differingPixels <= coveredPixels &&
        gbufferComparison.identityMismatches <= pixelCount / 1000 &&
        gbufferComparison.maximumAbsoluteError <= 1.0e-3f &&
        gbufferComparison.meanAbsoluteError <= 1.0e-5 &&
        interior.interiorPixels > pixelCount / 4 &&
        interior.mismatchedPixels == 0 &&
        interior.maximumInteriorError <= 1.0e-4f;
    const auto colorPass = colorComparison.Within(
        8, 0.5, pixelCount / 10);
    // Object 1 is fully occluded, so neither renderer may keep it, and the
    // partially occluded object 2 must cover fewer pixels than object 0.
    const auto occlusionPass = expectedCounts[1] == 0 &&
        actualCounts[1] == 0 && actualCounts[0] > pixelCount / 32 &&
        actualCounts[2] > pixelCount / 64 &&
        actualCounts[2] < actualCounts[0];

    auto reversedScene = sceneSource;
    std::reverse(reversedScene.objects.begin(), reversedScene.objects.end());
    std::vector<std::byte> reversedBytes;
    SceneSubmission reversed;
    const auto reversedEncoded = scene::EncodeScenePacket(
        reversedScene, reversedBytes) == scene::ScenePacketError::None;
    const auto reversedSubmitted = reversedEncoded && SubmitScene(
        host, packetBytes, frameBytes, &reversedBytes,
        options.width, options.height, false, reversed);
    const auto orderComparison = reversedSubmitted
        ? scene::CompareGBuffer(ordered.gbuffer, reversed.gbuffer)
        : scene::GBufferComparison{};
    const auto orderPass = reversedSubmitted &&
        orderComparison.Within(0.0f, 0.0, 0);

    auto duplicateScene = sceneSource;
    duplicateScene.objects[1].objectId = duplicateScene.objects[0].objectId;
    std::vector<std::byte> duplicateBytes;
    const auto duplicateEncoded = scene::EncodeScenePacket(
        duplicateScene, duplicateBytes);
    // Encoding must already fail closed; the ABI is fed the raw bytes of a
    // packet whose duplicate identity was forced after checksumming.
    auto forgedBytes = sceneBytes;
    scene::ScenePacketHeaderV1 forgedHeader{};
    std::memcpy(&forgedHeader, forgedBytes.data(), sizeof(forgedHeader));
    scene::OpaqueObjectV1 forgedObject{};
    std::memcpy(&forgedObject,
        forgedBytes.data() + forgedHeader.objectsOffset +
            sizeof(scene::OpaqueObjectV1),
        sizeof(forgedObject));
    forgedObject.objectId = sceneSource.objects[0].objectId;
    std::memcpy(forgedBytes.data() + forgedHeader.objectsOffset +
            sizeof(scene::OpaqueObjectV1),
        &forgedObject, sizeof(forgedObject));
    forgedHeader.payloadCrc32 = trace::Crc32(
        std::span<const std::byte>{forgedBytes}.subspan(
            sizeof(forgedHeader)));
    std::memcpy(forgedBytes.data(), &forgedHeader, sizeof(forgedHeader));
    SceneSubmission duplicate;
    const auto duplicateRejected =
        duplicateEncoded == scene::ScenePacketError::DuplicateObject &&
        !SubmitScene(host, packetBytes, frameBytes, &forgedBytes,
            options.width, options.height, false, duplicate) &&
        duplicate.status.result == abi::Result::RasterInvalidPacket;

    SceneSubmission unframed;
    const std::vector<std::byte> noFrame;
    const auto unframedRejected = !SubmitScene(host, packetBytes, noFrame,
        &sceneBytes, options.width, options.height, false, unframed);

    view::FramePacket unclassifiedFrame;
    std::vector<std::byte> unclassifiedBytes;
    SceneSubmission unclassified;
    const auto unclassifiedRejected = BuildSceneFrame(
            options.width, options.height, true,
            unclassifiedFrame, unclassifiedBytes) &&
        !SubmitScene(host, packetBytes, unclassifiedBytes, &sceneBytes,
            options.width, options.height, false, unclassified) &&
        unclassified.status.result == abi::Result::RasterInvalidPacket;

    SceneSubmission undersized;
    const auto undersizedRejected = !SubmitScene(host, packetBytes,
        frameBytes, &sceneBytes, options.width, options.height, true,
        undersized);

    const auto rejectionPass = duplicateRejected && unframedRejected &&
        unclassifiedRejected && undersizedRejected;
    const auto submissionsPass = undersized.status.submissionCount ==
        (reversedSubmitted ? 2u : 1u);

    auto wrote = !ordered.image.pixels.empty() &&
        WritePpm(options.output, ordered.image);
    if (!options.sceneOutput.empty()) {
        wrote = wrote && WriteBinaryFile(options.sceneOutput, sceneBytes);
    }
    if (!options.gbufferOutput.empty()) {
        wrote = wrote && WriteGBuffer(options.gbufferOutput, ordered.gbuffer);
    }
    if (!wrote) {
        std::cerr << "scene-replay: artifact write failed\n";
    }

    status = {};
    status.structSize = sizeof(status);
    const auto destroyed = host.DestroyRaster(status);
    const auto validationErrors = status.validationErrorCount;
    const auto shutdown = host.RequestShutdown();
    const auto lifecyclePass = destroyed && validationErrors == 0 &&
        shutdown.error == BackendHostError::ShutdownDeferred;

    passed = passed && gbufferPass && colorPass && occlusionPass &&
        orderPass && rejectionPass && submissionsPass && wrote &&
        lifecyclePass;
    std::cout << "scene-replay extent=" << options.width << 'x'
              << options.height
              << " objects=" << sceneSource.objects.size()
              << " opaque-passes=" << coverage.opaquePasses
              << " mirrored-passes=" << coverage.mirroredPasses
              << " deferred-classes=" << coverage.deferredClasses
              << " unknown-world-writers=" << coverage.unknownWorldWriters
              << " gbuffer-differing=" << gbufferComparison.differingPixels
              << " gbuffer-identity-mismatches="
              << gbufferComparison.identityMismatches
              << " gbuffer-max-error="
              << gbufferComparison.maximumAbsoluteError
              << " gbuffer-mean-error=" << gbufferComparison.meanAbsoluteError
              << " interior=" << interior.interiorPixels
              << " interior-mismatches=" << interior.mismatchedPixels
              << " interior-max-error=" << interior.maximumInteriorError
              << " color-differing=" << colorComparison.differingPixels
              << " color-max-error=" << colorComparison.maximumChannelError
              << " near-pixels=" << actualCounts[0]
              << " occluded-pixels=" << actualCounts[1]
              << " rotated-pixels=" << actualCounts[2]
              << " order-independent=" << (orderPass ? "yes" : "no")
              << " rejections=" << (rejectionPass ? "pass" : "fail")
              << " submissions=" << undersized.status.submissionCount
              << " validation-errors=" << validationErrors
              << " gbuffer=" << (gbufferPass ? "pass" : "fail")
              << " color=" << (colorPass ? "pass" : "fail")
              << " occlusion=" << (occlusionPass ? "pass" : "fail")
              << " output=" << options.output.string()
              << " result=" << (passed ? "pass" : "fail") << '\n';
    return passed ? 0 : 17;
}

bool ParseInstancedRenderOptions(
    const int argc,
    const char* const* argv,
    const int firstOption,
    InstancedRenderOptions& options)
{
    for (int index = firstOption; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--backend" && index + 1 < argc) {
            options.backend = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else if (argument == "--scene-output" && index + 1 < argc) {
            options.sceneOutput = argv[++index];
        } else if (argument == "--gbuffer-output" && index + 1 < argc) {
            options.gbufferOutput = argv[++index];
        } else if (argument == "--trace-output" && index + 1 < argc) {
            options.traceOutput = argv[++index];
        } else if (argument == "--width" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.width)) return false;
        } else if (argument == "--height" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.height)) return false;
        } else if (argument == "--validation") {
            options.validation = true;
        } else {
            return false;
        }
    }
    return !options.backend.empty() && !options.output.empty() &&
        options.width >= 32 && options.height >= 32 &&
        options.width <= raster::kMaximumExtent &&
        options.height <= raster::kMaximumExtent;
}

std::vector<std::uint64_t> VisibleInstanceIds(
    const std::span<const scene::GBufferPixelV1> pixels)
{
    std::vector<std::uint64_t> identities;
    for (const auto& pixel : pixels) {
        const auto objectId = static_cast<std::uint64_t>(pixel.objectId[0]) |
            (static_cast<std::uint64_t>(pixel.objectId[1]) << 32);
        if (objectId == 0) continue;
        if (std::find(identities.begin(), identities.end(), objectId) ==
            identities.end()) {
            identities.push_back(objectId);
        }
    }
    std::sort(identities.begin(), identities.end());
    return identities;
}

bool WriteRegistryTrace(
    const std::filesystem::path& path,
    const std::vector<scene::SceneDelta>& deltas,
    std::uint64_t& recordedDeltas)
{
    recordedDeltas = 0;
    trace::TraceWriter writer;
    if (writer.Begin({0x5646'0000'0000'1200ull, 10'000'000, 100}) !=
            trace::TraceError::None ||
        writer.Write(trace::FrameBegin{kSceneFrameIndex, 110, 1, 0},
            0x1200) != trace::TraceError::None) {
        return false;
    }
    for (const auto& delta : deltas) {
        if (writer.Write(scene::ToTraceRecord(delta, kSceneFrameIndex),
                0x1201 + recordedDeltas) != trace::TraceError::None) {
            return false;
        }
        ++recordedDeltas;
    }
    if (writer.Write(trace::FrameEnd{kSceneFrameIndex, 120, 1, 0, 0, 0},
            0x12FF) != trace::TraceError::None ||
        writer.Finish({0x5646'0000'0000'1200ull, 1, 130}) !=
            trace::TraceError::None) {
        return false;
    }
    const auto inspection = trace::InspectTrace(writer.Bytes());
    if (!inspection || inspection.summary.registryDeltaCount !=
        recordedDeltas) {
        return false;
    }
    const std::vector<std::byte> bytes(
        writer.Bytes().begin(), writer.Bytes().end());
    return path.empty() || WriteBinaryFile(path, bytes);
}

struct InstancedFrame
{
    scene::ScenePacket scene;
    std::vector<std::byte> sceneBytes;
    raster::DecodedPacket source;
    std::vector<std::byte> packetBytes;
    scene::GBufferImage expected;
    SceneSubmission submission;
    std::vector<std::uint64_t> visible;
    scene::GBufferComparison comparison{};
    InteriorComparison interior{};
    bool valid{};
};

bool RenderInstancedFrame(
    BackendHost& host,
    const scene::SceneDatabase& database,
    const std::vector<scene::InstanceHandle>& handles,
    const view::FramePacket& frame,
    const std::vector<std::byte>& frameBytes,
    const InstancedRenderOptions& options,
    InstancedFrame& rendered)
{
    rendered = {};
    std::vector<std::uint32_t> meshOrder;
    if (!BuildInstancedScene(database, handles, meshOrder, rendered.scene) ||
        !BuildInstancedSource(options.width, options.height, meshOrder,
            rendered.source, rendered.packetBytes) ||
        scene::EncodeScenePacket(rendered.scene, rendered.sceneBytes) !=
            scene::ScenePacketError::None) {
        std::cerr << "instanced-replay: scene construction failed\n";
        return false;
    }
    scene::SceneCoverage coverage{};
    if (scene::ValidateSceneAgainstFrame(
            rendered.scene, frame, coverage) !=
            scene::ScenePacketError::None ||
        !coverage.MirrorEligible()) {
        std::cerr << "instanced-replay: pass accounting rejected the scene\n";
        return false;
    }
    raster::DecodedPacket projected;
    if (scene::ProjectScenePacket(rendered.source, frame.views.front(),
            rendered.scene, projected) != scene::ScenePacketError::None ||
        scene::RenderReferenceGBuffer(projected, rendered.scene,
            rendered.expected) != scene::ScenePacketError::None) {
        std::cerr << "instanced-replay: reference render failed\n";
        return false;
    }
    if (!SubmitScene(host, rendered.packetBytes, frameBytes,
            &rendered.sceneBytes, options.width, options.height, false,
            rendered.submission)) {
        std::cerr << "instanced-replay: submission failed diagnostic="
                  << rendered.submission.status.diagnostic << '\n';
        return false;
    }
    rendered.comparison = scene::CompareGBuffer(
        rendered.expected.pixels, rendered.submission.gbuffer);
    rendered.interior = CompareInteriorPixels(
        rendered.expected, rendered.submission.gbuffer, 1.0e-4f);
    rendered.visible = VisibleInstanceIds(rendered.submission.gbuffer);
    rendered.valid = true;
    return true;
}

int RenderInstancedScene(const InstancedRenderOptions& options)
{
    view::FramePacket frame;
    std::vector<std::byte> frameBytes;
    if (!BuildSceneFrame(options.width, options.height, false,
            frame, frameBytes)) {
        std::cerr << "instanced-replay: frame fixture failed\n";
        return 5;
    }

    scene::SceneDatabase database{1u << 20};
    std::vector<scene::InstanceHandle> handles;
    std::uint64_t timeline = 1;
    if (database.AttachGroup(1, timeline) != scene::DatabaseError::None ||
        database.AttachGroup(2, timeline) != scene::DatabaseError::None) {
        std::cerr << "instanced-replay: cell attach failed\n";
        return 5;
    }
    for (const auto& placement : InstancedPlacements()) {
        const auto& mesh = InstancedMeshes()[placement.meshIndex];
        scene::GeometryDesc geometry{};
        geometry.address = placement.geometryAddress;
        geometry.contentHash = mesh.contentHash;
        geometry.byteSize = 4096;
        geometry.usage = resource::ResourceUsage::Immutable;
        scene::InstanceDesc instance{};
        instance.sourceId = placement.geometryAddress;
        instance.materialId = mesh.materialId;
        instance.groupId = placement.groupId;
        instance.passSequence = mesh.passSequence;
        instance.model = InstancedModel(
            placement.scale, placement.translation);
        instance.parameters = placement.parameters;
        scene::InstanceHandle handle{};
        if (database.AddInstance(geometry, instance, timeline, handle) !=
            scene::DatabaseError::None) {
            std::cerr << "instanced-replay: instance registration failed\n";
            return 5;
        }
        handles.push_back(handle);
    }
    for (const auto& mesh : InstancedMeshes()) {
        static_cast<void>(mesh);
    }
    // Only the first address of each content hash owns an upload; the
    // shared copies are already resident through it.
    if (database.CompleteUpload(0x7000'0001, 1, timeline) !=
            scene::DatabaseError::None ||
        database.CompleteUpload(0x7000'0004, 1, timeline) !=
            scene::DatabaseError::None) {
        std::cerr << "instanced-replay: upload completion failed\n";
        return 5;
    }
    const auto populated = database.Stats();
    const auto dedupPass = populated.aliveInstances == 5 &&
        populated.residentGeometries == 2 &&
        populated.sharedInstances == 3 &&
        populated.residentBytes == 8192 &&
        populated.pendingUploads == 0;

    abi::AdapterLuid luid{};
    if (!QueryDefaultAdapterLuid(luid)) {
        std::cerr << "instanced-replay: D3D adapter query failed\n";
        return 5;
    }
    WindowsBackendModule module{options.backend};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    const auto loaded = host.Load(module, callbacks);
    if (!loaded || !host.RasterAvailable()) {
        std::cerr << "instanced-replay: backend load/API failed host="
                  << static_cast<unsigned>(loaded.error)
                  << " backend=" << static_cast<unsigned>(loaded.backendResult)
                  << " win32=" << module.LastErrorCode() << '\n';
        return 6;
    }
    abi::RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.flags = abi::RasterCreateAnyAdapter |
        (options.validation ? abi::RasterCreateValidation : 0u);
    create.adapterLuid = luid;
    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    if (!host.CreateRaster(create, status)) {
        std::cerr << "instanced-replay: create failed diagnostic="
                  << status.diagnostic << '\n';
        return 7;
    }

    InstancedFrame settlement;
    auto passed = RenderInstancedFrame(host, database, handles, frame,
        frameBytes, options, settlement);

    // Cell transition: the second group detaches while the first survives.
    ++timeline;
    if (database.DetachGroup(2, timeline) != scene::DatabaseError::None ||
        database.Retire(timeline) != 2) {
        std::cerr << "instanced-replay: cell detach failed\n";
        passed = false;
    }
    view::FramePacket transitionFrame;
    std::vector<std::byte> transitionFrameBytes;
    if (!BuildSceneFrame(options.width, options.height, false,
            transitionFrame, transitionFrameBytes, false)) {
        std::cerr << "instanced-replay: transition frame fixture failed\n";
        passed = false;
    }
    const auto settled = database.Stats();
    const auto releasePass = settled.aliveInstances == 3 &&
        settled.retiringInstances == 0 &&
        settled.residentGeometries == 1 &&
        settled.residentBytes == 4096 &&
        settled.attachedGroups == 1;

    InstancedFrame transitioned;
    passed = RenderInstancedFrame(host, database, handles, transitionFrame,
        transitionFrameBytes, options, transitioned) && passed;

    const auto pixelCount = static_cast<std::uint64_t>(options.width) *
        options.height;
    // Five instanced silhouettes give the oracle's coverage rule and
    // hardware rasterization more edges to disagree on, so the bound is on
    // how many pixels may disagree, not on how far an edge pixel may swing.
    // Every interior pixel must still match within 1e-4.
    const auto parityPass = [pixelCount](const InstancedFrame& value) {
        return value.valid &&
            value.comparison.identityMismatches <= pixelCount / 1000 &&
            value.comparison.meanAbsoluteError <= 1.0e-4 &&
            value.interior.interiorPixels > pixelCount / 4 &&
            value.interior.mismatchedPixels == 0 &&
            value.interior.maximumInteriorError <= 1.0e-4f;
    };
    const auto instancingPass = settlement.valid && transitioned.valid &&
        settlement.visible.size() == 5 && transitioned.visible.size() == 3 &&
        settlement.scene.objects.size() == 2 &&
        settlement.scene.instances.size() == 5 &&
        transitioned.scene.objects.size() == 1 &&
        transitioned.scene.instances.size() == 3 &&
        settlement.source.draws.size() == 2 &&
        transitioned.source.draws.size() == 1 &&
        std::includes(settlement.visible.begin(), settlement.visible.end(),
            transitioned.visible.begin(), transitioned.visible.end());

    // Repeated load/unload cycles must return to the same plateau.
    std::uint64_t plateauBytes{};
    std::uint32_t plateauDescriptors{};
    auto plateauPass = true;
    for (std::uint32_t cycle = 0; cycle < 4; ++cycle) {
        ++timeline;
        plateauPass = plateauPass &&
            database.AttachGroup(10 + cycle, timeline) ==
                scene::DatabaseError::None;
        for (const auto& placement : InstancedPlacements()) {
            if (placement.groupId != 2) continue;
            const auto& mesh = InstancedMeshes()[placement.meshIndex];
            scene::GeometryDesc geometry{};
            geometry.address = placement.geometryAddress;
            geometry.contentHash = mesh.contentHash;
            geometry.byteSize = 4096;
            geometry.usage = resource::ResourceUsage::Immutable;
            scene::InstanceDesc instance{};
            instance.sourceId = placement.geometryAddress;
            instance.materialId = mesh.materialId;
            instance.groupId = 10 + cycle;
            instance.passSequence = mesh.passSequence;
            instance.model = InstancedModel(
                placement.scale, placement.translation);
            instance.parameters = placement.parameters;
            scene::InstanceHandle handle{};
            plateauPass = plateauPass &&
                database.AddInstance(geometry, instance, timeline, handle) ==
                    scene::DatabaseError::None;
        }
        const auto peak = database.Stats();
        plateauPass = plateauPass &&
            database.DetachGroup(10 + cycle, timeline) ==
                scene::DatabaseError::None &&
            database.Retire(timeline) == 2;
        if (cycle == 0) {
            plateauBytes = peak.residentBytes;
            plateauDescriptors = peak.peakDescriptorIndex;
        } else {
            plateauPass = plateauPass && peak.residentBytes == plateauBytes &&
                peak.peakDescriptorIndex == plateauDescriptors;
        }
    }
    const auto final = database.Stats();
    plateauPass = plateauPass && final.aliveInstances == 3 &&
        final.retiringInstances == 0 && final.residentGeometries == 1;

    std::uint64_t recordedDeltas{};
    const auto deltas = database.DrainDeltas();
    const auto tracePass = !deltas.empty() &&
        WriteRegistryTrace(options.traceOutput, deltas, recordedDeltas);

    auto wrote = settlement.valid &&
        !settlement.submission.image.pixels.empty() &&
        WritePpm(options.output, settlement.submission.image);
    if (!options.sceneOutput.empty()) {
        wrote = wrote &&
            WriteBinaryFile(options.sceneOutput, settlement.sceneBytes);
    }
    if (!options.gbufferOutput.empty()) {
        wrote = wrote && WriteGBuffer(options.gbufferOutput,
            settlement.submission.gbuffer);
    }
    if (!wrote) {
        std::cerr << "instanced-replay: artifact write failed\n";
    }

    status = {};
    status.structSize = sizeof(status);
    const auto destroyed = host.DestroyRaster(status);
    const auto validationErrors = status.validationErrorCount;
    const auto shutdown = host.RequestShutdown();
    const auto lifecyclePass = destroyed && validationErrors == 0 &&
        shutdown.error == BackendHostError::ShutdownDeferred;

    passed = passed && dedupPass && releasePass && instancingPass &&
        parityPass(settlement) && parityPass(transitioned) && plateauPass &&
        tracePass && wrote && lifecyclePass;
    std::cout << "instanced-replay extent=" << options.width << 'x'
              << options.height
              << " objects=" << settlement.scene.objects.size()
              << " instances=" << settlement.scene.instances.size()
              << " draws=" << settlement.source.draws.size()
              << " resident-geometries=" << populated.residentGeometries
              << " shared-instances=" << populated.sharedInstances
              << " resident-bytes=" << populated.residentBytes
              << " visible-before=" << settlement.visible.size()
              << " visible-after=" << transitioned.visible.size()
              << " released-geometries="
              << (populated.residentGeometries - settled.residentGeometries)
              << " gbuffer-identity-mismatches="
              << settlement.comparison.identityMismatches
              << " gbuffer-max-error="
              << settlement.comparison.maximumAbsoluteError
              << " gbuffer-mean-error="
              << settlement.comparison.meanAbsoluteError
              << " interior=" << settlement.interior.interiorPixels
              << " interior-mismatches="
              << settlement.interior.mismatchedPixels
              << " transition-identity-mismatches="
              << transitioned.comparison.identityMismatches
              << " registry-deltas=" << recordedDeltas
              << " plateau-bytes=" << plateauBytes
              << " submissions="
              << transitioned.submission.status.submissionCount
              << " validation-errors=" << validationErrors
              << " dedup=" << (dedupPass ? "pass" : "fail")
              << " release=" << (releasePass ? "pass" : "fail")
              << " instancing=" << (instancingPass ? "pass" : "fail")
              << " parity="
              << (parityPass(settlement) && parityPass(transitioned)
                    ? "pass" : "fail")
              << " plateau=" << (plateauPass ? "pass" : "fail")
              << " trace=" << (tracePass ? "pass" : "fail")
              << " output=" << options.output.string()
              << " result=" << (passed ? "pass" : "fail") << '\n';
    return passed ? 0 : 18;
}

bool ParseDeformedRenderOptions(
    const int argc,
    const char* const* argv,
    const int firstOption,
    DeformedRenderOptions& options)
{
    for (int index = firstOption; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--backend" && index + 1 < argc) {
            options.backend = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else if (argument == "--deform-output" && index + 1 < argc) {
            options.deformOutput = argv[++index];
        } else if (argument == "--gbuffer-output" && index + 1 < argc) {
            options.gbufferOutput = argv[++index];
        } else if (argument == "--width" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.width)) return false;
        } else if (argument == "--height" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.height)) return false;
        } else if (argument == "--frames" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.frames)) return false;
        } else if (argument == "--validation") {
            options.validation = true;
        } else {
            return false;
        }
    }
    return !options.backend.empty() && !options.output.empty() &&
        options.width >= 32 && options.height >= 32 &&
        options.frames >= 1 && options.frames <= 64 &&
        options.width <= raster::kMaximumExtent &&
        options.height <= raster::kMaximumExtent;
}

struct DeformSubmission
{
    std::vector<DeformOutputRecord> output;
    scene::GBufferImage expected;
    SceneSubmission submission;
    scene::GBufferComparison comparison{};
    InteriorComparison interior{};
    float maximumPositionError{};
    float maximumMotionError{};
    float referenceMotion{};
    bool valid{};
};

bool SubmitDeformedFrame(
    BackendHost& host,
    const std::vector<std::byte>& packetBytes,
    const std::vector<std::byte>& frameBytes,
    const std::vector<std::byte>& sceneBytes,
    const std::vector<std::byte>& deformBytes,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t vertexCount,
    DeformSubmission& rendered)
{
    const auto pixelCount = static_cast<std::size_t>(width) * height;
    try {
        rendered.output.assign(vertexCount, DeformOutputRecord{});
        rendered.submission.gbuffer.assign(pixelCount,
            scene::GBufferPixelV1{});
        rendered.submission.image.width = width;
        rendered.submission.image.height = height;
        rendered.submission.image.pixels.assign(pixelCount, raster::Rgba8{});
    } catch (...) {
        return false;
    }
    abi::RasterFrameRequestV1 request{};
    request.structSize = sizeof(request);
    request.packetData = reinterpret_cast<std::uintptr_t>(packetBytes.data());
    request.packetSize = packetBytes.size();
    request.outputData = reinterpret_cast<std::uintptr_t>(
        rendered.submission.image.pixels.data());
    request.outputRowPitch = width * sizeof(raster::Rgba8);
    request.outputCapacity =
        static_cast<std::uint64_t>(request.outputRowPitch) * height;
    request.frameData = reinterpret_cast<std::uintptr_t>(frameBytes.data());
    request.frameSize = frameBytes.size();
    request.sceneData = reinterpret_cast<std::uintptr_t>(sceneBytes.data());
    request.sceneSize = sceneBytes.size();
    request.gbufferData = reinterpret_cast<std::uintptr_t>(
        rendered.submission.gbuffer.data());
    request.gbufferCapacity = rendered.submission.gbuffer.size() *
        sizeof(scene::GBufferPixelV1);
    request.deformationData = reinterpret_cast<std::uintptr_t>(
        deformBytes.data());
    request.deformationSize = deformBytes.size();
    request.deformationOutputData = reinterpret_cast<std::uintptr_t>(
        rendered.output.data());
    request.deformationOutputCapacity =
        rendered.output.size() * sizeof(DeformOutputRecord);
    rendered.submission.status = {};
    rendered.submission.status.structSize =
        sizeof(rendered.submission.status);
    const auto result = host.RenderRasterFrame(
        request, rendered.submission.status);
    rendered.submission.submitted = static_cast<bool>(result);
    return rendered.submission.submitted;
}

bool ParseAlphaRenderOptions(
    const int argc,
    const char* const* argv,
    const int firstOption,
    AlphaRenderOptions& options)
{
    for (int index = firstOption; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--backend" && index + 1 < argc) {
            options.backend = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else if (argument == "--scene-output" && index + 1 < argc) {
            options.sceneOutput = argv[++index];
        } else if (argument == "--gbuffer-output" && index + 1 < argc) {
            options.gbufferOutput = argv[++index];
        } else if (argument == "--width" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.width)) return false;
        } else if (argument == "--height" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.height)) return false;
        } else if (argument == "--validation") {
            options.validation = true;
        } else {
            return false;
        }
    }
    return !options.backend.empty() && !options.output.empty() &&
        options.width >= 32 && options.height >= 32 &&
        options.width <= raster::kMaximumExtent &&
        options.height <= raster::kMaximumExtent;
}

bool ParseFamilyRenderOptions(
    const int argc,
    const char* const* argv,
    const int firstOption,
    FamilyRenderOptions& options)
{
    for (int index = firstOption; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--backend" && index + 1 < argc) {
            options.backend = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else if (argument == "--scene-output" && index + 1 < argc) {
            options.sceneOutput = argv[++index];
        } else if (argument == "--family-output" && index + 1 < argc) {
            options.familyOutput = argv[++index];
        } else if (argument == "--light-output" && index + 1 < argc) {
            options.lightOutput = argv[++index];
        } else if (argument == "--lit") {
            options.lit = true;
        } else if (argument == "--shadows") {
            options.lit = true;
            options.shadows = true;
        } else if (argument == "--reflections") {
            options.lit = true;
            options.shadows = true;
            options.reflections = true;
        } else if (argument == "--indirect") {
            options.lit = true;
            options.shadows = true;
            options.reflections = true;
            options.indirect = true;
        } else if (argument == "--transparency") {
            // Lit, but without shadows or indirect. This contract is about the
            // composite, and inheriting the shadow gate means inheriting a
            // ratio bound tuned for a three-object scene -- which a fixture
            // that adds two more trips by one pixel, reporting a shadow
            // regression that is really just a larger scene. The bounce term
            // is left off for the same reason it is elsewhere: it is a
            // stochastic estimator, and a one-way "never darker" bound
            // cannot hold against noise that is not the thing under test.
            // Reflections come with the shadow fixture's reflective object, so
            // they cannot be kept without it. Lit alone is what this contract
            // needs: the composite is over whatever is beneath it, and which
            // ray-traced terms produced that is not the thing under test.
            options.lit = true;
            options.transparency = true;
        } else if (argument == "--gbuffer-output" && index + 1 < argc) {
            options.gbufferOutput = argv[++index];
        } else if (argument == "--width" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.width)) return false;
        } else if (argument == "--height" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.height)) return false;
        } else if (argument == "--validation") {
            options.validation = true;
        } else {
            return false;
        }
    }
    return !options.backend.empty() && !options.output.empty() &&
        options.width >= 32 && options.height >= 32 &&
        options.width <= raster::kMaximumExtent &&
        options.height <= raster::kMaximumExtent;
}

// Runs the temporal half of the indirect pass on the device and against the
// host oracle, over a fixture built to reach every rejection reason the gate
// has. Comparing whole histories rather than a picture, because accumulation
// has no picture of its own: it is what decides how quickly one appears, and a
// pass that blended a rejected sample would look like a slightly soft frame
// rather than like a trail behind everything that moved.
int RenderIndirectAccumulation(const FamilyRenderOptions& options)
{
    // Eight pixels wide so a motion vector can move a sample within the row
    // and off the end of it, which is what separates a reprojection that
    // reads the named pixel from one that reads its own.
    constexpr std::uint32_t kWidth = 8;
    constexpr std::uint32_t kHeight = 4;
    constexpr std::uint32_t kPixels = kWidth * kHeight;

    gi::IndirectRules rules{};
    gi::QualityPreset preset{};
    // A key with a real extent. A default one has no extent, which means a
    // history that was never established, and ResetHistory rejects every pixel
    // for that alone -- the fixture would then exercise one gate and agree with
    // itself about nothing else.
    reflect::ReflectionHistoryKey epoch{};
    epoch.cameraEpoch = 1;
    epoch.width = kWidth;
    epoch.height = kHeight;
    epoch.viewId = 1;

    std::vector<gi::SurfaceSample> current(kPixels);
    std::vector<gi::SurfaceSample> previous(kPixels);
    std::vector<std::array<float, 2>> motion(kPixels);
    std::vector<std::array<float, 3>> radiance(kPixels);
    std::vector<gi::HistorySample> history(kPixels);
    for (std::uint32_t index = 0; index < kPixels; ++index) {
        const auto step = static_cast<float>(index);
        gi::SurfaceSample surface{};
        surface.geometricNormal = {0.0f, 0.0f, 1.0f};
        surface.depth = 100.0f + step;
        surface.objectId = 0x1000ull + index;
        surface.materialId = 0x2000ull + index;
        current[index] = surface;
        previous[index] = surface;
        motion[index] = {0.0f, 0.0f};
        radiance[index] = {0.05f * step, 0.02f * step, 0.01f * step};
        history[index].mean = {0.1f, 0.2f, 0.3f};
        history[index].secondMoment = {0.05f, 0.09f, 0.14f};
        // A spread of history lengths, including zero, which resets whatever
        // the gate says and is a different path through the accumulator.
        history[index].length = index % 5;
    }
    // One pixel per rejection reason, so a gate that stopped rejecting would
    // change an answer here rather than pass unexercised.
    previous[1].depth = current[1].depth * 4.0f;             // Depth
    previous[2].geometricNormal = {1.0f, 0.0f, 0.0f};        // Normal
    previous[3].objectId = current[3].objectId + 1;          // Object
    previous[4].materialId = current[4].materialId + 1;      // Material
    motion[5] = {100.0f, 0.0f};                              // OffScreen
    motion[6] = {-2.0f, 0.0f};                               // accepted, moved
    motion[7] = {std::numeric_limits<float>::quiet_NaN(), 0.0f};

    // A depth pair that a relative tolerance accepts and an absolute one
    // rejects. Without it, every pixel either agrees exactly or differs by
    // hundreds, and the two tolerances cannot be told apart -- which is the
    // difference between a filter that works at distance and one that rejects
    // every far pixel forever.
    current[9].depth = 1000.0f;
    previous[9].depth = 1002.0f;

    // A history whose second moment is below the square of its mean, so the
    // raw variance is negative and the clamp at zero is the only thing
    // stopping it reaching the spatial filter, where a negative variance would
    // widen the kernel exactly where it should narrow.
    history[10].mean = {1.0f, 1.0f, 1.0f};
    history[10].secondMoment = {0.1f, 0.1f, 0.1f};
    history[10].length = 4;
    radiance[10] = {1.0f, 1.0f, 1.0f};

    // Fractional negative motion, which is the only input that separates
    // truncating the motion from flooring the sum. At minus one and a half the
    // two name adjacent pixels, and those two hold different histories, so the
    // choice changes the answer instead of being invisible.
    motion[11] = {-1.5f, 0.0f};

    std::vector<gi::GpuIndirectPixelV1> currentRecords(kPixels);
    std::vector<gi::GpuIndirectPixelV1> previousRecords(kPixels);
    std::vector<gi::GpuIndirectHistoryV1> historyRecords(kPixels);
    for (std::uint32_t index = 0; index < kPixels; ++index) {
        currentRecords[index] = gi::BuildGpuIndirectPixel(current[index],
            motion[index], radiance[index]);
        previousRecords[index] = gi::BuildGpuIndirectPixel(previous[index],
            {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
        historyRecords[index] = gi::BuildGpuIndirectHistory(history[index]);
    }

    abi::AdapterLuid luid{};
    if (!QueryDefaultAdapterLuid(luid)) {
        std::cerr << "indirect-replay: D3D adapter query failed" << (char)10;
        return 5;
    }
    WindowsBackendModule module{options.backend};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    if (!host.Load(module, callbacks) || !host.RasterAvailable()) {
        std::cerr << "indirect-replay: backend load failed" << (char)10;
        return 6;
    }
    abi::RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.flags = abi::RasterCreateAnyAdapter |
        (options.validation ? abi::RasterCreateValidation : 0u);
    create.adapterLuid = luid;
    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    if (!host.CreateRaster(create, status)) {
        std::cerr << "indirect-replay: create failed diagnostic="
                  << status.diagnostic << (char)10;
        return 7;
    }

    // A minimal raster frame, because the pass rides on a submission and a
    // submission needs something to render. What it draws is irrelevant here.
    const auto packetBytes = raster::BuildSyntheticPacket();
    raster::DecodedPacket decoded{};
    if (!raster::DecodePacket(packetBytes, decoded)) {
        std::cerr << "indirect-replay: fixture packet invalid" << (char)10;
        return 5;
    }
    std::vector<raster::Rgba8> image(
        static_cast<std::size_t>(decoded.header.width) *
        decoded.header.height);
    std::vector<gi::GpuIndirectResultV1> results(kPixels);

    abi::RasterFrameRequestV1 request{};
    request.structSize = sizeof(request);
    request.packetData =
        reinterpret_cast<std::uintptr_t>(packetBytes.data());
    request.packetSize = packetBytes.size();
    request.outputData = reinterpret_cast<std::uintptr_t>(image.data());
    request.outputRowPitch = decoded.header.width * sizeof(raster::Rgba8);
    request.outputCapacity =
        static_cast<std::uint64_t>(request.outputRowPitch) *
        decoded.header.height;
    request.indirectCurrentData =
        reinterpret_cast<std::uintptr_t>(currentRecords.data());
    request.indirectPreviousData =
        reinterpret_cast<std::uintptr_t>(previousRecords.data());
    request.indirectHistoryData =
        reinterpret_cast<std::uintptr_t>(historyRecords.data());
    request.indirectResultData =
        reinterpret_cast<std::uintptr_t>(results.data());
    request.indirectResultCapacity =
        results.size() * sizeof(gi::GpuIndirectResultV1);
    request.indirectPixelCount = kPixels;
    request.indirectWidth = kWidth;
    request.indirectHeight = kHeight;
    request.indirectEpochMatches = 1;
    abi::RasterStatusV1 frameStatus{};
    frameStatus.structSize = sizeof(frameStatus);
    if (!host.RenderRasterFrame(request, frameStatus)) {
        std::cerr << "indirect-replay: submission failed diagnostic="
                  << frameStatus.diagnostic << (char)10;
        return 8;
    }

    std::uint32_t reasonMismatches = 0;
    std::uint32_t lengthMismatches = 0;
    std::uint32_t meanMismatches = 0;
    std::uint32_t varianceMismatches = 0;
    auto worstMean = 0.0f;
    auto worstVariance = 0.0f;
    std::array<std::uint32_t, 7> reasonCounts{};
    for (std::uint32_t index = 0; index < kPixels; ++index) {
        const auto x = index % kWidth;
        const auto y = index / kWidth;
        const auto reprojected = gi::Reproject(current[index],
            previous[index], motion[index], x, y, kWidth, kHeight, rules,
            epoch, epoch);
        const auto sourceIndex =
            reprojected.reason == gi::RejectReason::Accepted
                ? reprojected.sourceY * kWidth + reprojected.sourceX
                : index;
        const auto expected = gi::Accumulate(history[sourceIndex],
            gi::ClampRadiance(radiance[index], rules), reprojected.reason,
            preset);
        const auto expectedVariance = gi::Variance(expected);

        const auto& produced = results[index];
        reasonCounts[static_cast<std::size_t>(reprojected.reason)] += 1;
        if (produced.reason !=
            static_cast<std::uint32_t>(reprojected.reason)) {
            ++reasonMismatches;
        }
        if (produced.samples != expected.length) ++lengthMismatches;
        auto meanError = 0.0f;
        auto varianceError = 0.0f;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            meanError = std::max(meanError, std::abs(
                produced.mean[channel] - expected.mean[channel]));
            varianceError = std::max(varianceError, std::abs(
                produced.variance[channel] - expectedVariance[channel]));
        }
        worstMean = std::max(worstMean, meanError);
        worstVariance = std::max(worstVariance, varianceError);
        // A tolerance for the last bit of a float, not for a difference in
        // what the two computed. The device and the host run the same
        // expression on the same inputs.
        if (meanError > 1.0e-5f) ++meanMismatches;
        if (varianceError > 1.0e-5f) ++varianceMismatches;
    }

    // The fixture has to actually reach the gates it claims to. A pass over
    // pixels that were all accepted would agree with the oracle perfectly and
    // prove nothing about rejection.
    std::uint32_t reasonsSeen = 0;
    for (const auto count : reasonCounts) {
        if (count != 0) ++reasonsSeen;
    }
    // The renderer owns a Vulkan device, and leaving it alive past the end of
    // this function left the device to be torn down after the module that
    // created it had already gone -- which the release build reported as a
    // crash at exit, after every comparison above had already passed.
    // Destroying it here is what every other mode does.
    status = {};
    status.structSize = sizeof(status);
    const auto destroyed = host.DestroyRaster(status);
    const auto shutdown = host.RequestShutdown();
    const auto lifecyclePass = destroyed && status.validationErrorCount == 0 &&
        shutdown.error == BackendHostError::ShutdownDeferred;

    const auto passed = reasonMismatches == 0 && lengthMismatches == 0 &&
        meanMismatches == 0 && varianceMismatches == 0 &&
        reasonsSeen >= 6 && frameStatus.validationErrorCount == 0 &&
        lifecyclePass;
    std::cout << "indirect-replay extent=" << kWidth << char{120} << kHeight
              << " reason-mismatches=" << reasonMismatches
              << " length-mismatches=" << lengthMismatches
              << " mean-mismatches=" << meanMismatches
              << " variance-mismatches=" << varianceMismatches
              << " max-mean-error=" << worstMean
              << " max-variance-error=" << worstVariance
              << " reasons-exercised=" << reasonsSeen
              << " validation-errors=" << frameStatus.validationErrorCount
              << " destroyed=" << (destroyed ? 1 : 0)
              << " lifecycle=" << (lifecyclePass ? "pass" : "fail")
              << " result=" << (passed ? "pass" : "fail") << (char)10;
    return passed ? 0 : 1;
}

// Renders the mirror fixture twice, with and without the target, and counts
// the pixels on the mirror that moved. The bounce is switched off in both, so
// nothing else can carry the target's light there: the lights are identical,
// the target is not visible on the mirror's own pixels, and the indirect path
// returns black. What is left is the traced reflection.
int RenderMirrorScene(const FamilyRenderOptions& options)
{
    view::FramePacket frame{};
    std::vector<std::byte> frameBytes;
    // Without the distant-tree pass. Every opaque pass the frame declares
    // must be covered by an object that writes it, and this fixture is two
    // objects on purpose -- claiming a pass nothing draws is refused, and
    // adding a third object to satisfy it would put something else in the
    // frame for the mirror to catch.
    if (!BuildSceneFrame(options.width, options.height, false,
            frame, frameBytes, false)) {
        std::cerr << "mirror-replay: frame fixture failed" << (char)10;
        return 5;
    }

    lighting::LightPacket lightPacket{};
    lightPacket.header.frameId = kSceneFrameIndex;
    lightPacket.header.viewId = kSceneViewId;
    lighting::LightCapture key{};
    key.lightId = 0x1900'0000'0000'0001ull;
    key.type = lighting::LightType::Directional;
    key.diffuse = {1.0f, 0.98f, 0.95f};
    key.dimmer = 1.6f;
    key.direction = {0.0f, -0.4f, 1.0f};
    lighting::LightRecordV1 translated{};
    if (lighting::TranslateLight(key, translated) !=
        lighting::LightError::None) {
        std::cerr << "mirror-replay: light translate failed" << (char)10;
        return 5;
    }
    lightPacket.lights.push_back(translated);
    lighting::EnvironmentCapture environment{};
    environment.ambient = {0.05f, 0.05f, 0.06f};
    environment.sunDirection = key.direction;
    environment.sunColor = key.diffuse;
    environment.sunIntensity = key.dimmer;
    environment.fog.nearDistance = 2.0f;
    environment.fog.farDistance = 12.0f;
    environment.fog.color = {0.45f, 0.50f, 0.60f};
    environment.fog.power = 1.0f;
    environment.fog.maximum = 0.85f;
    if (lighting::TranslateEnvironment(environment,
            lightPacket.environment) != lighting::LightError::None) {
        std::cerr << "mirror-replay: environment translate failed"
                  << (char)10;
        return 5;
    }
    // Switched off so the target's light cannot reach the mirror by any
    // route except the reflection being measured.
    lightPacket.environment.flags |= lighting::EnvironmentIndirectDisabled;
    std::vector<std::byte> lightBytes;
    if (lighting::EncodeLightPacket(lightPacket, lightBytes) !=
        lighting::LightPacketError::None) {
        std::cerr << "mirror-replay: light encode failed" << (char)10;
        return 5;
    }

    struct Variant
    {
        raster::DecodedPacket source;
        scene::ScenePacket scenePacket;
        std::vector<std::byte> packetBytes;
        std::vector<std::byte> sceneBytes;
        std::vector<std::byte> familyBytes;
        std::vector<std::array<float, 4>> hdr;
        std::vector<scene::GBufferPixelV1> gbuffer;
    };
    // Three renders: the mirror alone, the mirror with a target to reflect,
    // and the same target with the mirror roughened past the tracing cutoff
    // so the shader takes its fallback instead. The first pair says whether
    // the reflection carries the target; the second says whether the traced
    // path differs from the fallback at all, which is the question the
    // previous cycle's probe could not answer because it changed both
    // renders at once.
    const std::array<float, 3> mirrorRoughness{0.02f, 0.02f, 0.90f};
    std::array<Variant, 3> variants{};
    for (std::size_t slot = 0; slot < variants.size(); ++slot) {
        auto& variant = variants[slot];
        if (!BuildMirrorFixture(options.width, options.height, slot != 0,
                mirrorRoughness[slot],
                variant.source, variant.scenePacket)) {
            std::cerr << "mirror-replay: fixture invalid" << (char)10;
            return 5;
        }
        if (!raster::EncodePacket(variant.source, variant.packetBytes) ||
            scene::EncodeScenePacket(variant.scenePacket,
                variant.sceneBytes) != scene::ScenePacketError::None) {
            std::cerr << "mirror-replay: encode failed" << (char)10;
            return 5;
        }
        {
            // Per variant, because a family record naming an object the
            // scene does not own is refused -- and the variants differ by
            // exactly one object.
            material::FamilyPacket familyPacket{};
            familyPacket.header.frameId = kSceneFrameIndex;
            familyPacket.header.viewId = kSceneViewId;
            for (const auto& object : variant.scenePacket.objects) {
                material::FamilyCapture capture{};
                capture.materialId = object.materialId;
                capture.generation = 1;
                capture.revision = 1;
                capture.staticRevision = 1;
                capture.featureId = material::FeatureIdOf(
                    material::MaterialFamily::Default);
                capture.baseTechniqueId = 0x1900;
                capture.tintColor = {1.0f, 1.0f, 1.0f};
                capture.emitColor = {0.0f, 0.0f, 0.0f};
                capture.emitScale = 1.0f;
                capture.subsurfaceRolloff = 0.4f;
                capture.rimPower = 2.0f;
                capture.backlightPower = 1.5f;
                capture.slots[0].resourceId = 0x8000'0000'0000'1901ull;
                capture.slots[0].generation = 1;
                capture.slots[0].authored = true;
                capture.slots[1].resourceId = 0x8000'0000'0000'1902ull;
                capture.slots[1].generation = 1;
                capture.slots[1].authored = true;
                capture.slots[7].resourceId = 0x8000'0000'0000'1903ull;
                capture.slots[7].generation = 1;
                capture.slots[7].authored = true;
                material::FamilyDescriptor descriptor;
                if (material::TranslateMaterialFamily(capture, descriptor) !=
                    material::FamilyError::None) {
                    std::cerr << "mirror-replay: family translate failed"
                              << (char)10;
                    return 5;
                }
                familyPacket.records.push_back(
                    material::MakeFamilyRecord(descriptor, object.objectId));
            }
            if (material::EncodeFamilyPacket(familyPacket,
                    variant.familyBytes) !=
                material::FamilyPacketError::None) {
                std::cerr << "mirror-replay: family encode failed"
                          << (char)10;
                return 5;
            }
        }
    }

    abi::AdapterLuid luid{};
    if (!QueryDefaultAdapterLuid(luid)) {
        std::cerr << "mirror-replay: D3D adapter query failed" << (char)10;
        return 5;
    }
    WindowsBackendModule module{options.backend};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    const auto loaded = host.Load(module, callbacks);
    if (!loaded || !host.RasterAvailable()) {
        std::cerr << "mirror-replay: backend load failed" << (char)10;
        return 6;
    }
    abi::RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.flags = abi::RasterCreateAnyAdapter |
        (options.validation ? abi::RasterCreateValidation : 0u);
    create.adapterLuid = luid;
    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    if (!host.CreateRaster(create, status)) {
        std::cerr << "mirror-replay: create failed diagnostic="
                  << status.diagnostic << (char)10;
        return 7;
    }

    const auto pixelCount =
        static_cast<std::size_t>(options.width) * options.height;
    std::uint32_t validationErrors = 0;
    for (auto& variant : variants) {
        variant.hdr.assign(pixelCount,
            std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
        variant.gbuffer.assign(pixelCount, scene::GBufferPixelV1{});
        std::vector<raster::Rgba8> image(pixelCount);
        abi::RasterFrameRequestV1 request{};
        request.structSize = sizeof(request);
        request.packetData =
            reinterpret_cast<std::uintptr_t>(variant.packetBytes.data());
        request.packetSize = variant.packetBytes.size();
        request.outputData = reinterpret_cast<std::uintptr_t>(image.data());
        request.outputRowPitch = options.width * sizeof(raster::Rgba8);
        request.outputCapacity =
            static_cast<std::uint64_t>(request.outputRowPitch) *
            options.height;
        request.frameData =
            reinterpret_cast<std::uintptr_t>(frameBytes.data());
        request.frameSize = frameBytes.size();
        request.sceneData =
            reinterpret_cast<std::uintptr_t>(variant.sceneBytes.data());
        request.sceneSize = variant.sceneBytes.size();
        request.gbufferData =
            reinterpret_cast<std::uintptr_t>(variant.gbuffer.data());
        request.gbufferCapacity =
            variant.gbuffer.size() * sizeof(scene::GBufferPixelV1);
        request.hdrData = reinterpret_cast<std::uintptr_t>(variant.hdr.data());
        request.hdrCapacity = variant.hdr.size() * sizeof(variant.hdr[0]);
        request.lightData =
            reinterpret_cast<std::uintptr_t>(lightBytes.data());
        request.lightSize = lightBytes.size();
        request.familyData =
            reinterpret_cast<std::uintptr_t>(variant.familyBytes.data());
        request.familySize = variant.familyBytes.size();
        abi::RasterStatusV1 frameStatus{};
        frameStatus.structSize = sizeof(frameStatus);
        if (!host.RenderRasterFrame(request, frameStatus)) {
            std::cerr << "mirror-replay: submission failed diagnostic="
                      << frameStatus.diagnostic << (char)10;
            return 8;
        }
        validationErrors += frameStatus.validationErrorCount;
    }

    auto hostTraceHit = false;
    auto hostTraceDistance = 0.0f;
    // Trace the mirror ray on the host before reading anything from the
    // device. Three device-side attempts could not say whether the ray hits;
    // the contract's own tracer answers it directly, over the same triangles,
    // and it is the tool that found the family fixture's misses in one step.
    {
        const auto& withTarget = variants[1].scenePacket;
        const auto& withTargetSource = variants[1].source;
        std::vector<reflect::ReflectionTriangle> triangles;
        for (std::size_t object = 1; object < withTarget.objects.size();
            ++object) {
            const auto& record = withTarget.objects[object];
            const auto& draw = withTargetSource.draws[record.drawIndex];
            std::array<std::array<float, 3>, 3> corners{};
            for (std::size_t vertex = 0; vertex < 3; ++vertex) {
                const auto index = withTargetSource.indices[
                    draw.firstIndex + vertex];
                const auto& local =
                    withTargetSource.vertices[index].position;
                for (std::size_t row = 0; row < 3; ++row) {
                    corners[vertex][row] =
                        record.model[row * 4 + 0] * local[0] +
                        record.model[row * 4 + 1] * local[1] +
                        record.model[row * 4 + 2] * local[2] +
                        record.model[row * 4 + 3];
                }
            }
            reflect::ReflectionTriangle triangle{};
            triangle.a = corners[0];
            triangle.b = corners[1];
            triangle.c = corners[2];
            triangle.normal = {record.geometricNormal[0],
                record.geometricNormal[1], record.geometricNormal[2]};
            triangle.twoSided = true;
            triangles.push_back(triangle);
        }
        const auto& mirror = withTarget.objects[0];
        const std::array<float, 3> centre{mirror.model[3], mirror.model[7],
            mirror.model[11]};
        const auto toCamera = Normalize3({-centre[0], -centre[1], -centre[2]});
        const auto direction = reflect::MirrorDirection(toCamera,
            {mirror.geometricNormal[0], mirror.geometricNormal[1],
             mirror.geometricNormal[2]});
        reflect::ReflectionRay ray{};
        ray.origin = centre;
        ray.direction = direction;
        ray.maximumDistance = 1.0e4f;
        const auto hit = reflect::TraceReflection(triangles, ray);
        hostTraceHit = hit.hit;
        hostTraceDistance = hit.distance;
    }
    const auto mirrorId = variants[0].scenePacket.objects.front().objectId;
    std::uint64_t mirrorPixels = 0;
    std::uint64_t reflectedPixels = 0;
    std::uint64_t tracedDifferingPixels = 0;
    float maximumReflected = 0.0f;
    for (std::size_t index = 0; index < pixelCount; ++index) {
        const auto ownerId =
            static_cast<std::uint64_t>(
                variants[0].gbuffer[index].objectId[0]) |
            (static_cast<std::uint64_t>(
                variants[0].gbuffer[index].objectId[1]) << 32);
        if (ownerId != mirrorId) continue;
        ++mirrorPixels;
        auto delta = 0.0f;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            delta = std::max(delta, std::abs(
                variants[1].hdr[index][channel] -
                variants[0].hdr[index][channel]));
        }
        if (delta > 1.0e-2f) {
            ++reflectedPixels;
            maximumReflected = std::max(maximumReflected, delta);
        }
        // The traced path against the fallback, same scene either way. If
        // these agree the shader is not tracing at all, whatever the
        // geometry says.
        auto fallbackDelta = 0.0f;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            fallbackDelta = std::max(fallbackDelta, std::abs(
                variants[1].hdr[index][channel] -
                variants[2].hdr[index][channel]));
        }
        if (fallbackDelta > 1.0e-2f) ++tracedDifferingPixels;
    }

    // The mirror has to be on screen at all, or "nothing was reflected" and
    // "there was nowhere to reflect onto" are the same measurement.
    // The mirror's mean colour, reported because a branch that cannot be
    // told apart by a difference can be told apart by what it actually put
    // on the surface.
    std::array<double, 3> mirrorMean{};
    if (mirrorPixels != 0) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const auto ownerId =
                static_cast<std::uint64_t>(
                    variants[1].gbuffer[index].objectId[0]) |
                (static_cast<std::uint64_t>(
                    variants[1].gbuffer[index].objectId[1]) << 32);
            if (ownerId != mirrorId) continue;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                mirrorMean[channel] += variants[1].hdr[index][channel];
            }
        }
        for (auto& channel : mirrorMean) {
            channel /= static_cast<double>(mirrorPixels);
        }
    }

    const auto passed = mirrorPixels > 0 && reflectedPixels > 0 &&
        validationErrors == 0;
    std::cout << "mirror-replay extent=" << options.width << char{120}
              << options.height
              << " mirror-pixels=" << mirrorPixels
              << " reflected-pixels=" << reflectedPixels
              << " reflected-max=" << maximumReflected
              << " mirror-mean=" << mirrorMean[0] << "," << mirrorMean[1]
              << "," << mirrorMean[2]
              << " host-trace-hit=" << (hostTraceHit ? 1 : 0)
              << " host-trace-distance=" << hostTraceDistance
              << " traced-vs-fallback=" << tracedDifferingPixels
              << " validation-errors=" << validationErrors
              << " result=" << (passed ? "pass" : "fail") << (char)10;
    return passed ? 0 : 1;
}

int RenderFamilyScene(const FamilyRenderOptions& options)
{
    view::FramePacket frame;
    std::vector<std::byte> frameBytes;
    raster::DecodedPacket source;
    std::vector<std::byte> packetBytes;
    scene::ScenePacket scenePacket;
    std::vector<std::byte> sceneBytes;
    material::FamilyPacket familyPacket;
    std::vector<std::byte> familyBytes;
    const auto bundle = BuildFamilyMaterialFixture();
    std::vector<std::byte> bundleBytes;
    // Each step is reported separately, because "fixture construction
    // failed" names no cause and a fixture has several ways to fail.
    if (!BuildSceneFrame(options.width, options.height, false,
            frame, frameBytes)) {
        std::cerr << "family-replay: frame fixture failed\n";
        return 5;
    }
    if (!BuildSceneSource(options.width, options.height, source,
            packetBytes)) {
        std::cerr << "family-replay: raster fixture failed\n";
        return 5;
    }
    if (!BuildFamilySceneObjects(scenePacket, familyPacket)) {
        std::cerr << "family-replay: family fixture failed\n";
        return 5;
    }
    lighting::LightPacket lightPacket;
    if (options.lit && !BuildFixtureLights(scenePacket, lightPacket)) {
        std::cerr << "family-replay: light fixture failed\n";
        return 5;
    }
    // Appended after the shared fixture and before anything is encoded, so
    // phase 17's scene, families, and lights stay exactly what its artifacts
    // recorded, and the occluder still reaches every encoded packet.
    if (options.shadows && !AppendShadowFixture(
            source, scenePacket, familyPacket, lightPacket,
            {frame.views.front().cameraRelativeOrigin[0],
             frame.views.front().cameraRelativeOrigin[1],
             frame.views.front().cameraRelativeOrigin[2]},
            options.reflections)) {
        std::cerr << "family-replay: shadow fixture failed\n";
        return 5;
    }
    // After the shadow fixture, because both append objects and each takes
    // the next free draw index. Appending in the other order gives two
    // objects the same one, and the scene is refused for a duplicate draw.
    if (options.transparency && !AppendTransparencyFixture(
            source, scenePacket, familyPacket)) {
        std::cerr << "family-replay: transparency fixture failed\n";
        return 5;
    }
    if (options.shadows || options.transparency) {
        // The raster packet grew, so its bytes have to be re-encoded from the
        // appended source rather than the three-object one.
        packetBytes.clear();
        if (!raster::EncodePacket(source, packetBytes)) {
            std::cerr << "family-replay: appended raster encode failed\n";
            return 5;
        }
    }
    const auto sceneEncoded =
        scene::EncodeScenePacket(scenePacket, sceneBytes);
    if (sceneEncoded != scene::ScenePacketError::None) {
        std::cerr << "family-replay: scene encode failed error="
                  << scene::ToString(sceneEncoded) << '\n';
        return 5;
    }
    // The same scene with the transparent table removed, so a second device
    // render differs from the first in exactly one thing. Differencing two
    // device renders is what isolates the blend; differencing against the CPU
    // reference measured the pre-existing divergence between oracle and
    // device instead, because the reference does not composite at all.
    std::vector<std::byte> opaqueSceneBytes;
    // The same scene with the transparent draws listed in the opposite order.
    // The pass sorts them itself, so the composite must not depend on the
    // order they arrive in -- and if it does not sort, reversing the list
    // changes the image. That is the only property here that can observe the
    // sort, and it needs no knowledge of what the right answer looks like.
    // The same geometry with the opaque materials recoloured. The refractive
    // quad is opaque over its own area, so what is behind it can only reach
    // those pixels through the snapshot: if they move when this moves, the
    // shader is reading it, and if they do not, it is not.
    std::vector<std::byte> recolouredPacketBytes;
    if (options.transparency) {
        auto recoloured = source;
        for (auto& material : recoloured.materials) {
            // Only the geometry behind. Recolouring the blended quad's own
            // material changes its pixels through its own shading, and the
            // measurement then reports every pixel as reading the snapshot
            // whether or not the shader touches it.
            const auto blendedOwn = std::any_of(
                scenePacket.transparent.begin(),
                scenePacket.transparent.end(),
                [&](const scene::TransparentDrawRecordV1& record) {
                    return record.materialId == material.resourceId;
                });
            if (blendedOwn) continue;
            material.baseColor[0] = 1.0f - material.baseColor[0];
            material.baseColor[1] = 1.0f - material.baseColor[1];
            material.baseColor[2] = 1.0f - material.baseColor[2];
        }
        if (!raster::EncodePacket(recoloured, recolouredPacketBytes)) {
            std::cerr << "family-replay: recoloured raster encode failed"
                      << (char)10;
            return 5;
        }
    }
    // The scene with everything except the additive draws removed.
    // Differencing this against the baseline isolates additive blending
    // exactly, so its one-way "never darker" bound stays meaningful alongside
    // layers that legitimately darken and would otherwise break a property
    // they were never about: a refractive layer reads what is behind it, and
    // a multiply decal darkens by definition -- that is what the mode is for.
    // Excluding only the refractive one was enough until the fixture gained a
    // decal, which is exactly the kind of quiet coupling naming the wanted
    // mode avoids.
    std::vector<std::byte> additiveOnlySceneBytes;
    if (options.transparency) {
        auto additiveOnly = scenePacket;
        additiveOnly.transparent.erase(
            std::remove_if(additiveOnly.transparent.begin(),
                additiveOnly.transparent.end(),
                [](const scene::TransparentDrawRecordV1& record) {
                    return record.blend != static_cast<std::uint32_t>(
                        blend::BlendMode::Additive);
                }),
            additiveOnly.transparent.end());
        const auto additiveEncoded = scene::EncodeScenePacket(
            additiveOnly, additiveOnlySceneBytes);
        if (additiveEncoded != scene::ScenePacketError::None) {
            std::cerr << "family-replay: additive scene encode failed error="
                      << scene::ToString(additiveEncoded) << (char)10;
            return 5;
        }
    }
    // The decal alone, and the same decal with its volume removed. The pair is
    // what separates a projection from a blended card: the first shows what
    // the decal covers, the second shows the quad it was evaluated over, and
    // the difference between them is everything the projection clipped away.
    // Either one on its own only shows that something was drawn.
    std::vector<std::byte> decalOnlySceneBytes;
    std::vector<std::byte> decalUnclippedSceneBytes;
    std::size_t decalDrawCount = 0;
    if (options.transparency) {
        auto decalOnly = scenePacket;
        decalOnly.transparent.erase(
            std::remove_if(decalOnly.transparent.begin(),
                decalOnly.transparent.end(),
                [](const scene::TransparentDrawRecordV1& record) {
                    return record.domain != static_cast<std::uint32_t>(
                        blend::EffectDomain::Decal);
                }),
            decalOnly.transparent.end());
        auto decalUnclipped = decalOnly;
        for (auto& record : decalUnclipped.transparent) {
            // A range of zero is the record's own way of saying this draw
            // projects nothing, so the quad composites whole.
            record.decalRange = 0.0f;
            record.decalRadius = 0.0f;
        }
        decalDrawCount = decalOnly.transparent.size();
        if (scene::EncodeScenePacket(decalOnly, decalOnlySceneBytes) !=
                scene::ScenePacketError::None ||
            scene::EncodeScenePacket(decalUnclipped,
                decalUnclippedSceneBytes) != scene::ScenePacketError::None) {
            std::cerr << "family-replay: decal scene encode failed" << (char)10;
            return 5;
        }
    }
    std::vector<std::byte> reorderedSceneBytes;
    if (options.transparency) {
        auto reordered = scenePacket;
        std::reverse(reordered.transparent.begin(),
            reordered.transparent.end());
        const auto reorderedEncoded =
            scene::EncodeScenePacket(reordered, reorderedSceneBytes);
        if (reorderedEncoded != scene::ScenePacketError::None) {
            std::cerr << "family-replay: reordered scene encode failed error="
                      << scene::ToString(reorderedEncoded) << (char)10;
            return 5;
        }
    }
    if (options.transparency) {
        auto withoutBlended = scenePacket;
        withoutBlended.transparent.clear();
        const auto opaqueEncoded =
            scene::EncodeScenePacket(withoutBlended, opaqueSceneBytes);
        if (opaqueEncoded != scene::ScenePacketError::None) {
            std::cerr << "family-replay: baseline scene encode failed error="
                      << scene::ToString(opaqueEncoded) << '\n';
            return 5;
        }
    }
    const auto familyEncoded =
        material::EncodeFamilyPacket(familyPacket, familyBytes);
    if (familyEncoded != material::FamilyPacketError::None) {
        std::cerr << "family-replay: family encode failed error="
                  << material::ToString(familyEncoded) << '\n';
        return 5;
    }
    const auto bundleEncoded =
        material::EncodeMaterialReplayBundle(bundle, bundleBytes);
    if (bundleEncoded != material::MaterialPacketError::None) {
        std::cerr << "family-replay: material bundle encode failed error="
                  << material::ToString(bundleEncoded) << '\n';
        return 5;
    }
    std::vector<std::byte> lightBytes;
    std::vector<lighting::GpuLightRecordV1> gpuLights;
    lighting::GpuEnvironmentV1 gpuEnvironment{};
    if (options.lit) {
        const auto lightEncoded =
            lighting::EncodeLightPacket(lightPacket, lightBytes);
        if (lightEncoded != lighting::LightPacketError::None) {
            std::cerr << "family-replay: light encode failed error="
                      << lighting::ToString(lightEncoded) << '\n';
            return 5;
        }
        // The reference evaluates the same GPU records the backend uploads,
        // narrowed against the same camera origin, so the two sides cannot
        // disagree about where a light is.
        const std::array<double, 3> cameraOrigin{
            frame.views.front().cameraRelativeOrigin[0],
            frame.views.front().cameraRelativeOrigin[1],
            frame.views.front().cameraRelativeOrigin[2]};
        for (const auto& record : lightPacket.lights) {
            lighting::GpuLightRecordV1 gpu{};
            if (lighting::BuildGpuLight(record, cameraOrigin, gpu) !=
                lighting::LightError::None) {
                std::cerr << "family-replay: light narrowing failed\n";
                return 5;
            }
            gpuLights.push_back(gpu);
        }
        gpuEnvironment = lighting::BuildGpuEnvironment(
            lightPacket.environment,
            static_cast<std::uint32_t>(gpuLights.size()));
    }

    abi::AdapterLuid luid{};
    if (!QueryDefaultAdapterLuid(luid)) {
        std::cerr << "family-replay: D3D adapter query failed\n";
        return 5;
    }
    WindowsBackendModule module{options.backend};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    const auto loaded = host.Load(module, callbacks);
    if (!loaded || !host.RasterAvailable()) {
        std::cerr << "family-replay: backend load/API failed host="
                  << static_cast<unsigned>(loaded.error) << '\n';
        return 6;
    }
    abi::RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.flags = abi::RasterCreateAnyAdapter |
        (options.validation ? abi::RasterCreateValidation : 0u);
    create.adapterLuid = luid;
    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    if (!host.CreateRaster(create, status)) {
        std::cerr << "family-replay: create failed diagnostic="
                  << status.diagnostic << '\n';
        return 7;
    }

    SceneSubmission rendered;
    scene::GBufferImage expected;
    scene::HdrImage expectedHdr;
    raster::DecodedPacket projected;
    std::vector<std::array<float, 3>> vertexPositions;
    scene::ReferenceInputs inputs{};
    inputs.baseColor = &bundle.textures[0];
    inputs.normalMap = &bundle.textures[1];
    inputs.families = &familyPacket;
    if (options.lit) {
        inputs.lights = gpuLights;
        inputs.environment = &gpuEnvironment;
    }
    // The unshadowed reference is kept so the fixture can prove the shadow
    // term actually changed something. Without it a scene that occludes
    // nothing would compare clean and report success while measuring nothing,
    // which is exactly how the phase 16 normal check passed for a while.
    scene::HdrImage unshadowedHdr;
    scene::HdrImage unreflectedHdr;
    scene::HdrImage unindirectHdr;
    std::vector<accel::ShadowTriangle> occluders;
    std::vector<reflect::ReflectionTriangle> reflectionGeometry;
    std::uint32_t reflectionProbeRays = 0;
    std::uint32_t reflectionProbeHits = 0;
    const auto referenceBuilt =
        scene::ProjectScenePacket(source, frame.views.front(), scenePacket,
            projected, &vertexPositions) ==
            scene::ScenePacketError::None &&
        (inputs.vertexPositions = vertexPositions, true) &&
        [&] {
            if (!options.lit) return true;
            // Neither term, so the reflection can be isolated the same way the
            // shadow is: three references, each adding one term.
            scene::GBufferImage plain;
            if (scene::RenderReferenceGBuffer(projected, scenePacket, inputs,
                    plain, &unreflectedHdr) !=
                scene::ScenePacketError::None) {
                return false;
            }
            // The unshadowed pass carries the same reflections as the full
            // one, so the difference between them is the shadow term alone.
            // Without that the mask marks every pixel either term touched
            // and the interior comparison stops isolating the boundary it
            // was written to exclude.
            reflectionGeometry = BuildReflectionGeometry(
                projected, vertexPositions, scenePacket, familyPacket);
            inputs.reflectionGeometry = reflectionGeometry;
            // Probe the fixture with the contract's own tracer before
            // trusting it. A reflective surface whose mirror direction finds
            // nothing makes the traced branch and the miss branch return the
            // same value, and every mutation of the traced path then reports
            // a false pass -- which is exactly what this contract did.
            for (const auto& probe : scenePacket.objects) {
                if (probe.roughness > 0.2f) continue;
                const std::array<float, 3> centre{probe.model[3],
                    probe.model[7], probe.model[11]};
                const std::array<float, 3> toCamera{-centre[0], -centre[1],
                    -centre[2]};
                const std::array<float, 3> normal{probe.geometricNormal[0],
                    probe.geometricNormal[1], probe.geometricNormal[2]};
                const auto mirror = reflect::MirrorDirection(
                    Normalize3(toCamera), normal);
                reflect::ReflectionRay ray{};
                ray.origin = centre;
                ray.direction = mirror;
                ray.maximumDistance = 1.0e4f;
                const auto hit =
                    reflect::TraceReflection(reflectionGeometry, ray);
                reflectionProbeRays++;
                if (hit.hit) ++reflectionProbeHits;

            }
            // The same ray count the shader traces, walking the same
            // sequence, so the two integrate one set of directions rather
            // than two different estimates of the same integral.
            inputs.indirectPreset.raysPerPixel = 8;
            // Reflections on, indirect off: differencing against this
            // isolates the bounce term, exactly as the unshadowed pass
            // isolates the shadow term. Without it the two ray-traced terms
            // are measured together and either could be doing nothing.
            inputs.indirectEnabled = false;
            scene::GBufferImage direct;
            if (scene::RenderReferenceGBuffer(projected, scenePacket, inputs,
                    direct, &unindirectHdr) !=
                scene::ScenePacketError::None) {
                return false;
            }
            // The shader traces indirect for every lit surface whenever ray
            // query is available, so the reference does too. Enabling it only
            // for the indirect fixture would leave every earlier lit fixture
            // comparing a bounced frame against an unbounced reference.
            inputs.indirectEnabled = true;
            scene::GBufferImage unshadowed;
            if (scene::RenderReferenceGBuffer(projected, scenePacket, inputs,
                    unshadowed, &unshadowedHdr) !=
                scene::ScenePacketError::None) {
                return false;
            }
            std::vector<std::uint32_t> blendedDraws;
            for (const auto& record : scenePacket.transparent) {
                if (record.objectIndex < scenePacket.objects.size()) {
                    blendedDraws.push_back(
                        scenePacket.objects[record.objectIndex].drawIndex);
                }
            }
            occluders = BuildOccluders(projected, vertexPositions,
                blendedDraws);
            inputs.occluders = occluders;
            return true;
        }() &&
        scene::RenderReferenceGBuffer(projected, scenePacket, inputs,
            expected, &expectedHdr) == scene::ScenePacketError::None;
    if (!referenceBuilt) {
        std::cerr << "family-replay: reference render failed\n";
    }

    const auto pixelCount =
        static_cast<std::size_t>(options.width) * options.height;
    std::vector<std::array<float, 4>> renderedHdr;
    // The same frame without the transparent table, rendered by the same
    // device, so the two differ in exactly the blended layer.
    std::vector<std::array<float, 4>> baselineHdr;
    auto baselineRendered = false;
    // The same frame with the transparent table listed backwards.
    std::vector<std::array<float, 4>> reorderedHdr;
    auto reorderedRendered = false;
    // The same frame with only the additive layer.
    std::vector<std::array<float, 4>> additiveOnlyHdr;
    // The additive layer rendered alone. The refractive layer is opaque in
    // alpha and would otherwise dominate every maximum taken over the frame,
    // which hides whether the additive rule ran at all.
    std::vector<scene::GBufferPixelV1> additiveGbuffer;
    // The decal projecting, and the same draw with its volume removed.
    auto decalOnlyRendered = false;
    std::vector<std::array<float, 4>> decalOnlyHdr;
    std::vector<std::array<float, 4>> decalUnclippedHdr;
    // The reactive plane from each. A discarded fragment writes no attachment
    // at all, so this counts exactly the pixels the projection rejected --
    // where a colour comparison cannot, because the decal object is excluded
    // from the opaque pass and a rejected pixel differs from the baseline for
    // that reason rather than for this one.
    std::vector<scene::GBufferPixelV1> decalOnlyGbuffer;
    std::vector<scene::GBufferPixelV1> decalUnclippedGbuffer;
    // The same frame with bloom armed, so the kernel can be differenced
    // against the identical frame without it.
    auto bloomRendered = false;
    std::vector<raster::Rgba8> bloomPixels;
    auto additiveOnlyRendered = false;
    // The same frame with the geometry behind the refractive quad recoloured.
    std::vector<std::array<float, 4>> recolouredHdr;
    auto recolouredRendered = false;
    // The baseline render's surface data. The composited render deliberately
    // leaves blended geometry out of the G-buffer, so the oracle-parity
    // comparisons have to read the baseline's, exactly as they read its HDR.
    std::vector<scene::GBufferPixelV1> baselineGbuffer;
    auto submitted = false;
    if (referenceBuilt) {
        try {
            rendered.gbuffer.resize(pixelCount);
            renderedHdr.resize(pixelCount);
            rendered.image.width = options.width;
            rendered.image.height = options.height;
            rendered.image.pixels.resize(pixelCount);
        } catch (...) {
            std::cerr << "family-replay: allocation failed\n";
            return 5;
        }
        abi::RasterFrameRequestV1 request{};
        request.structSize = sizeof(request);
        request.packetData = reinterpret_cast<std::uintptr_t>(
            packetBytes.data());
        request.packetSize = packetBytes.size();
        request.outputData = reinterpret_cast<std::uintptr_t>(
            rendered.image.pixels.data());
        request.outputRowPitch = options.width * sizeof(raster::Rgba8);
        request.outputCapacity =
            static_cast<std::uint64_t>(request.outputRowPitch) *
            options.height;
        request.frameData = reinterpret_cast<std::uintptr_t>(
            frameBytes.data());
        request.frameSize = frameBytes.size();
        request.sceneData = reinterpret_cast<std::uintptr_t>(
            sceneBytes.data());
        request.sceneSize = sceneBytes.size();
        request.gbufferData = reinterpret_cast<std::uintptr_t>(
            rendered.gbuffer.data());
        request.gbufferCapacity =
            rendered.gbuffer.size() * sizeof(scene::GBufferPixelV1);
        // The normal map has to arrive through the material bundle, because
        // that is the only path that binds more than the base texture.
        request.materialData = reinterpret_cast<std::uintptr_t>(
            bundleBytes.data());
        request.materialSize = bundleBytes.size();
        request.familyData = reinterpret_cast<std::uintptr_t>(
            familyBytes.data());
        request.familySize = familyBytes.size();
        // Emission is compared at its authored magnitude, which an 8-bit
        // tone-mapped output would clamp away.
        request.hdrData = reinterpret_cast<std::uintptr_t>(
            renderedHdr.data());
        request.hdrCapacity = renderedHdr.size() * sizeof(renderedHdr[0]);
        if (options.lit) {
            request.lightData = reinterpret_cast<std::uintptr_t>(
                lightBytes.data());
            request.lightSize = lightBytes.size();
        }
        rendered.status.structSize = sizeof(rendered.status);
        submitted = static_cast<bool>(
            host.RenderRasterFrame(request, rendered.status));
        if (!submitted) {
            std::cerr << "family-replay: submission failed diagnostic="
                      << rendered.status.diagnostic << '\n';
        }
        // The same frame again with the transparent table removed. Everything
        // else -- geometry, lights, materials, ray-traced terms -- is bit for
        // bit what the first render used, so the difference between the two
        // is the blended layer and nothing else.
        if (submitted && options.transparency) {
            baselineHdr.assign(renderedHdr.size(),
                std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            std::vector<raster::Rgba8> baselineImage(
                rendered.image.pixels.size());
            baselineGbuffer.assign(rendered.gbuffer.size(),
                scene::GBufferPixelV1{});
            auto baselineRequest = request;
            baselineRequest.sceneData = reinterpret_cast<std::uintptr_t>(
                opaqueSceneBytes.data());
            baselineRequest.sceneSize = opaqueSceneBytes.size();
            baselineRequest.outputData = reinterpret_cast<std::uintptr_t>(
                baselineImage.data());
            baselineRequest.gbufferData = reinterpret_cast<std::uintptr_t>(
                baselineGbuffer.data());
            baselineRequest.hdrData = reinterpret_cast<std::uintptr_t>(
                baselineHdr.data());
            abi::RasterStatusV1 baselineStatus{};
            baselineStatus.structSize = sizeof(baselineStatus);
            baselineRendered = static_cast<bool>(
                host.RenderRasterFrame(baselineRequest, baselineStatus));
            if (!baselineRendered) {
                std::cerr << "family-replay: baseline submission failed"
                             " diagnostic=" << baselineStatus.diagnostic
                          << '\n';
            }
            // Once with the geometry behind recoloured, so the refraction can
            // be told from a refractive draw that merely runs.
            recolouredHdr.assign(renderedHdr.size(),
                std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            std::vector<scene::GBufferPixelV1> recolouredGbuffer(
                rendered.gbuffer.size());
            std::vector<raster::Rgba8> recolouredImage(
                rendered.image.pixels.size());
            auto recolouredRequest = request;
            recolouredRequest.packetData = reinterpret_cast<std::uintptr_t>(
                recolouredPacketBytes.data());
            recolouredRequest.packetSize = recolouredPacketBytes.size();
            recolouredRequest.gbufferData = reinterpret_cast<std::uintptr_t>(
                recolouredGbuffer.data());
            recolouredRequest.outputData = reinterpret_cast<std::uintptr_t>(
                recolouredImage.data());
            recolouredRequest.hdrData = reinterpret_cast<std::uintptr_t>(
                recolouredHdr.data());
            abi::RasterStatusV1 recolouredStatus{};
            recolouredStatus.structSize = sizeof(recolouredStatus);
            recolouredRendered = static_cast<bool>(host.RenderRasterFrame(
                recolouredRequest, recolouredStatus));
            if (!recolouredRendered) {
                std::cerr << "family-replay: recoloured submission failed"
                             " diagnostic=" << recolouredStatus.diagnostic
                          << (char)10;
            }
            // Once with only the additive layer, so the never-darker bound
            // measures additive blending and nothing else.
            additiveOnlyHdr.assign(renderedHdr.size(),
                std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            additiveGbuffer.assign(rendered.gbuffer.size(),
                scene::GBufferPixelV1{});
            std::vector<raster::Rgba8> additiveImage(
                rendered.image.pixels.size());
            auto additiveRequest = baselineRequest;
            additiveRequest.gbufferData = reinterpret_cast<std::uintptr_t>(
                additiveGbuffer.data());
            additiveRequest.outputData = reinterpret_cast<std::uintptr_t>(
                additiveImage.data());
            additiveRequest.sceneData = reinterpret_cast<std::uintptr_t>(
                additiveOnlySceneBytes.data());
            additiveRequest.sceneSize = additiveOnlySceneBytes.size();
            additiveRequest.hdrData = reinterpret_cast<std::uintptr_t>(
                additiveOnlyHdr.data());
            abi::RasterStatusV1 additiveStatus{};
            additiveStatus.structSize = sizeof(additiveStatus);
            additiveOnlyRendered = static_cast<bool>(host.RenderRasterFrame(
                additiveRequest, additiveStatus));
            if (!additiveOnlyRendered) {
                std::cerr << "family-replay: additive submission failed"
                             " diagnostic=" << additiveStatus.diagnostic
                          << (char)10;
            }
            // And once with bloom armed. The same scene and the same
            // exposure, differing only in the post flag, so the two outputs
            // can be differenced and the difference attributed to the kernel.
            {
                bloomPixels.assign(rendered.image.pixels.size(),
                    raster::Rgba8{});
                std::vector<scene::GBufferPixelV1> bloomGbuffer(
                    rendered.gbuffer.size());
                std::vector<std::array<float, 4>> bloomHdr(
                    renderedHdr.size(), std::array<float, 4>{});
                // The frame as submitted, not the baseline: bloom is compared
                // against the composited image the oracle was given.
                auto bloomRequest = request;
                bloomRequest.flags |= abi::RasterFrameBloom;
                // Stronger than the contract's default tuning, and stated
                // here rather than there. At the default intensity the peak
                // difference between the smoothstep knee and a linear ramp is
                // half a per cent of radiance, which is below one code in an
                // eight-bit comparison -- so the curve's shape would be
                // unobservable and a linear ramp would pass. The oracle reads
                // the same numbers, so this changes what can be seen rather
                // than what is being asserted.
                bloomRequest.bloomThreshold = kReplayBloomRules.threshold;
                bloomRequest.bloomKnee = kReplayBloomRules.knee;
                bloomRequest.bloomIntensity = kReplayBloomRules.intensity;
                bloomRequest.outputData = reinterpret_cast<std::uintptr_t>(
                    bloomPixels.data());
                bloomRequest.gbufferData = reinterpret_cast<std::uintptr_t>(
                    bloomGbuffer.data());
                bloomRequest.hdrData = reinterpret_cast<std::uintptr_t>(
                    bloomHdr.data());
                abi::RasterStatusV1 bloomStatus{};
                bloomStatus.structSize = sizeof(bloomStatus);
                bloomRendered = static_cast<bool>(
                    host.RenderRasterFrame(bloomRequest, bloomStatus));
                if (!bloomRendered) {
                    std::cerr << "family-replay: bloom submission failed"
                                 " diagnostic=" << bloomStatus.diagnostic
                              << (char)10;
                }
            }

            // The decal twice: once projecting, once with its volume removed.
            // Both against the same baseline, so the difference between them
            // is exactly what the projection clipped.
            const auto renderDecal = [&](
                const std::vector<std::byte>& sceneBytesForDecal,
                std::vector<std::array<float, 4>>& hdrOut,
                std::vector<scene::GBufferPixelV1>& decalGbuffer) {
                hdrOut.assign(renderedHdr.size(),
                    std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
                decalGbuffer.assign(rendered.gbuffer.size(),
                    scene::GBufferPixelV1{});
                std::vector<raster::Rgba8> decalImage(
                    rendered.image.pixels.size());
                auto decalRequest = baselineRequest;
                decalRequest.gbufferData =
                    reinterpret_cast<std::uintptr_t>(decalGbuffer.data());
                decalRequest.outputData =
                    reinterpret_cast<std::uintptr_t>(decalImage.data());
                decalRequest.sceneData = reinterpret_cast<std::uintptr_t>(
                    sceneBytesForDecal.data());
                decalRequest.sceneSize = sceneBytesForDecal.size();
                decalRequest.hdrData =
                    reinterpret_cast<std::uintptr_t>(hdrOut.data());
                abi::RasterStatusV1 decalStatus{};
                decalStatus.structSize = sizeof(decalStatus);
                const auto ok = static_cast<bool>(
                    host.RenderRasterFrame(decalRequest, decalStatus));
                if (!ok) {
                    std::cerr << "family-replay: decal submission failed"
                                 " diagnostic=" << decalStatus.diagnostic
                              << (char)10;
                }
                return ok;
            };
            decalOnlyRendered =
                renderDecal(decalOnlySceneBytes, decalOnlyHdr,
                    decalOnlyGbuffer) &&
                renderDecal(decalUnclippedSceneBytes, decalUnclippedHdr,
                    decalUnclippedGbuffer);

            // And once more with the table listed backwards. The pass sorts
            // the draws itself, so this must come out pixel for pixel
            // identical to the first render; if it takes them in capture
            // order instead, it will not.
            reorderedHdr.assign(renderedHdr.size(),
                std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            // Its own G-buffer and colour target. Inheriting the baseline's
            // pointers would have this render overwrite the surface data the
            // oracle-parity comparisons read, and they would then be
            // comparing the oracle against a third scene entirely.
            std::vector<scene::GBufferPixelV1> reorderedGbuffer(
                rendered.gbuffer.size());
            std::vector<raster::Rgba8> reorderedImage(
                rendered.image.pixels.size());
            auto reorderedRequest = baselineRequest;
            reorderedRequest.gbufferData = reinterpret_cast<std::uintptr_t>(
                reorderedGbuffer.data());
            reorderedRequest.outputData = reinterpret_cast<std::uintptr_t>(
                reorderedImage.data());
            reorderedRequest.sceneData = reinterpret_cast<std::uintptr_t>(
                reorderedSceneBytes.data());
            reorderedRequest.sceneSize = reorderedSceneBytes.size();
            reorderedRequest.hdrData = reinterpret_cast<std::uintptr_t>(
                reorderedHdr.data());
            abi::RasterStatusV1 reorderedStatus{};
            reorderedStatus.structSize = sizeof(reorderedStatus);
            reorderedRendered = static_cast<bool>(host.RenderRasterFrame(
                reorderedRequest, reorderedStatus));
            if (!reorderedRendered) {
                std::cerr << "family-replay: reordered submission failed"
                             " diagnostic=" << reorderedStatus.diagnostic
                          << '\n';
            }
        }
    }

    scene::GBufferComparison comparison{};
    // The oracle-parity checks compare against a reference that carries no
    // transparency, so with a blended layer present they must read the
    // baseline render. The blend itself is checked separately, by the
    // device-to-device difference; comparing the composited frame here would
    // report the layer as an oracle disagreement.
    const auto& parityHdr =
        (options.transparency && baselineRendered) ? baselineHdr : renderedHdr;
    const auto& parityGbuffer =
        (options.transparency && baselineRendered)
            ? baselineGbuffer : rendered.gbuffer;

    InteriorComparison interior{};
    std::uint64_t tintPixels = 0;
    std::uint64_t expectedTintPixels = 0;
    std::uint64_t emissivePixels = 0;
    std::uint64_t expectedEmissivePixels = 0;
    float maximumHdrError = 0.0f;
    std::uint64_t differingHdrPixels = 0;
    auto normalsDiffer = false;
    auto lobeDiffers = false;
    if (submitted) {
        comparison = scene::CompareGBuffer(expected.pixels, parityGbuffer);
        interior = CompareInteriorPixels(expected, parityGbuffer, 1.0e-3f);
        const auto tintedId = scenePacket.objects[0].objectId;
        const auto emissiveId = scenePacket.objects[2].objectId;
        tintPixels = CountObjectPixels(parityGbuffer, tintedId);
        expectedTintPixels = CountObjectPixels(expected.pixels, tintedId);

        // Emission only exists in the float colour target, so it is checked
        // there rather than in the G-buffer.
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const auto& want = expectedHdr.pixels[index];
            const auto& got = parityHdr[index];
            auto pixelError = 0.0f;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                pixelError = std::max(pixelError,
                    std::abs(want[channel] - got[channel]));
            }
            maximumHdrError = std::max(maximumHdrError, pixelError);
            if (pixelError > 1.0e-2f) ++differingHdrPixels;
            const auto objectId =
                static_cast<std::uint64_t>(
                    parityGbuffer[index].objectId[0]) |
                (static_cast<std::uint64_t>(
                    parityGbuffer[index].objectId[1]) << 32);
            if (objectId == emissiveId && got[0] > 1.0f) ++emissivePixels;
            const auto expectedObjectId =
                static_cast<std::uint64_t>(
                    expected.pixels[index].objectId[0]) |
                (static_cast<std::uint64_t>(
                    expected.pixels[index].objectId[1]) << 32);
            if (expectedObjectId == emissiveId && want[0] > 1.0f) {
                ++expectedEmissivePixels;
            }
        }

        // Comparing two objects' normals would measure their geometry, not
        // their encoding, because the fixture's objects face different ways.
        // The check instead predicts both decodes for the *same* object and
        // asserts the render took the declared one, which is what makes
        // "model-space normals never pass through the tangent path" testable
        // rather than asserted.
        const auto shadingOf = [&](const std::uint64_t objectId,
                                   std::array<float, 3>& normal) {
            for (std::size_t index = 0; index < pixelCount; ++index) {
                const auto id = static_cast<std::uint64_t>(
                        parityGbuffer[index].objectId[0]) |
                    (static_cast<std::uint64_t>(
                        parityGbuffer[index].objectId[1]) << 32);
                if (id != objectId) continue;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    normal[axis] =
                        parityGbuffer[index].shadingNormalDepth[axis];
                }
                return true;
            }
            return false;
        };
        const auto channel = [](const std::uint8_t value) {
            return static_cast<float>(value) / 255.0f * 2.0f - 1.0f;
        };
        const std::array<float, 3> geometric{
            scenePacket.objects[0].geometricNormal[0],
            scenePacket.objects[0].geometricNormal[1],
            scenePacket.objects[0].geometricNormal[2]};
        auto modelPrediction = Normalize3({channel(kFamilyNormalTexel[0]),
            channel(kFamilyNormalTexel[1]), channel(kFamilyNormalTexel[2])});
        const auto tangentX = channel(kFamilyNormalTexel[0]);
        const auto tangentY = channel(kFamilyNormalTexel[1]);
        const auto tangentZ = std::sqrt(std::max(0.0f,
            1.0f - (tangentX * tangentX + tangentY * tangentY)));
        const auto sign = geometric[2] >= 0.0f ? 1.0f : -1.0f;
        const auto basisA = -1.0f / (sign + geometric[2]);
        const auto basisC = geometric[0] * geometric[1] * basisA;
        const std::array<float, 3> basisT{
            1.0f + sign * geometric[0] * geometric[0] * basisA,
            sign * basisC, -sign * geometric[0]};
        const std::array<float, 3> basisB{basisC,
            sign + geometric[1] * geometric[1] * basisA, -geometric[1]};
        auto tangentPrediction = Normalize3({
            basisT[0] * tangentX + basisB[0] * tangentY +
                geometric[0] * tangentZ,
            basisT[1] * tangentX + basisB[1] * tangentY +
                geometric[1] * tangentZ,
            basisT[2] * tangentX + basisB[2] * tangentY +
                geometric[2] * tangentZ});
        std::array<float, 3> observed{};
        if (shadingOf(tintedId, observed)) {
            auto modelDelta = 0.0f;
            auto tangentDelta = 0.0f;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                modelDelta = std::max(modelDelta,
                    std::abs(observed[axis] - modelPrediction[axis]));
                tangentDelta = std::max(tangentDelta,
                    std::abs(observed[axis] - tangentPrediction[axis]));
            }
            // The declared decode must match, the other must not, and the
            // two must be far enough apart for that to mean something.
            normalsDiffer = modelDelta < 1.0e-2f && tangentDelta > 5.0e-2f;
        }

        // An anisotropic hair lobe is narrower than the object's own
        // roughness, so the class must have changed the stored value.
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const auto id = static_cast<std::uint64_t>(
                    parityGbuffer[index].objectId[0]) |
                (static_cast<std::uint64_t>(
                    parityGbuffer[index].objectId[1]) << 32);
            if (id != tintedId) continue;
            lobeDiffers = std::abs(
                parityGbuffer[index].geometricNormalRoughness[3] -
                scenePacket.objects[0].roughness) > 1.0e-3f;
            break;
        }
    }

    auto wrote = submitted && WritePpm(options.output, rendered.image);
    if (!options.sceneOutput.empty()) {
        wrote = wrote && WriteBinaryFile(options.sceneOutput, sceneBytes);
    }
    if (!options.familyOutput.empty()) {
        wrote = wrote && WriteBinaryFile(options.familyOutput, familyBytes);
    }
    if (!options.lightOutput.empty()) {
        wrote = wrote && WriteBinaryFile(options.lightOutput, lightBytes);
    }
    if (!options.gbufferOutput.empty()) {
        wrote = wrote && WriteBinaryFile(options.gbufferOutput,
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(rendered.gbuffer.data()),
                rendered.gbuffer.size() * sizeof(scene::GBufferPixelV1)});
    }

    // Lighting must actually change the frame. Without this the comparison
    // would pass just as well against a renderer that ignored every light,
    // because agreeing on "unlit" is still agreeing.
    auto litPixels = std::uint64_t{0};
    if (submitted && options.lit) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const auto& want = expectedHdr.pixels[index];
            const auto& albedo = expected.pixels[index].albedo;
            if (std::abs(want[0] - albedo[0]) > 1.0e-3f ||
                std::abs(want[1] - albedo[1]) > 1.0e-3f ||
                std::abs(want[2] - albedo[2]) > 1.0e-3f) {
                ++litPixels;
            }
        }
    }

    // Pixels the shadow term actually darkened, measured against the
    // unshadowed reference. Zero of them would mean the comparison below
    // proves only that two unshadowed images agree.
    std::uint64_t shadowedPixels = 0;
    float maximumShadowDelta = 0.0f;
    if (options.lit && expectedHdr.width == unshadowedHdr.width &&
        expectedHdr.height == unshadowedHdr.height) {
        for (std::size_t index = 0; index < expectedHdr.pixels.size();
             ++index) {
            const auto& shadowed = expectedHdr.pixels[index];
            const auto& open = unshadowedHdr.pixels[index];
            auto delta = 0.0f;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                delta = std::max(delta,
                    std::abs(open[channel] - shadowed[channel]));
            }
            if (delta > 1.0e-3f) {
                ++shadowedPixels;
                maximumShadowDelta = std::max(maximumShadowDelta, delta);
            }
        }
    }

    // Pixels the reflection term actually changed, measured against a
    // reference carrying neither ray-traced term. Zero of them would mean
    // the comparison proves only that two unreflected images agree.
    std::uint64_t reflectedPixels = 0;
    float maximumReflectionDelta = 0.0f;
    if (options.lit && unreflectedHdr.width == unshadowedHdr.width &&
        unreflectedHdr.height == unshadowedHdr.height) {
        for (std::size_t index = 0; index < unreflectedHdr.pixels.size();
             ++index) {
            auto delta = 0.0f;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                delta = std::max(delta,
                    std::abs(unreflectedHdr.pixels[index][channel] -
                        unshadowedHdr.pixels[index][channel]));
            }
            if (delta > 1.0e-3f) {
                ++reflectedPixels;
                maximumReflectionDelta =
                    std::max(maximumReflectionDelta, delta);
            }
        }
    }

    // The additive layer against a reference that carries no transparency at
    // all. Additive can only ever brighten what is behind it, so a single
    // pixel that came out darker than the reference means the pass is not
    // compositing additively -- a blend factor mixed up, or the layer drawn
    // into the wrong attachment. The bound is one-way on purpose: how much
    // brighter depends on the effect, but never-darker is exact.
    std::uint64_t transparentBrighterPixels = 0;
    std::uint64_t transparentDarkerPixels = 0;
    float maximumTransparentDrop = 0.0f;
    // The colour balance the composite ends up with, summed over every pixel
    // the blended layers changed. The near layer is straight alpha and blue,
    // the far one additive and orange: composited in the contract's order the
    // blue layer covers the orange glow, and composited in the reverse order
    // the orange is added last and unmasked. The sign of this sum is what
    // tells the two apart, and it is the only thing in this contract that
    // can observe the sort at all.
    double transparentBlueBias = 0.0;
    // Against the same device rendering the same frame without the table, so
    // the only difference is the blended layer. Additive can only brighten
    // what is behind it, which makes this bound exact rather than
    // approximate: one darker pixel means the blend factors are wrong, the
    // layer landed in the wrong attachment, or it replaced instead of added.
    if (options.transparency && baselineRendered && additiveOnlyRendered) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const std::array<float, 3> want{baselineHdr[index][0],
                baselineHdr[index][1], baselineHdr[index][2]};
            const auto& got = additiveOnlyHdr[index];
            auto brighter = false;
            auto darker = false;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto drop = want[channel] - got[channel];
                if (drop > 1.0e-2f) {
                    darker = true;
                    maximumTransparentDrop =
                        std::max(maximumTransparentDrop, drop);
                }
                if (got[channel] - want[channel] > 1.0e-2f) brighter = true;
            }
            if (darker) ++transparentDarkerPixels;
            if (brighter) ++transparentBrighterPixels;
            if (brighter || darker) {
                transparentBlueBias +=
                    static_cast<double>(got[2] - want[2]) -
                    static_cast<double>(got[0] - want[0]);
            }
        }
    }

    // Pixels of the composited G-buffer carrying a blended object's identity.
    // The blended pass writes only the colour target, so this must be zero:
    // a particle in the G-buffer makes the reflection and indirect passes
    // treat it as an opaque surface and every ray behind it stops there.
    // Read from the composited render deliberately -- the parity comparisons
    // read the baseline, so nothing else looks at these pixels at all.
    std::uint64_t blendedGbufferPixels = 0;
    if (options.transparency && submitted) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const auto objectId =
                static_cast<std::uint64_t>(
                    rendered.gbuffer[index].objectId[0]) |
                (static_cast<std::uint64_t>(
                    rendered.gbuffer[index].objectId[1]) << 32);
            for (const auto& record : scenePacket.transparent) {
                if (record.objectIndex >= scenePacket.objects.size()) continue;
                if (scenePacket.objects[record.objectIndex].objectId ==
                    objectId) {
                    ++blendedGbufferPixels;
                    break;
                }
            }
        }
    }

    // How much of the decal's own geometry its projection kept. The fixture's
    // decal is evaluated over a quad and given a radius a third of it, so a
    // projection that works covers part of that quad and leaves the rest --
    // and one that does nothing covers all of it. Counted against the same
    // quad rendered without a volume, because "some pixels changed" is what a
    // decal drawn as an ordinary blended card would also produce.
    std::uint64_t decalCoveredPixels = 0;
    std::uint64_t decalClippedPixels = 0;
    std::uint64_t decalShadedPixels = 0;
    if (options.transparency && submitted && baselineRendered &&
        decalOnlyRendered) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            auto decalDrew = false;
            auto unclipped = false;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                if (std::abs(decalOnlyHdr[index][channel] -
                        baselineHdr[index][channel]) > 1.0e-4f) {
                    decalDrew = true;
                }
                // Against the projecting render rather than the baseline. The
                // decal object is excluded from the opaque pass once the
                // transparent table claims it, so "drawn blended" and
                // "clipped away entirely" both differ from a baseline that
                // drew it opaque -- a comparison against the baseline cannot
                // tell a projection from a discard.
                if (std::abs(decalUnclippedHdr[index][channel] -
                        decalOnlyHdr[index][channel]) > 1.0e-4f) {
                    unclipped = true;
                }
            }
            if (decalDrew) ++decalCoveredPixels;
            if (unclipped) ++decalShadedPixels;
            // Covered by the quad and rejected by the projection. The reactive
            // plane is written by any fragment that survives and by no
            // fragment that discards, so this is exactly what the three
            // rejection rules removed -- and unlike a colour comparison it
            // cannot be produced by the falloff merely dimming a pixel.
            if (decalUnclippedGbuffer[index].reactive > 0.0f &&
                !(decalOnlyGbuffer[index].reactive > 0.0f)) {
                ++decalClippedPixels;
            }
        }
    }

    // The reactive mask the transparent pass wrote. A transparent draw is the
    // one thing that must not appear in the shading planes and must appear
    // here: an upscaler that does not know a pixel was decided by a particle
    // reconstructs the particle from history that never contained it, which is
    // the ghost trailing every spark and muzzle flash.
    //
    // Counted where the composite actually changed the frame, because that is
    // where a transparent draw covered the pixel; the identity planes still
    // name the opaque surface behind it, by design.
    std::uint64_t reactivePixels = 0;
    // Pixels the composite never touched that carry a mask anyway, which
    // means the opaque pass claimed them.
    std::uint64_t reactiveBelowOpaque = 0;
    // The strongest claim any transparent draw made. The fixture's additive
    // draw is brighter than its alpha, so this separates a mask that weighs
    // radiance from one that returns the alpha and calls it done.
    float reactiveMaximum = 0.0f;
    // Pixels whose mask came from the dim layer's alpha rather than from any
    // radiance. The bright layer cannot produce one and neither can a rule
    // that has dropped the alpha term.
    std::uint64_t reactiveAlphaBand = 0;
    if (options.transparency && submitted && baselineRendered) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            auto changed = false;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                if (std::abs(renderedHdr[index][channel] -
                        baselineHdr[index][channel]) > 1.0e-4f) {
                    changed = true;
                }
            }
            const auto reactive = rendered.gbuffer[index].reactive;
            if (!changed) {
                // Opaque geometry decides none of the pixel. A mask that
                // marked it would tell an upscaler to distrust the stable
                // part of the frame, which is the part it exists to reuse.
                if (reactive != 0.0f) ++reactiveBelowOpaque;
                continue;
            }
            if (reactive > 0.0f) ++reactivePixels;
        }
    }
    // The additive layer measured alone, because the refractive layer is
    // opaque in alpha and dominates any maximum taken over the whole frame.
    // The additive fixture is dim in alpha and bright in radiance, so this is
    // the number that says whether the mask weighs radiance or just returns
    // the alpha -- and the composite cannot tell them apart, because additive
    // blending never reads the alpha at all.
    if (options.transparency && additiveOnlyRendered &&
        additiveGbuffer.size() == pixelCount) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const auto reactive = additiveGbuffer[index].reactive;
            reactiveMaximum = std::max(reactiveMaximum, reactive);
            // The dim layer's own alpha, which is the other half of the
            // maximum. Its radiance is near black, so a mask that returned
            // the radiance alone would leave these pixels near zero and a
            // mask that returned the alpha alone would leave the bright
            // layer's pixels at a fifth. Only a rule that takes both puts
            // pixels in each band at once.
            if (reactive > 0.45f && reactive < 0.75f) ++reactiveAlphaBand;
        }
    }

    // Pixels where listing the transparent draws backwards changed the
    // composite. The pass sorts them itself, so this must be zero; any other
    // number means the image depends on capture order rather than on depth,
    // and which layer ends up on top would then vary between runs of the
    // same scene.
    // Pixels the refractive layer changed, against the same frame carrying
    // only the additive one. A refractive surface reads what is behind it, so
    // this must be non-zero: a shader that ignored the snapshot would leave
    // the two identical and the refraction would be doing nothing at all.
    std::uint64_t refractedPixels = 0;
    if (options.transparency && additiveOnlyRendered) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                if (std::abs(renderedHdr[index][channel] -
                        additiveOnlyHdr[index][channel]) > 1.0e-3f) {
                    ++refractedPixels;
                    break;
                }
            }
        }
    }

    // Of the pixels the refractive layer covers, how many moved when the
    // geometry behind them was recoloured. The refractive quad is opaque over
    // its own area, so the only route from that geometry to those pixels is
    // the snapshot: a shader that ignores it leaves them identical, which is
    // exactly the mutation the plain "the refractive draw changed something"
    // count could not tell apart.
    std::uint64_t refractionReadsBehind = 0;
    if (options.transparency && additiveOnlyRendered && recolouredRendered) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            auto covered = false;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                if (std::abs(renderedHdr[index][channel] -
                        additiveOnlyHdr[index][channel]) > 1.0e-3f) {
                    covered = true;
                    break;
                }
            }
            if (!covered) continue;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                if (std::abs(renderedHdr[index][channel] -
                        recolouredHdr[index][channel]) > 1.0e-3f) {
                    ++refractionReadsBehind;
                    break;
                }
            }
        }
    }

    // The device's tone-mapped image against the post contract's own curve and
    // transfer function, applied on the host to the same HDR pixels the
    // device read. This is the whole output stage compared against its oracle:
    // the curve, the exposure that scales it, and the sRGB encoding.
    std::uint64_t toneDifferingPixels = 0;
    std::uint32_t maximumToneCode = 0;
    // The same frame with bloom armed. Compared against the same oracle with
    // the bloom rule applied, and counted for how many pixels it actually
    // changed: a kernel that ran and did nothing would match the oracle
    // perfectly if the oracle also did nothing.
    std::uint64_t bloomDifferingPixels = 0;
    std::uint32_t maximumBloomCode = 0;
    std::uint64_t bloomChangedPixels = 0;
    if (submitted && rendered.image.pixels.size() == pixelCount) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const std::array<float, 3> hdr{renderedHdr[index][0],
                renderedHdr[index][1], renderedHdr[index][2]};
            const auto mapped = post::ToneMap(hdr, 1.0f);
            const auto encoded = post::ApplyOutputTransform(
                mapped, post::OutputFormat::Srgb8);
            const auto& got = rendered.image.pixels[index];
            const std::array<std::uint8_t, 3> want{
                static_cast<std::uint8_t>(std::lround(
                    std::clamp(encoded[0], 0.0f, 1.0f) * 255.0f)),
                static_cast<std::uint8_t>(std::lround(
                    std::clamp(encoded[1], 0.0f, 1.0f) * 255.0f)),
                static_cast<std::uint8_t>(std::lround(
                    std::clamp(encoded[2], 0.0f, 1.0f) * 255.0f))};
            const std::array<std::uint8_t, 3> have{got.r, got.g, got.b};
            auto differs = false;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto delta = want[channel] > have[channel]
                    ? want[channel] - have[channel]
                    : have[channel] - want[channel];
                // One code of slack: the device encodes in half precision
                // and rounds in fixed function, so an exact match would be
                // asserting bit-identical arithmetic across two different
                // machines rather than asserting the same curve.
                if (delta > 1) differs = true;
                maximumToneCode = std::max<std::uint32_t>(
                    maximumToneCode, delta);
            }
            if (differs) ++toneDifferingPixels;
        }
    }

    // The same frame with bloom armed, against the same oracle carrying the
    // bloom rule. Two things are asserted together and neither is sufficient
    // alone: that the device matches the oracle, and that the oracle's own
    // answer differs from the unbloomed one somewhere. A kernel that ran and
    // returned its input would satisfy the first and fail the second.
    if (submitted && bloomRendered &&
        bloomPixels.size() == pixelCount) {
        const auto& bloomRules = kReplayBloomRules;
        for (std::size_t index = 0; index < pixelCount; ++index) {
            std::array<float, 3> hdr{renderedHdr[index][0],
                renderedHdr[index][1], renderedHdr[index][2]};
            // Thresholded before exposure and the curve, because the highlight
            // that blooms is a property of the scene rather than of the
            // display decision. Applying it after would make the bloom level
            // move with the exposure.
            const auto weight = post::BloomWeight(bloomRules,
                post::Luminance(hdr));
            const auto gain = weight * bloomRules.intensity;
            for (auto& channel : hdr) channel += channel * gain;
            const auto mapped = post::ToneMap(hdr, 1.0f);
            const auto encoded = post::ApplyOutputTransform(
                mapped, post::OutputFormat::Srgb8);
            const auto& got = bloomPixels[index];
            const std::array<std::uint8_t, 3> want{
                static_cast<std::uint8_t>(std::lround(
                    std::clamp(encoded[0], 0.0f, 1.0f) * 255.0f)),
                static_cast<std::uint8_t>(std::lround(
                    std::clamp(encoded[1], 0.0f, 1.0f) * 255.0f)),
                static_cast<std::uint8_t>(std::lround(
                    std::clamp(encoded[2], 0.0f, 1.0f) * 255.0f))};
            const std::array<std::uint8_t, 3> have{got.r, got.g, got.b};
            auto differs = false;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto delta = want[channel] > have[channel]
                    ? want[channel] - have[channel]
                    : have[channel] - want[channel];
                if (delta > 1) differs = true;
                maximumBloomCode = std::max<std::uint32_t>(
                    maximumBloomCode, delta);
            }
            if (differs) ++bloomDifferingPixels;
            const auto& plain = rendered.image.pixels[index];
            if (got.r != plain.r || got.g != plain.g || got.b != plain.b) {
                ++bloomChangedPixels;
            }
        }
    }

    std::uint64_t reorderDifferingPixels = 0;
    if (options.transparency && reorderedRendered) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                if (std::abs(renderedHdr[index][channel] -
                        reorderedHdr[index][channel]) > 1.0e-3f) {
                    ++reorderDifferingPixels;
                    break;
                }
            }
        }
    }

    // Pixels the bounce term changed, against a reference carrying every
    // other ray-traced term but that one.
    std::uint64_t indirectPixels = 0;
    float maximumIndirectDelta = 0.0f;
    if (options.indirect && unindirectHdr.width == unshadowedHdr.width &&
        unindirectHdr.height == unshadowedHdr.height) {
        for (std::size_t index = 0; index < unindirectHdr.pixels.size();
             ++index) {
            auto delta = 0.0f;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                delta = std::max(delta,
                    std::abs(unindirectHdr.pixels[index][channel] -
                        unshadowedHdr.pixels[index][channel]));
            }
            if (delta > 1.0e-3f) {
                ++indirectPixels;
                maximumIndirectDelta = std::max(maximumIndirectDelta, delta);
            }
        }
    }

    // A shadow boundary is a silhouette. The oracle resolves it by tracing
    // from a pixel centre the reference rasterizer chose, the ray query
    // resolves it from the centre hardware rasterization chose, and the two
    // disagree along the edge for the same reason object silhouettes do.
    // Every pixel whose whole 3x3 neighbourhood is on one side of the
    // boundary and belongs to one object must agree exactly; the boundary
    // itself is counted and reported rather than silently tolerated.
    std::uint64_t shadowInterior = 0;
    std::uint64_t shadowInteriorMismatches = 0;
    float maximumShadowInteriorError = 0.0f;
    std::uint32_t worstX = 0;
    std::uint32_t worstY = 0;
    auto worstShadowed = false;
    auto worstWant = 0.0f;
    auto worstGot = 0.0f;
    if (options.shadows && shadowedPixels > 0) {
        std::vector<std::uint8_t> shadowMask(pixelCount, 0);
        for (std::size_t index = 0; index < pixelCount; ++index) {
            auto delta = 0.0f;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                delta = std::max(delta,
                    std::abs(unshadowedHdr.pixels[index][channel] -
                        expectedHdr.pixels[index][channel]));
            }
            shadowMask[index] = delta > 1.0e-3f ? 1u : 0u;
        }
        // Both images, not just the rendered one: a pixel the oracle assigns
        // to a different object is an object silhouette, and comparing
        // shading across it measures coverage rather than shadowing.
        const auto identityAt = [&](const std::size_t index) {
            const auto rasterized = static_cast<std::uint64_t>(
                parityGbuffer[index].objectId[0]) |
                (static_cast<std::uint64_t>(
                    parityGbuffer[index].objectId[1]) << 32);
            const auto oracle = static_cast<std::uint64_t>(
                expected.pixels[index].objectId[0]) |
                (static_cast<std::uint64_t>(
                    expected.pixels[index].objectId[1]) << 32);
            return std::pair<std::uint64_t, std::uint64_t>{rasterized, oracle};
        };
        for (std::uint32_t y = 2; y + 2 < options.height; ++y) {
            for (std::uint32_t x = 2; x + 2 < options.width; ++x) {
                const auto centre =
                    static_cast<std::size_t>(y) * options.width + x;
                auto uniform = true;
                for (int dy = -2; dy <= 2 && uniform; ++dy) {
                    for (int dx = -2; dx <= 2 && uniform; ++dx) {
                        const auto neighbour = centre +
                            static_cast<std::size_t>(dy) * options.width + dx;
                        uniform = shadowMask[neighbour] == shadowMask[centre] &&
                            identityAt(neighbour) == identityAt(centre);
                    }
                }
                if (!uniform) continue;
                ++shadowInterior;
                // Mixed absolute and relative. These are unbounded HDR values,
                // so a fixed epsilon silently becomes a tighter relative
                // bound the brighter the light: at radiance 6.6 the two
                // sides agree to 0.1%% and a flat 1e-2 still calls it a
                // failure. The shadow term itself is all-or-nothing, so it
                // cannot hide inside this band.
                auto error = 0.0f;
                auto allowed = 1.0e-2f;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const auto want = expectedHdr.pixels[centre][channel];
                    error = std::max(error,
                        std::abs(want - parityHdr[centre][channel]));
                    allowed = std::max(allowed,
                        1.0e-2f + 1.0e-3f * std::abs(want));
                }
                if (error > maximumShadowInteriorError) {
                    maximumShadowInteriorError = error;
                    worstX = x;
                    worstY = y;
                    worstShadowed = shadowMask[centre] != 0;
                    worstWant = expectedHdr.pixels[centre][0];
                    worstGot = parityHdr[centre][0];
                }
                if (error > allowed) ++shadowInteriorMismatches;
            }
        }
    }

    const auto passed = submitted && wrote &&
        comparison.identityMismatches == 0 &&
        interior.mismatchedPixels == 0 &&
        (!options.shadows ||
            (shadowInterior > 0 &&
                shadowInteriorMismatches <= shadowInterior / 1000)) &&
        tintPixels == expectedTintPixels && tintPixels > 0 &&
        emissivePixels == expectedEmissivePixels && emissivePixels > 0 &&
        // Indirect light is stochastic, so the bound is on how many pixels
        // may disagree rather than on how far one may move. A lit frame
        // without ray-traced terms would still be held to the absolute
        // bound, but there is no longer such a frame: the shader traces
        // indirect for every lit surface the device can trace for.
        (!options.lit || differingHdrPixels <= pixelCount / 100) &&
        (options.lit || maximumHdrError <= 1.0e-2f) &&
        normalsDiffer && lobeDiffers &&
        (!options.lit || litPixels > pixelCount / 8) &&
        // The shadow fixture must contain a real occlusion, or the GPU shadow
        // term is compared against a reference that never shadows anything.
        (!options.shadows || shadowedPixels > 0) &&
        // Every pixel the transparent composite changed must carry a reactive
        // mask, and none may drop below the opaque claim already there. The
        // first says the mask is produced at all; the second says the blended
        // pass combines with it rather than overwriting it, which is the
        // difference between a spark marking its pixel and a dim puff drawn
        // afterwards erasing the mark.
        (!options.transparency || reactivePixels > 0) &&
        // No pixel the composite left alone may carry a mask: opaque
        // geometry is the stable part of the frame and an upscaler reuses it.
        reactiveBelowOpaque == 0 &&
        // The bright layer carries an alpha of a fifth and a radiance near one.
        // The threshold sits above the dim layer's alpha of three fifths on
        // purpose: a mask that returned the alpha for every additive draw
        // would still reach three fifths through that layer, and a bound
        // below it would call that a pass.
        (!options.transparency || reactiveMaximum > 0.9f) &&
        // And the other half of the maximum: the dim layer reports its alpha,
        // which no radiance in that draw could have produced.
        (!options.transparency || reactiveAlphaBand > 0) &&
        // The decal has to draw at all, and its volume has to change what it
        // covers. Without the second, a decal composited as an ordinary
        // blended card passes: it draws, it is in the right layer, and the
        // projection it is supposed to be doing is never exercised.
        (!options.transparency || decalCoveredPixels > 0) &&
        (!options.transparency || decalClippedPixels > 0) &&
        // Indirect light is a stochastic estimator, and a ray-triangle hit
        // decision at an edge is not bit-identical between this oracle and
        // the hardware intersector. With eight rays a single flipped ray
        // moves a pixel by an eighth of the radiance behind it, so the bound
        // is on how many pixels may disagree rather than on how far one may
        // move -- the same rule the phase 11 silhouette comparison uses, and
        // for the same reason. Without indirect the interior must still
        // agree exactly.
        (!options.lit ||
            shadowInteriorMismatches <= shadowInterior / 1000) &&
        // The reflection fixture must contain a visible reflection, or the
        // GPU term is compared against a reference that reflects nothing.
        (!options.reflections || reflectedPixels > 0) &&

        // The indirect fixture must contain a visible bounce, or the GPU
        // term is compared against a reference that bounces nothing.
        (!options.indirect || indirectPixels > 0) &&
        // Additive can only brighten what is behind it, measured against the
        // same device rendering the same frame without the table -- so this
        // bound is exact rather than approximate. A single darker pixel
        // means the blend factors are wrong, the layer landed in the wrong
        // attachment, or it replaced instead of added. The layer must also
        // have composited something: a pass that draws nothing satisfies
        // "never darker" trivially, and a coplanar draw rejected by a strict
        // depth test does exactly that while reporting no error at all.
        (!options.transparency ||
            (baselineRendered && transparentDarkerPixels == 0 &&
                transparentBrighterPixels > 0)) &&
        // Sorted, not taken in capture order.
        (!options.transparency ||
            (reorderedRendered && reorderDifferingPixels == 0)) &&
        // And the blended layer stayed out of the surface data.
        (!options.transparency || blendedGbufferPixels == 0) &&
        // The output stage matches its oracle.
        toneDifferingPixels == 0 &&
        // Bloom must match the oracle, and on the fixture that carries a
        // highlight above the threshold it must also change something. The
        // first alone passes for a kernel that returns its input, because the
        // oracle would then be compared against a frame it did not alter
        // either.
        bloomDifferingPixels == 0 &&
        (!options.transparency || bloomChangedPixels > 0) &&
        // The refractive layer ran, and it read what was behind it.
        (!options.transparency ||
            (additiveOnlyRendered && refractedPixels > 0)) &&
        // Most of the covered area, not merely some of it. A shader that
        // ignores the snapshot still moves the pixels at the quad's edges,
        // where partial coverage lets the hardware blend leak the
        // destination through -- measured, that is 13 per cent of the area
        // against 71 per cent when the snapshot is read, so a bound at half
        // separates them with room on both sides.
        (!options.transparency ||
            (recolouredRendered &&
                refractionReadsBehind * 2 > refractedPixels)) &&
        rendered.status.validationErrorCount == 0;
    std::cout << "family-replay extent=" << options.width << char{120}
              << options.height
              << " families=" << familyPacket.records.size()
              << " lights=" << lightPacket.lights.size()
              << " lit-pixels=" << litPixels
              << " shadowed-pixels=" << shadowedPixels
              << " shadow-max-delta=" << maximumShadowDelta
              << " reflected-pixels=" << reflectedPixels
              << " reflection-max-delta=" << maximumReflectionDelta
              << " reflection-probe-rays=" << reflectionProbeRays
              << " reflection-probe-hits=" << reflectionProbeHits
              << " indirect-pixels=" << indirectPixels
              << " indirect-max-delta=" << maximumIndirectDelta
              << " tint-pixels=" << tintPixels
              << " expected-tint-pixels=" << expectedTintPixels
              << " emissive-pixels=" << emissivePixels
              << " expected-emissive-pixels=" << expectedEmissivePixels
              << " transparent-brighter=" << transparentBrighterPixels
              << " transparent-darker=" << transparentDarkerPixels
              << " transparent-max-drop=" << maximumTransparentDrop
              << " transparent-reorder-differing=" << reorderDifferingPixels
              << " transparent-reactive=" << reactivePixels
              << " transparent-reactive-on-opaque=" << reactiveBelowOpaque
              << " transparent-reactive-max=" << reactiveMaximum
              << " transparent-reactive-alpha-band=" << reactiveAlphaBand
              << " decal-covered=" << decalCoveredPixels
              << " decal-clipped=" << decalClippedPixels
              << " decal-shaded=" << decalShadedPixels
              << " decal-draws=" << decalDrawCount
              << " transparent-gbuffer-pixels=" << blendedGbufferPixels
              << " transparent-refracted=" << refractedPixels
              << " transparent-reads-behind=" << refractionReadsBehind
              << " bloom-differing=" << bloomDifferingPixels
              << " bloom-max-code=" << maximumBloomCode
              << " bloom-changed=" << bloomChangedPixels
              << " tone-differing=" << toneDifferingPixels
              << " tone-max-code=" << maximumToneCode
              << " hdr-max-error=" << maximumHdrError
              << " hdr-differing=" << differingHdrPixels
              << " shadow-interior=" << shadowInterior
              << " shadow-interior-mismatches=" << shadowInteriorMismatches
              << " shadow-interior-max-error="
              << maximumShadowInteriorError
              << " worst=" << worstX << "," << worstY
              << " worst-shadowed=" << (worstShadowed ? "yes" : "no")
              << " worst-want=" << worstWant
              << " worst-got=" << worstGot
              << " normal-encodings-differ=" << (normalsDiffer ? "yes" : "no")
              << " lobe-differs=" << (lobeDiffers ? "yes" : "no")
              << " gbuffer-identity-mismatches="
              << comparison.identityMismatches
              << " gbuffer-max-error=" << comparison.maximumAbsoluteError
              << " interior=" << interior.interiorPixels
              << " interior-mismatches=" << interior.mismatchedPixels
              << " validation-errors="
              << rendered.status.validationErrorCount
              << " output=" << options.output.string()
              << " result=" << (passed ? "pass" : "fail") << '\n';
    static_cast<void>(host.DestroyRaster(status));
    return passed ? 0 : 1;
}

int RenderAlphaScene(const AlphaRenderOptions& options)
{
    view::FramePacket frame;
    std::vector<std::byte> frameBytes;
    raster::DecodedPacket source;
    std::vector<std::byte> packetBytes;
    scene::ScenePacket scenePacket;
    std::vector<std::byte> sceneBytes;
    const auto cutout = BuildAlphaCutoutTexture();
    std::vector<std::byte> cutoutBytes;
    if (!BuildSceneFrame(options.width, options.height, false,
            frame, frameBytes) ||
        !BuildSceneSource(options.width, options.height, source,
            packetBytes) ||
        !BuildAlphaSceneObjects(scenePacket) ||
        scene::EncodeScenePacket(scenePacket, sceneBytes) !=
            scene::ScenePacketError::None ||
        texture::EncodeCapturedTexture(cutout, cutoutBytes) !=
            texture::TexturePacketError::None) {
        std::cerr << "alpha-replay: fixture construction failed\n";
        return 5;
    }

    abi::AdapterLuid luid{};
    if (!QueryDefaultAdapterLuid(luid)) {
        std::cerr << "alpha-replay: D3D adapter query failed\n";
        return 5;
    }
    WindowsBackendModule module{options.backend};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    const auto loaded = host.Load(module, callbacks);
    if (!loaded || !host.RasterAvailable()) {
        std::cerr << "alpha-replay: backend load/API failed host="
                  << static_cast<unsigned>(loaded.error) << '\n';
        return 6;
    }
    abi::RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.flags = abi::RasterCreateAnyAdapter |
        (options.validation ? abi::RasterCreateValidation : 0u);
    create.adapterLuid = luid;
    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    if (!host.CreateRaster(create, status)) {
        std::cerr << "alpha-replay: create failed diagnostic="
                  << status.diagnostic << '\n';
        return 7;
    }

    SceneSubmission rendered;
    scene::GBufferImage expected;
    raster::DecodedPacket projected;
    const auto referenceBuilt =
        scene::ProjectScenePacket(source, frame.views.front(), scenePacket,
            projected) == scene::ScenePacketError::None &&
        scene::RenderReferenceGBuffer(projected, scenePacket, &cutout,
            expected) == scene::ScenePacketError::None;
    if (!referenceBuilt) {
        std::cerr << "alpha-replay: reference render failed\n";
    }

    const auto pixelCount =
        static_cast<std::size_t>(options.width) * options.height;
    auto submitted = false;
    if (referenceBuilt) {
        try {
            rendered.gbuffer.resize(pixelCount);
            rendered.image.width = options.width;
            rendered.image.height = options.height;
            rendered.image.pixels.resize(pixelCount);
        } catch (...) {
            std::cerr << "alpha-replay: allocation failed\n";
            return 5;
        }
        abi::RasterFrameRequestV1 request{};
        request.structSize = sizeof(request);
        request.packetData = reinterpret_cast<std::uintptr_t>(
            packetBytes.data());
        request.packetSize = packetBytes.size();
        request.outputData = reinterpret_cast<std::uintptr_t>(
            rendered.image.pixels.data());
        request.outputRowPitch = options.width * sizeof(raster::Rgba8);
        request.outputCapacity =
            static_cast<std::uint64_t>(request.outputRowPitch) *
            options.height;
        request.frameData = reinterpret_cast<std::uintptr_t>(
            frameBytes.data());
        request.frameSize = frameBytes.size();
        request.sceneData = reinterpret_cast<std::uintptr_t>(
            sceneBytes.data());
        request.sceneSize = sceneBytes.size();
        request.gbufferData = reinterpret_cast<std::uintptr_t>(
            rendered.gbuffer.data());
        request.gbufferCapacity =
            rendered.gbuffer.size() * sizeof(scene::GBufferPixelV1);
        // Alpha is sampled from the base-colour texture, so the cutout has
        // to reach the shader through the captured-texture slot.
        request.textureData = reinterpret_cast<std::uintptr_t>(
            cutoutBytes.data());
        request.textureSize = cutoutBytes.size();
        rendered.status.structSize = sizeof(rendered.status);
        submitted = static_cast<bool>(
            host.RenderRasterFrame(request, rendered.status));
        if (!submitted) {
            std::cerr << "alpha-replay: submission failed diagnostic="
                      << rendered.status.diagnostic << '\n';
        }
    }

    scene::GBufferComparison comparison{};
    InteriorComparison interior{};
    std::uint64_t cutoutPixels = 0;
    std::uint64_t expectedCutoutPixels = 0;
    if (submitted) {
        comparison = scene::CompareGBuffer(expected.pixels, rendered.gbuffer);
        interior = CompareInteriorPixels(expected, rendered.gbuffer, 1.0e-3f);
        cutoutPixels = CountObjectPixels(rendered.gbuffer,
            scenePacket.objects[0].objectId);
        expectedCutoutPixels = CountObjectPixels(expected.pixels,
            scenePacket.objects[0].objectId);
    }

    // A bare "interior mismatches" count cannot say which channel disagrees,
    // which is the only thing that identifies the cause.
    if (submitted && interior.mismatchedPixels != 0) {
        std::size_t reported = 0;
        for (std::uint32_t y = 1; y + 1 < expected.height && reported < 2;
             ++y) {
            for (std::uint32_t x = 1; x + 1 < expected.width && reported < 2;
                 ++x) {
                const auto& want = expected.At(x, y);
                const auto& got = rendered.gbuffer[
                    static_cast<std::size_t>(y) * expected.width + x];
                const float* wantGroups[]{want.albedo,
                    want.geometricNormalRoughness, want.shadingNormalDepth};
                const float* gotGroups[]{got.albedo,
                    got.geometricNormalRoughness, got.shadingNormalDepth};
                auto differs = false;
                for (std::size_t group = 0; group < 3 && !differs; ++group) {
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        if (std::abs(wantGroups[group][channel] -
                                gotGroups[group][channel]) > 1.0e-3f) {
                            differs = true;
                            break;
                        }
                    }
                }
                if (!differs) continue;
                static const char* kNames[]{
                    "albedo", "geometricNormalRoughness",
                    "shadingNormalDepth"};
                std::cerr << "  mismatch at " << x << ',' << y << " object="
                          << (static_cast<std::uint64_t>(want.objectId[0]) |
                              (static_cast<std::uint64_t>(want.objectId[1])
                                  << 32))
                          << '\n';
                for (std::size_t group = 0; group < 3; ++group) {
                    std::cerr << "    " << kNames[group] << " expected";
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        std::cerr << ' ' << wantGroups[group][channel];
                    }
                    std::cerr << " actual";
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        std::cerr << ' ' << gotGroups[group][channel];
                    }
                    std::cerr << '\n';
                }
                ++reported;
            }
        }
    }

    auto wrote = submitted && WritePpm(options.output, rendered.image);
    if (!options.sceneOutput.empty()) {
        wrote = wrote && WriteBinaryFile(options.sceneOutput, sceneBytes);
    }
    if (!options.gbufferOutput.empty()) {
        wrote = wrote && WriteGBuffer(options.gbufferOutput,
            rendered.gbuffer);
    }
    if (!wrote) std::cerr << "alpha-replay: artifact write failed\n";

    status = {};
    status.structSize = sizeof(status);
    const auto destroyed = host.DestroyRaster(status);
    const auto validationErrors = status.validationErrorCount;
    const auto shutdown = host.RequestShutdown();
    const auto lifecyclePass = destroyed && validationErrors == 0 &&
        shutdown.error == BackendHostError::ShutdownDeferred;

    // The cutout must remove coverage without erasing the object, and the
    // depth prepass must not punch holes: a silhouette disagreement between
    // the prepass and the colour pass fails the EQUAL test and shows up here
    // as missing interior pixels.
    const auto passed = submitted && wrote && lifecyclePass &&
        interior.mismatchedPixels == 0 &&
        comparison.identityMismatches == 0 &&
        cutoutPixels > 0 && cutoutPixels == expectedCutoutPixels;
    std::cout << "alpha-replay extent=" << options.width << 'x'
              << options.height
              << " cutout-pixels=" << cutoutPixels
              << " expected-cutout-pixels=" << expectedCutoutPixels
              << " gbuffer-identity-mismatches="
              << comparison.identityMismatches
              << " gbuffer-max-error=" << comparison.maximumAbsoluteError
              << " gbuffer-mean-error=" << comparison.meanAbsoluteError
              << " interior=" << interior.interiorPixels
              << " interior-mismatches=" << interior.mismatchedPixels
              << " validation-errors=" << validationErrors
              << " output=" << options.output.string()
              << " result=" << (passed ? "pass" : "fail") << '\n';
    return passed ? 0 : 1;
}

bool ParseTerrainRenderOptions(
    const int argc,
    const char* const* argv,
    const int firstOption,
    TerrainRenderOptions& options)
{
    for (int index = firstOption; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--backend" && index + 1 < argc) {
            options.backend = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else if (argument == "--terrain-output" && index + 1 < argc) {
            options.terrainOutput = argv[++index];
        } else if (argument == "--gbuffer-output" && index + 1 < argc) {
            options.gbufferOutput = argv[++index];
        } else if (argument == "--width" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.width)) return false;
        } else if (argument == "--height" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.height)) return false;
        } else if (argument == "--validation") {
            options.validation = true;
        } else {
            return false;
        }
    }
    return !options.backend.empty() && !options.output.empty() &&
        options.width >= 32 && options.height >= 32 &&
        options.width <= raster::kMaximumExtent &&
        options.height <= raster::kMaximumExtent;
}

struct TerrainSubmission
{
    SceneSubmission submission;
    scene::GBufferImage expected;
    scene::GBufferComparison comparison{};
    InteriorComparison interior{};
    bool valid{};
};

bool SubmitTerrainFrame(
    BackendHost& host,
    const std::vector<std::byte>& packetBytes,
    const std::vector<std::byte>& frameBytes,
    const std::vector<std::byte>& sceneBytes,
    const std::vector<std::byte>& terrainBytes,
    const std::vector<std::byte>& layerTextureBytes,
    const std::uint32_t width,
    const std::uint32_t height,
    TerrainSubmission& rendered)
{
    // The oracle image is filled before submission, so only the submission
    // side is reset here.
    rendered.submission = {};
    const auto pixelCount = static_cast<std::size_t>(width) * height;
    try {
        rendered.submission.gbuffer.assign(pixelCount,
            scene::GBufferPixelV1{});
        rendered.submission.image.width = width;
        rendered.submission.image.height = height;
        rendered.submission.image.pixels.assign(pixelCount, raster::Rgba8{});
    } catch (...) {
        return false;
    }
    abi::RasterFrameRequestV1 request{};
    request.structSize = sizeof(request);
    request.packetData = reinterpret_cast<std::uintptr_t>(packetBytes.data());
    request.packetSize = packetBytes.size();
    request.outputData = reinterpret_cast<std::uintptr_t>(
        rendered.submission.image.pixels.data());
    request.outputRowPitch = width * sizeof(raster::Rgba8);
    request.outputCapacity =
        static_cast<std::uint64_t>(request.outputRowPitch) * height;
    request.frameData = reinterpret_cast<std::uintptr_t>(frameBytes.data());
    request.frameSize = frameBytes.size();
    request.sceneData = reinterpret_cast<std::uintptr_t>(sceneBytes.data());
    request.sceneSize = sceneBytes.size();
    request.gbufferData = reinterpret_cast<std::uintptr_t>(
        rendered.submission.gbuffer.data());
    request.gbufferCapacity = rendered.submission.gbuffer.size() *
        sizeof(scene::GBufferPixelV1);
    // A terrain frame carries its landscape layer array in the captured
    // texture slot.
    request.textureData = reinterpret_cast<std::uintptr_t>(
        layerTextureBytes.data());
    request.textureSize = layerTextureBytes.size();
    request.terrainData = reinterpret_cast<std::uintptr_t>(
        terrainBytes.data());
    request.terrainSize = terrainBytes.size();
    rendered.submission.status = {};
    rendered.submission.status.structSize =
        sizeof(rendered.submission.status);
    const auto result = host.RenderRasterFrame(
        request, rendered.submission.status);
    rendered.submission.submitted = static_cast<bool>(result);
    return rendered.submission.submitted;
}

int RenderTerrainScene(const TerrainRenderOptions& options)
{
    view::FramePacket frame;
    std::vector<std::byte> frameBytes;
    raster::DecodedPacket source;
    std::vector<std::byte> packetBytes;
    scene::ScenePacket scenePacket;
    std::vector<std::byte> sceneBytes;
    const auto terrainPacket = BuildTerrainFixture();
    std::vector<std::byte> terrainBytes;
    const auto layerTexture = BuildTerrainLayerArray();
    std::vector<std::byte> layerTextureBytes;
    const auto terrainEncode = terrain::EncodeTerrainPacket(
        terrainPacket, terrainBytes);
    const auto layerEncode = texture::EncodeCapturedTexture(
        layerTexture, layerTextureBytes);
    if (!BuildSceneFrame(options.width, options.height, false,
            frame, frameBytes, true, kTerrainFarPlane, kTerrainNearPlane) ||
        !BuildSceneSource(options.width, options.height, source,
            packetBytes) ||
        !BuildTerrainSceneObjects(scenePacket) ||
        scene::EncodeScenePacket(scenePacket, sceneBytes) !=
            scene::ScenePacketError::None ||
        terrainEncode != terrain::TerrainError::None ||
        layerEncode != texture::TexturePacketError::None) {
        std::cerr << "terrain-replay: fixture construction failed"
                  << " terrain=" << terrain::ToString(terrainEncode)
                  << " layers=" << texture::ToString(layerEncode)
                  << " frame=" << (frameBytes.empty() ? "empty" : "ok")
                  << " packet=" << (packetBytes.empty() ? "empty" : "ok")
                  << " scene=" << (sceneBytes.empty() ? "empty" : "ok")
                  << '\n';
        return 5;
    }

    // The captured terrain has to survive its own contract before any of it
    // reaches the GPU.
    terrain::TerrainEvaluation evaluation{};
    if (terrain::EvaluateTerrain(terrainPacket, evaluation) !=
        terrain::TerrainError::None) {
        std::cerr << "terrain-replay: terrain evaluation failed\n";
        return 5;
    }

    abi::AdapterLuid luid{};
    if (!QueryDefaultAdapterLuid(luid)) {
        std::cerr << "terrain-replay: D3D adapter query failed\n";
        return 5;
    }
    WindowsBackendModule module{options.backend};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    const auto loaded = host.Load(module, callbacks);
    if (!loaded || !host.RasterAvailable()) {
        std::cerr << "terrain-replay: backend load/API failed host="
                  << static_cast<unsigned>(loaded.error)
                  << " backend=" << static_cast<unsigned>(loaded.backendResult)
                  << " win32=" << module.LastErrorCode() << '\n';
        return 6;
    }
    abi::RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.flags = abi::RasterCreateAnyAdapter |
        (options.validation ? abi::RasterCreateValidation : 0u);
    create.adapterLuid = luid;
    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    if (!host.CreateRaster(create, status)) {
        std::cerr << "terrain-replay: create failed diagnostic="
                  << status.diagnostic << '\n';
        return 7;
    }

    const auto& view = frame.views.front();
    terrain::TerrainViewport target{};
    target.width = options.width;
    target.height = options.height;
    target.viewportWidth = static_cast<float>(options.width);
    target.viewportHeight = static_cast<float>(options.height);
    target.maxDepth = 1.0f;

    TerrainSubmission rendered;
    raster::DecodedPacket projected;
    auto referenceBuilt =
        scene::ProjectScenePacket(source, view, scenePacket, projected) ==
            scene::ScenePacketError::None &&
        scene::RenderReferenceGBuffer(projected, scenePacket,
            rendered.expected) == scene::ScenePacketError::None &&
        terrain::ComposeReferenceTerrainGBuffer(terrainPacket, view,
            layerTexture, target, rendered.expected) ==
            terrain::TerrainError::None;
    if (!referenceBuilt) {
        std::cerr << "terrain-replay: reference render failed\n";
    }
    const auto submitted = referenceBuilt && SubmitTerrainFrame(host,
        packetBytes, frameBytes, sceneBytes, terrainBytes, layerTextureBytes,
        options.width, options.height, rendered);
    if (referenceBuilt && !submitted) {
        std::cerr << "terrain-replay: submission failed diagnostic="
                  << rendered.submission.status.diagnostic
                  << " packet-error=" << rendered.submission.status.packetError
                  << " result="
                  << static_cast<unsigned>(rendered.submission.status.result)
                  << '\n';
    }
    std::uint64_t terrainPixels = 0;
    std::uint64_t firstCellPixels = 0;
    std::uint64_t secondCellPixels = 0;
    const auto expectedTerrainPixels = referenceBuilt
        ? CountObjectPixels(rendered.expected.pixels,
              terrainPacket.cells[0].cellId) +
          CountObjectPixels(rendered.expected.pixels,
              terrainPacket.cells[1].cellId)
        : 0;
    if (submitted) {
        rendered.comparison = scene::CompareGBuffer(
            rendered.expected.pixels, rendered.submission.gbuffer);
        rendered.interior = CompareInteriorPixels(
            rendered.expected, rendered.submission.gbuffer, 1.0e-3f);
        rendered.valid = true;
        firstCellPixels = CountObjectPixels(
            rendered.submission.gbuffer, terrainPacket.cells[0].cellId);
        secondCellPixels = CountObjectPixels(
            rendered.submission.gbuffer, terrainPacket.cells[1].cellId);
        terrainPixels = firstCellPixels + secondCellPixels;
    }

    // A terrain packet that claims another frame must be refused rather than
    // drawn against the wrong camera.
    auto mismatched = terrainPacket;
    mismatched.header.frameId = kSceneFrameIndex + 1;
    std::vector<std::byte> mismatchedBytes;
    TerrainSubmission rejectedFrame;
    const auto frameMismatchRejected =
        terrain::EncodeTerrainPacket(mismatched, mismatchedBytes) ==
            terrain::TerrainError::None &&
        !SubmitTerrainFrame(host, packetBytes, frameBytes, sceneBytes,
            mismatchedBytes, layerTextureBytes, options.width,
            options.height, rejectedFrame) &&
        rejectedFrame.submission.status.result ==
            abi::Result::RasterInvalidPacket;

    // A cell that references a layer outside the captured table must be
    // refused before it can sample an undefined array slice.
    auto missingLayer = terrainPacket;
    missingLayer.cells[0].layerSlots[1] = kTerrainLayerCount + 4;
    std::vector<std::byte> missingLayerBytes;
    TerrainSubmission rejectedLayer;
    const auto missingLayerRejected =
        terrain::EncodeTerrainPacket(missingLayer, missingLayerBytes) ==
            terrain::TerrainError::MissingLayer;
    if (missingLayerRejected) {
        // The encoder refuses it, so the backend is fed the raw bytes of a
        // valid packet with the slot rewritten in place instead.
        missingLayerBytes = terrainBytes;
        const auto slotOffset = terrainPacket.header.cellsOffset +
            offsetof(terrain::TerrainCellV1, layerSlots) + sizeof(std::uint32_t);
        std::uint32_t stray = kTerrainLayerCount + 4;
        if (slotOffset + sizeof(stray) <= missingLayerBytes.size()) {
            std::memcpy(missingLayerBytes.data() + slotOffset, &stray,
                sizeof(stray));
        }
    }
    const auto strayLayerRejected = missingLayerRejected &&
        !SubmitTerrainFrame(host, packetBytes, frameBytes, sceneBytes,
            missingLayerBytes, layerTextureBytes, options.width,
            options.height, rejectedLayer) &&
        rejectedLayer.submission.status.result ==
            abi::Result::RasterInvalidPacket;

    auto wrote = rendered.valid &&
        !rendered.submission.image.pixels.empty() &&
        WritePpm(options.output, rendered.submission.image);
    if (!options.terrainOutput.empty()) {
        wrote = wrote && WriteBinaryFile(options.terrainOutput, terrainBytes);
    }
    if (!options.gbufferOutput.empty()) {
        wrote = wrote && WriteGBuffer(options.gbufferOutput,
            rendered.submission.gbuffer);
    }
    if (!wrote) {
        std::cerr << "terrain-replay: artifact write failed\n";
    }

    status = {};
    status.structSize = sizeof(status);
    const auto destroyed = host.DestroyRaster(status);
    const auto validationErrors = status.validationErrorCount;
    const auto shutdown = host.RequestShutdown();
    const auto lifecyclePass = destroyed && validationErrors == 0 &&
        shutdown.error == BackendHostError::ShutdownDeferred;

    const auto passed = rendered.valid && wrote && lifecyclePass &&
        frameMismatchRejected && strayLayerRejected &&
        rendered.interior.mismatchedPixels == 0 &&
        rendered.comparison.identityMismatches == 0 &&
        evaluation.seamMismatches == 0 && evaluation.seamChecks > 0 &&
        evaluation.lodSeamChecks > 0 &&
        firstCellPixels > 0 && secondCellPixels > 0;
    std::cout << "terrain-replay extent=" << options.width << 'x'
              << options.height
              << " cells=" << terrainPacket.cells.size()
              << " layers=" << terrainPacket.layers.size()
              << " terrain-pixels=" << terrainPixels
              << " expected-terrain-pixels=" << expectedTerrainPixels
              << " cell0-pixels=" << firstCellPixels
              << " cell1-pixels=" << secondCellPixels
              << " seam-checks=" << evaluation.seamChecks
              << " lod-seam-checks=" << evaluation.lodSeamChecks
              << " seam-mismatches=" << evaluation.seamMismatches
              << " max-seam-gap=" << evaluation.maximumSeamGap
              << " gbuffer-identity-mismatches="
              << rendered.comparison.identityMismatches
              << " gbuffer-max-error="
              << rendered.comparison.maximumAbsoluteError
              << " gbuffer-mean-error="
              << rendered.comparison.meanAbsoluteError
              << " interior=" << rendered.interior.interiorPixels
              << " interior-mismatches="
              << rendered.interior.mismatchedPixels
              << " frame-mismatch-rejected="
              << (frameMismatchRejected ? "yes" : "no")
              << " stray-layer-rejected="
              << (strayLayerRejected ? "yes" : "no")
              << " validation-errors=" << validationErrors
              << " output=" << options.output.string()
              << " result=" << (passed ? "pass" : "fail") << '\n';
    return passed ? 0 : 1;
}

int RenderDeformedScene(const DeformedRenderOptions& options)
{
    view::FramePacket frame;
    std::vector<std::byte> frameBytes;
    scene::ScenePacket scenePacket;
    std::vector<std::byte> sceneBytes;
    if (!BuildSceneFrame(options.width, options.height, false,
            frame, frameBytes, false) ||
        !BuildDeformScene(scenePacket) ||
        scene::EncodeScenePacket(scenePacket, sceneBytes) !=
            scene::ScenePacketError::None) {
        std::cerr << "deform-replay: fixture construction failed\n";
        return 5;
    }

    abi::AdapterLuid luid{};
    if (!QueryDefaultAdapterLuid(luid)) {
        std::cerr << "deform-replay: D3D adapter query failed\n";
        return 5;
    }
    WindowsBackendModule module{options.backend};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    const auto loaded = host.Load(module, callbacks);
    if (!loaded || !host.RasterAvailable()) {
        std::cerr << "deform-replay: backend load/API failed host="
                  << static_cast<unsigned>(loaded.error)
                  << " backend=" << static_cast<unsigned>(loaded.backendResult)
                  << " win32=" << module.LastErrorCode() << '\n';
        return 6;
    }
    abi::RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.flags = abi::RasterCreateAnyAdapter |
        (options.validation ? abi::RasterCreateValidation : 0u);
    create.adapterLuid = luid;
    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    if (!host.CreateRaster(create, status)) {
        std::cerr << "deform-replay: create failed diagnostic="
                  << status.diagnostic << '\n';
        return 7;
    }

    // Every frame changes the pose, so a stale ring range or a skipped
    // dispatch cannot pass by reproducing the previous frame.
    auto passed = true;
    DeformSubmission last;
    std::vector<std::byte> lastDeformBytes;
    float worstPositionError = 0.0f;
    float worstMotionError = 0.0f;
    float worstGBufferMean = 0.0f;
    std::uint64_t worstIdentityMismatches = 0;
    for (std::uint32_t frameIndex = 0; frameIndex < options.frames;
         ++frameIndex) {
        const auto deformPacket = BuildDeformFixture(frameIndex);
        std::vector<std::byte> deformBytes;
        if (deform::EncodeDeformationPacket(deformPacket, deformBytes) !=
            deform::DeformError::None) {
            std::cerr << "deform-replay: deformation encode failed\n";
            passed = false;
            break;
        }
        deform::DeformationResult reference{};
        if (deform::EvaluateDeformation(deformPacket, DeformBaseTriangle(),
                reference) != deform::DeformError::None) {
            std::cerr << "deform-replay: reference deformation failed\n";
            passed = false;
            break;
        }
        raster::DecodedPacket bindSource;
        std::vector<std::byte> bindBytes;
        raster::DecodedPacket deformedSource;
        std::vector<std::byte> deformedBytes;
        if (!BuildDeformSource(options.width, options.height,
                DeformBaseTriangle(), bindSource, bindBytes) ||
            !BuildDeformSource(options.width, options.height,
                reference.current, deformedSource, deformedBytes)) {
            std::cerr << "deform-replay: source packet failed\n";
            passed = false;
            break;
        }
        DeformSubmission rendered;
        // The oracle rasterizes the CPU-deformed stream while the backend
        // receives only the bind pose, so Vulkan has to run the kernel.
        raster::DecodedPacket projected;
        if (scene::ProjectScenePacket(deformedSource, frame.views.front(),
                scenePacket, projected) != scene::ScenePacketError::None ||
            scene::RenderReferenceGBuffer(projected, scenePacket,
                rendered.expected) != scene::ScenePacketError::None) {
            std::cerr << "deform-replay: reference render failed\n";
            passed = false;
            break;
        }
        if (!SubmitDeformedFrame(host, bindBytes, frameBytes, sceneBytes,
                deformBytes, options.width, options.height,
                static_cast<std::uint32_t>(deformPacket.vertices.size()),
                rendered)) {
            std::cerr << "deform-replay: submission failed diagnostic="
                      << rendered.submission.status.diagnostic << '\n';
            passed = false;
            break;
        }
        for (std::size_t index = 0; index < reference.current.size();
             ++index) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                rendered.maximumPositionError = std::max(
                    rendered.maximumPositionError,
                    std::abs(rendered.output[index].current[axis] -
                        reference.current[index][axis]));
                const auto referenceMotion =
                    reference.current[index][axis] -
                    reference.previous[index][axis];
                const auto observedMotion =
                    rendered.output[index].current[axis] -
                    rendered.output[index].previous[axis];
                rendered.maximumMotionError = std::max(
                    rendered.maximumMotionError,
                    std::abs(observedMotion - referenceMotion));
            }
        }
        rendered.referenceMotion = reference.motionMagnitude;
        rendered.comparison = scene::CompareGBuffer(
            rendered.expected.pixels, rendered.submission.gbuffer);
        rendered.interior = CompareInteriorPixels(
            rendered.expected, rendered.submission.gbuffer, 1.0e-3f);
        rendered.valid = true;
        worstPositionError = std::max(worstPositionError,
            rendered.maximumPositionError);
        worstMotionError = std::max(worstMotionError,
            rendered.maximumMotionError);
        worstGBufferMean = std::max(worstGBufferMean,
            static_cast<float>(rendered.comparison.meanAbsoluteError));
        worstIdentityMismatches = std::max(worstIdentityMismatches,
            rendered.comparison.identityMismatches);
        if (rendered.interior.mismatchedPixels != 0 ||
            rendered.maximumPositionError > 1.0e-4f ||
            rendered.maximumMotionError > 1.0e-4f ||
            reference.motionMagnitude <= 0.0f) {
            passed = false;
        }
        last = std::move(rendered);
        lastDeformBytes = std::move(deformBytes);
    }

    // A changed topology with an unchanged generation must fail closed.
    const auto grown = BuildDeformFixture(0, 4, 1);
    std::vector<std::byte> grownBytes;
    raster::DecodedPacket grownSource;
    std::vector<std::byte> grownSourceBytes;
    std::array<std::array<float, 3>, 4> grownPositions{};
    std::copy(DeformBaseTriangle().begin(), DeformBaseTriangle().end(),
        grownPositions.begin());
    grownPositions[3] = {0.4f, -0.2f, 0.0f};
    DeformSubmission rejected;
    const auto topologyRejected =
        deform::EncodeDeformationPacket(grown, grownBytes) ==
            deform::DeformError::None &&
        BuildDeformSource(options.width, options.height, grownPositions,
            grownSource, grownSourceBytes) &&
        !SubmitDeformedFrame(host, grownSourceBytes, frameBytes, sceneBytes,
            grownBytes, options.width, options.height, 4, rejected) &&
        rejected.submission.status.result ==
            abi::Result::RasterInvalidPacket;

    // The same topology under a new generation is accepted.
    const auto regenerated = BuildDeformFixture(0, 4, 2);
    std::vector<std::byte> regeneratedBytes;
    DeformSubmission accepted;
    const auto generationAccepted =
        deform::EncodeDeformationPacket(regenerated, regeneratedBytes) ==
            deform::DeformError::None &&
        SubmitDeformedFrame(host, grownSourceBytes, frameBytes, sceneBytes,
            regeneratedBytes, options.width, options.height, 4, accepted);

    auto wrote = last.valid && !last.submission.image.pixels.empty() &&
        WritePpm(options.output, last.submission.image);
    if (!options.deformOutput.empty()) {
        wrote = wrote && WriteBinaryFile(options.deformOutput,
            lastDeformBytes);
    }
    if (!options.gbufferOutput.empty()) {
        wrote = wrote && WriteGBuffer(options.gbufferOutput,
            last.submission.gbuffer);
    }
    if (!wrote) {
        std::cerr << "deform-replay: artifact write failed\n";
    }

    status = {};
    status.structSize = sizeof(status);
    const auto destroyed = host.DestroyRaster(status);
    const auto validationErrors = status.validationErrorCount;
    const auto shutdown = host.RequestShutdown();
    const auto lifecyclePass = destroyed && validationErrors == 0 &&
        shutdown.error == BackendHostError::ShutdownDeferred;

    passed = passed && last.valid && topologyRejected &&
        generationAccepted && wrote && lifecyclePass;
    std::cout << "deform-replay extent=" << options.width << 'x'
              << options.height
              << " frames=" << options.frames
              << " vertices=" << last.output.size()
              << " max-position-error=" << worstPositionError
              << " max-motion-error=" << worstMotionError
              << " reference-motion=" << last.referenceMotion
              << " gbuffer-identity-mismatches=" << worstIdentityMismatches
              << " gbuffer-mean-error=" << worstGBufferMean
              << " interior=" << last.interior.interiorPixels
              << " interior-mismatches=" << last.interior.mismatchedPixels
              << " topology-rejected="
              << (topologyRejected ? "yes" : "no")
              << " generation-accepted="
              << (generationAccepted ? "yes" : "no")
              << " submissions=" << accepted.submission.status.submissionCount
              << " validation-errors=" << validationErrors
              << " output=" << options.output.string()
              << " result=" << (passed ? "pass" : "fail") << '\n';
    return passed ? 0 : 19;
}

void PrintUsage()
{
    std::cerr
        << "usage:\n"
        << "  vf_packet_replay --inspect <trace.vftrace>\n"
        << "  vf_packet_replay --inspect-frame <capture.vfframe>\n"
        << "  vf_packet_replay --render-synthetic --backend <dll> "
           "--output <image.ppm> [--width N] [--height N] "
           "[--index-width 16|32] [--validation] [--fixtures] "
           "[--texture <capture.vftex>|--texture-fixture] "
           "[--material <capture.vfmat>|--material-fixture] "
           "[--frame <capture.vfframe>|--view-fixture] "
           "[--frame-output <capture.vfframe>]\n"
        << "  vf_packet_replay --render-mesh <mesh.vfmesh> --backend <dll> "
           "--output <image.ppm> [--width N] [--height N] [--validation] "
           "[--texture <capture.vftex>|--texture-fixture] "
           "[--material <capture.vfmat>|--material-fixture]\n"
        << "  vf_packet_replay --render-material <material.vfmat> "
           "--output <image.ppm> [--width N] [--height N]\n"
        << "  vf_packet_replay --render-material-fixture --output <image.ppm> "
           "[--bundle-output <material.vfmat>] [--width N] [--height N]\n"
        << "  vf_packet_replay --render-scene --backend <dll> "
           "--output <image.ppm> [--scene-output <scene.vfscene>] "
           "[--gbuffer-output <scene.vfgbuf>] [--width N] [--height N] "
           "[--validation]\n"
        << "  vf_packet_replay --render-instanced-scene --backend <dll> "
           "--output <image.ppm> [--scene-output <scene.vfscene>] "
           "[--gbuffer-output <scene.vfgbuf>] "
           "[--trace-output <registry.vftrace>] [--width N] [--height N] "
           "[--validation]\n"
        << "  vf_packet_replay --render-deformed-scene --backend <dll> "
           "--output <image.ppm> [--deform-output <pose.vfdeform>] "
           "[--gbuffer-output <scene.vfgbuf>] [--frames N] [--width N] "
           "[--height N] [--validation]\n"
        << "  vf_packet_replay --render-terrain-scene --backend <dll> "
           "--output <image.ppm> [--terrain-output <cells.vfterrain>] "
           "[--gbuffer-output <scene.vfgbuf>] [--width N] [--height N] "
           "[--validation]\n"
        << "  vf_packet_replay --render-family-scene --backend <dll> "
           "--output <image.ppm> [--scene-output <scene.vfscene>] "
           "[--family-output <families.vffam>] "
           "[--gbuffer-output <scene.vfgbuf>] [--width N] [--height N] "
           "[--validation]\n";
}

}

int main(const int argc, const char* const* argv)
{
    if (argc == 3 && std::string_view{argv[1]} == "--inspect") {
        return InspectFile(std::filesystem::path{argv[2]});
    }
    if (argc == 3 && std::string_view{argv[1]} == "--inspect-frame") {
        return InspectFrame(std::filesystem::path{argv[2]});
    }
    if (argc >= 2 && std::string_view{argv[1]} == "--render-synthetic") {
        RenderOptions options;
        if (!ParseRenderOptions(argc, argv, 2, options)) {
            PrintUsage();
            return 2;
        }
        return RenderSynthetic(options);
    }
    if (argc >= 3 && std::string_view{argv[1]} == "--render-mesh") {
        RenderOptions options;
        options.meshInput = argv[2];
        if (!ParseRenderOptions(argc, argv, 3, options)) {
            PrintUsage();
            return 2;
        }
        return RenderCapturedMesh(options);
    }
    if (argc >= 2 &&
        std::string_view{argv[1]} == "--render-alpha-scene") {
        AlphaRenderOptions options;
        if (!ParseAlphaRenderOptions(argc, argv, 2, options)) {
            PrintUsage();
            return 2;
        }
        return RenderAlphaScene(options);
    }
    if (argc >= 2 &&
        std::string_view{argv[1]} == "--render-indirect-accumulation") {
        FamilyRenderOptions options;
        if (!ParseFamilyRenderOptions(argc, argv, 2, options)) {
            PrintUsage();
            return 2;
        }
        return RenderIndirectAccumulation(options);
    }
    if (argc >= 2 &&
        std::string_view{argv[1]} == "--render-mirror-scene") {
        FamilyRenderOptions options;
        if (!ParseFamilyRenderOptions(argc, argv, 2, options)) {
            PrintUsage();
            return 2;
        }
        return RenderMirrorScene(options);
    }
    if (argc >= 2 &&
        std::string_view{argv[1]} == "--render-family-scene") {
        FamilyRenderOptions options;
        if (!ParseFamilyRenderOptions(argc, argv, 2, options)) {
            PrintUsage();
            return 2;
        }
        return RenderFamilyScene(options);
    }
    if (argc >= 2 &&
        std::string_view{argv[1]} == "--render-terrain-scene") {
        TerrainRenderOptions options;
        if (!ParseTerrainRenderOptions(argc, argv, 2, options)) {
            PrintUsage();
            return 2;
        }
        return RenderTerrainScene(options);
    }
    if (argc >= 2 &&
        std::string_view{argv[1]} == "--render-deformed-scene") {
        DeformedRenderOptions options;
        if (!ParseDeformedRenderOptions(argc, argv, 2, options)) {
            PrintUsage();
            return 2;
        }
        return RenderDeformedScene(options);
    }
    if (argc >= 2 &&
        std::string_view{argv[1]} == "--render-instanced-scene") {
        InstancedRenderOptions options;
        if (!ParseInstancedRenderOptions(argc, argv, 2, options)) {
            PrintUsage();
            return 2;
        }
        return RenderInstancedScene(options);
    }
    if (argc >= 2 && std::string_view{argv[1]} == "--render-scene") {
        SceneRenderOptions options;
        if (!ParseSceneRenderOptions(argc, argv, 2, options)) {
            PrintUsage();
            return 2;
        }
        return RenderScene(options);
    }
    if (argc >= 3 && std::string_view{argv[1]} == "--render-material") {
        MaterialRenderOptions options;
        options.input = argv[2];
        if (!ParseMaterialRenderOptions(argc, argv, 3, options)) {
            PrintUsage();
            return 2;
        }
        return RenderMaterial(options);
    }
    if (argc >= 2 &&
        std::string_view{argv[1]} == "--render-material-fixture") {
        MaterialRenderOptions options;
        options.fixture = true;
        if (!ParseMaterialRenderOptions(argc, argv, 2, options)) {
            PrintUsage();
            return 2;
        }
        return RenderMaterial(options);
    }
    PrintUsage();
    return 2;
}
