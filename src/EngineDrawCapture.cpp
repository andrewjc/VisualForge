#include "EngineDrawCapture.h"

#include "Log.h"
#include "DepthCapture.h"
#include "EngineWorldSuppression.h"
#include "renderer_core/EngineSuppression.h"
#include "renderer_core/EngineVertex.h"
#include "renderer_core/ShaderReflection.h"

#include <MinHook.h>
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <mutex>
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>

namespace vf::engine_draw_capture {

namespace {

// These run on the render thread once per draw, so the whole body is a few
// relaxed atomics. No allocation, no logging, no lock: a capture that costs
// the frame its budget changes the thing it is measuring.
std::atomic<std::uint64_t> s_drawCalls{};
std::atomic<std::uint64_t> s_instancedDrawCalls{};
std::atomic<std::uint64_t> s_indices{};
std::atomic<std::uint32_t> s_largestIndexCount{};
std::atomic<std::uint64_t> s_overflowed{};

// A small open-addressed table of the vertex buffers bound this frame. Fixed
// capacity on purpose: an unbounded set would allocate on the render thread.
// Overflow is counted and reported rather than hidden.
constexpr std::size_t kBufferTableSize = 4096;

// Input layouts the engine created. Fallout 4 builds a bounded set of vertex
// formats at load, so this is sized for all of them with room to spare.
constexpr std::size_t kMaximumRecordedLayouts = 512;
constexpr std::size_t kMaximumLayoutElements = 16;
std::array<std::atomic<std::uintptr_t>, kBufferTableSize> s_bufferTable{};
std::atomic<std::uint32_t> s_distinctBuffers{};

// The buffer bound by the most recent IASetVertexBuffers on this thread. A
// draw takes its geometry from whatever is bound when it runs, so the two
// have to be paired per thread rather than globally.
thread_local std::uintptr_t t_currentVertexBuffer = 0;
thread_local ID3D11Buffer* t_currentVertexBufferObject = nullptr;
thread_local std::uint32_t t_currentVertexStride = 0;
thread_local std::uint32_t t_currentVertexOffset = 0;
thread_local std::uintptr_t t_currentInputLayout = 0;
thread_local std::uintptr_t t_currentIndexBuffer = 0;
thread_local std::uint32_t t_currentIndexFormat = 0;
thread_local std::uint32_t t_currentIndexOffset = 0;
// The buffer the engine has bound at vertex-shader constant slot 0. Taking
// the transform from "the last small buffer unmapped" was too loose: shader
// variants map several small buffers per draw and the scene packet rejected
// the result as an invalid affine. The transform is read only from the
// buffer the draw will actually read it from.
thread_local std::uintptr_t t_vsConstantSlot0 = 0;
// The pixel shader bound on this thread. Which shader is active decides what
// the pixel-shader resource slots *mean*: the engine has no fixed convention
// for which slot carries a draw's base colour, so "the texture at slot 0" is
// only an answer once the shader that declared slot 0 is known.
thread_local std::uintptr_t t_currentPixelShader = 0;
// Set when the engine writes a matrix-sized constant buffer, cleared when a
// draw consumes it. Per-draw constant buffers are written immediately before
// the draw that reads them, so a draw with a fresh write is world geometry
// and one without is a pass reusing whatever is bound -- user interface,
// fullscreen, or a second pass over geometry already recorded. Deciding by
// buffer size instead depended on a description table that fills up, and
// deciding by "is bound at slot zero" failed because the engine binds after
// it maps.
thread_local bool t_transformFresh = false;

// The transform the engine wrote into its per-draw constant buffer most
// recently on this thread. The engine maps, writes, unmaps, then draws, so
// the draw takes whatever the last unmap left.
thread_local float t_lastTransform[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f};

// The draw arena. Written on the render thread with one atomic increment per
// draw and no allocation; overflow is counted so a short frame is a known gap
// rather than a scene quietly missing objects.
std::array<renderer::drawstream::DrawRecordV1, kDrawArenaCapacity> s_arena{};
std::atomic<std::uint32_t> s_arenaCount{};
std::atomic<std::uint64_t> s_arenaDropped{};

using DrawIndexedFn = void(__stdcall*)(void*, UINT, UINT, INT);
using DrawIndexedInstancedFn =
    void(__stdcall*)(void*, UINT, UINT, UINT, INT, UINT);
using SetVertexBuffersFn = void(__stdcall*)(
    void*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
using SetConstantBuffersFn = void(__stdcall*)(
    void*, UINT, UINT, ID3D11Buffer* const*);
using SetIndexBufferFn = void(__stdcall*)(void*, void*, UINT, UINT);
using SetInputLayoutFn = void(__stdcall*)(void*, void*);
using CreateInputLayoutFn = HRESULT(__stdcall*)(
    void*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T,
    ID3D11InputLayout**);
using CreatePixelShaderFn = HRESULT(__stdcall*)(
    void*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using SetPixelShaderFn = void(__stdcall*)(
    void*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using MapFn = HRESULT(__stdcall*)(
    void*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UpdateSubresourceFn = void(__stdcall*)(
    void*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);

DrawIndexedFn s_origDrawIndexed = nullptr;
DrawIndexedInstancedFn s_origDrawIndexedInstanced = nullptr;
SetVertexBuffersFn s_origSetVertexBuffers = nullptr;
SetIndexBufferFn s_origSetIndexBuffer = nullptr;
SetInputLayoutFn s_origSetInputLayout = nullptr;
CreateInputLayoutFn s_origCreateInputLayout = nullptr;
CreatePixelShaderFn s_origCreatePixelShader = nullptr;
SetPixelShaderFn s_origSetPixelShader = nullptr;
SetConstantBuffersFn s_origSetVsConstantBuffers = nullptr;
SetConstantBuffersFn s_origSetPsConstantBuffers = nullptr;
MapFn s_origMap = nullptr;
UpdateSubresourceFn s_origUpdateSubresource = nullptr;

void* s_drawIndexedAddress = nullptr;
void* s_drawIndexedInstancedAddress = nullptr;
void* s_setVertexBuffersAddress = nullptr;
void* s_setIndexBufferAddress = nullptr;
void* s_setInputLayoutAddress = nullptr;
void* s_createInputLayoutAddress = nullptr;
void* s_createPixelShaderAddress = nullptr;
void* s_setPixelShaderAddress = nullptr;
void* s_createRasterizerStateAddress = nullptr;
void* s_setRasterizerStateAddress = nullptr;

using CreateRasterizerStateFn = HRESULT(__stdcall*)(
    void*, const D3D11_RASTERIZER_DESC*, ID3D11RasterizerState**);
using RSSetStateFn = void(__stdcall*)(void*, ID3D11RasterizerState*);

CreateRasterizerStateFn s_origCreateRasterizerState = nullptr;
RSSetStateFn s_origSetRasterizerState = nullptr;

// The winding and cull mode of every rasterizer state the engine created.
//
// D3D11 offers no way to read a D3D11_RASTERIZER_DESC back off an
// ID3D11RasterizerState, so creation is the only moment it is visible -- the
// same reason CreateInputLayout has to be hooked for vertex formats.
struct DescribedRasterizer
{
    std::atomic<std::uintptr_t> state{};
    bool frontCounterClockwise{};
    std::uint32_t cullMode{};
};

constexpr std::size_t kMaximumRasterizerStates = 512;
std::array<DescribedRasterizer, kMaximumRasterizerStates> s_rasterizerStates{};
std::atomic<std::uint32_t> s_rasterizerStateCount{};
std::atomic<std::uint64_t> s_rasterizerStateOverflow{};

// The state bound on this thread, already resolved. Draws outnumber state
// changes by a wide margin, so the table walk is paid per bind.
thread_local bool t_frontCounterClockwise = false;
thread_local std::uint32_t t_cullMode = renderer::drawstream::kCullModeUnknown;

// Whether the capture is observing anything at all.
//
// Without these, a draw that never saw a rasterizer state and a draw whose
// state said clockwise are indistinguishable in the output, and replacing a
// hardcoded winding with an uncaptured one would look exactly like a fix.
std::atomic<std::uint64_t> s_rasterizerBinds{};
std::atomic<std::uint64_t> s_rasterizerBindsUnmatched{};
std::atomic<std::uint64_t> s_drawsKnownCull{};
std::atomic<std::uint64_t> s_drawsUnknownCull{};
std::atomic<std::uint64_t> s_drawsFrontCcw{};
std::atomic<std::uint64_t> s_drawsFrontCw{};
void* s_setVsConstantBuffersAddress = nullptr;
void* s_setPsConstantBuffersAddress = nullptr;
void* s_mapAddress = nullptr;
void* s_updateSubresourceAddress = nullptr;
bool s_enabled = false;

// First-sighting descriptions, written once per buffer under a spin-free
// claim. GetDesc is a vtable call, so it is paid only on the first draw that
// names a buffer, never per draw.
struct DescribedBuffer
{
    std::atomic<std::uintptr_t> handle{};
    std::uint32_t byteWidth{};
    std::uint32_t stride{};
    std::uint32_t usage{};
    std::uint32_t cpuAccessFlags{};
    std::uint32_t bindFlags{};
    std::atomic<std::uint64_t> draws{};
    std::atomic<std::uint64_t> maps{};
};

std::array<DescribedBuffer, kMaximumReportedBuffers> s_describedVertex{};
std::array<DescribedBuffer, kMaximumReportedBuffers> s_describedConstant{};
// Pixel-shader constants, described separately from the vertex ones. The
// per-frame lighting block lives here, and mixing the two tables would make a
// crowded vertex table evict it.
//
// Far larger than the vertex table, and measured rather than guessed: at
// thirty-two entries the engine filled every slot, nineteen of them with
// immutable blocks that are never written and can never carry a frame's
// lighting, and the per-frame buffers the shaders name were simply never
// described. A table that fills is a table that reports whichever buffers
// happened to bind first.
std::array<DescribedBuffer, kMaximumPsConstantBuffers> s_describedPsConstant{};
std::atomic<std::uint64_t> s_psBinds{};
std::atomic<std::uint64_t> s_psSampled{};
std::atomic<std::uint64_t> s_psDescribeOverflow{};
// The last per-frame pixel-shader constants seen, under a lock because the
// mirror reads them from the present thread while the render thread writes.
std::mutex s_lightingMutex;
std::array<LightingSample, kLightingSampleSlots> s_lighting{};

// The constant-buffer layouts the engine's own pixel shaders declare, keyed by
// name and width so the hundreds of techniques that share a block collapse to
// one entry. Built at shader-creation time, which is off the render thread's
// hot path -- Fallout 4 compiles its shaders once at load.
std::mutex s_reflectionMutex;
std::array<ShaderBufferLayout, kShaderBufferLayoutCapacity> s_shaderBuffers{};
std::uint32_t s_shaderBufferCount = 0;
std::atomic<std::uint64_t> s_shadersSeen{};
std::atomic<std::uint64_t> s_shadersReflected{};
std::atomic<std::uint64_t> s_shadersFailed{};
std::atomic<std::uint32_t> s_reflectLayoutOverflow{};
std::atomic<std::uint32_t> s_fieldOverflow{};

// The texture/sampler bindings the engine's own pixel shaders declare,
// collapsed the same way the constant-buffer catalogue is: by exact name and
// slot, with a per-entry count of how many shaders share that declaration.
std::array<ShaderResourceBinding, kShaderResourceCapacity> s_shaderResources{};
std::uint32_t s_shaderResourceCount = 0;
std::atomic<std::uint32_t> s_shaderResourceOverflow{};

// No base-colour texture. Distinct from slot 0, which is a real register.
constexpr std::uint32_t kNoBaseColorSlot = 0xFFFF'FFFFu;

// Which register each created pixel shader binds its base colour at, resolved
// once from that shader's own reflection at creation time. Storing the answer
// rather than the whole resource list: the question asked per draw is only
// ever "which register", and doing the name matching once at creation keeps
// it off the render thread entirely.
//
// Open-addressed and a power of two, for the same reason the pixel-shader
// constant table is: the engine creates thousands of these and a linear scan
// would be paid on every draw.
constexpr std::size_t kMaximumDescribedShaders = 8192;

struct DescribedShader
{
    std::atomic<std::uintptr_t> handle{};
    std::uint32_t baseColorSlot{kNoBaseColorSlot};
};

std::array<DescribedShader, kMaximumDescribedShaders> s_describedShaders{};
std::atomic<std::uint32_t> s_shadersDescribed{};
std::atomic<std::uint32_t> s_shadersWithBaseColor{};

// Only the low registers are tracked. D3D11 allows 128 shader-resource slots
// per stage, but a material's textures live in the first handful, and a table
// this size is a single cache line's worth of pointers per thread.
constexpr std::size_t kTrackedPixelShaderSlots = 16;
thread_local void* t_pixelShaderResources[kTrackedPixelShaderSlots] = {};
std::atomic<std::uint64_t> s_psResourceNotices{};
std::atomic<std::uint64_t> s_drawsWithBaseColor{};
std::atomic<std::uint64_t> s_drawsMissingBaseColor{};
std::atomic<std::uint64_t> s_drawsNoShader{};
// `t_currentPixelShader == 0` has two causes that call for opposite responses,
// and counting them as one is what has made this population look like a single
// 36% defect.
//
// The engine binds a null pixel shader for every depth-only pass -- the depth
// prepass and each shadow cascade -- and those draws correctly have no albedo
// to find. A thread that has never had a PSSetShader observed at all is the
// other thing entirely: state this module never saw, and the only one of the
// two that is a capture gap.
thread_local bool t_pixelShaderEverSet = false;
std::atomic<std::uint64_t> s_drawsShaderExplicitNull{};
std::atomic<std::uint64_t> s_drawsShaderNeverSet{};
// Of the explicit nulls, how many ran with the main scene depth bound. A depth
// prepass writes the scene depth; a shadow cascade writes its own map. The
// split says which of the two the population actually is, rather than leaving
// "depth-only" as one word covering both.
std::atomic<std::uint64_t> s_drawsNullSceneDepth{};
std::atomic<std::uint64_t> s_drawsNullOtherTarget{};
std::atomic<std::uint64_t> s_drawsShaderUnknown{};
std::atomic<std::uint64_t> s_drawsShaderNoBase{};
std::atomic<std::uint64_t> s_drawsConventionBaseColor{};
std::atomic<std::uint64_t> s_psShaderBinds{};

// A view the engine bound, and the resource behind it. GetResource is a
// vtable call that also touches the reference count, so it is paid once per
// distinct view rather than once per draw.
struct ResolvedView
{
    std::atomic<std::uintptr_t> view{};
    std::uintptr_t resource{};
};

constexpr std::size_t kMaximumResolvedViews = 8192;
std::array<ResolvedView, kMaximumResolvedViews> s_resolvedViews{};

void CopyName(char (&destination)[64], const std::string& source) noexcept
{
    const std::size_t length = std::min(source.size(), sizeof(destination) - 1);
    std::memcpy(destination, source.data(), length);
    destination[length] = '\0';
}

void DescribeShader(std::uintptr_t handle, std::uint32_t baseColorSlot) noexcept;

// Folds one shader's declarations into the catalogue. A buffer already present
// under the same name and width has its shader count raised rather than being
// stored twice: what distinguishes engine-wide state from a technique's own
// constants is how many techniques declare it.
void RecordShaderReflection(
    const void* const bytecode,
    const SIZE_T length,
    const std::uintptr_t shaderHandle) noexcept
{
    s_shadersSeen.fetch_add(1, std::memory_order_relaxed);
    if (bytecode == nullptr || length == 0) {
        s_shadersFailed.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    renderer::shader::ReflectedShader reflection{};
    const std::span<const std::byte> bytes{
        static_cast<const std::byte*>(bytecode), static_cast<std::size_t>(length)};
    if (renderer::shader::ReflectShader(bytes, reflection)
        != renderer::shader::ReflectionError::None) {
        s_shadersFailed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s_shadersReflected.fetch_add(1, std::memory_order_relaxed);

    // Resolved here, once, rather than per draw: the answer is a property of
    // the shader and this runs at creation time, off the render thread.
    {
        std::uint32_t baseColorSlot = 0;
        if (!renderer::shader::FindBaseColorTextureSlot(
                reflection, baseColorSlot)) {
            baseColorSlot = kNoBaseColorSlot;
        }
        DescribeShader(shaderHandle, baseColorSlot);
    }

    const std::lock_guard<std::mutex> guard{s_reflectionMutex};
    for (const auto& buffer : reflection.buffers) {
        ShaderBufferLayout candidate{};
        CopyName(candidate.name, buffer.name);
        candidate.byteWidth = buffer.size;

        ShaderBufferLayout* slot = nullptr;
        for (std::uint32_t index = 0; index < s_shaderBufferCount; ++index) {
            auto& existing = s_shaderBuffers[index];
            if (existing.byteWidth == candidate.byteWidth &&
                std::strcmp(existing.name, candidate.name) == 0) {
                slot = &existing;
                break;
            }
        }
        if (slot == nullptr) {
            if (s_shaderBufferCount >= kShaderBufferLayoutCapacity) {
                s_reflectLayoutOverflow.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            slot = &s_shaderBuffers[s_shaderBufferCount++];
            *slot = candidate;
            for (const auto& variable : buffer.variables) {
                if (slot->fieldCount >= kShaderBufferFieldCapacity) {
                    s_fieldOverflow.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                auto& field = slot->fields[slot->fieldCount++];
                CopyName(field.name, variable.name);
                field.offset = variable.offset;
                field.size = variable.size;
            }
        }
        ++slot->shaders;
    }

    for (const auto& resource : reflection.resources) {
        ShaderResourceBinding* slot = nullptr;
        for (std::uint32_t index = 0; index < s_shaderResourceCount; ++index) {
            auto& existing = s_shaderResources[index];
            if (existing.bindPoint == resource.bindPoint &&
                std::strcmp(existing.name, resource.name.c_str()) == 0) {
                slot = &existing;
                break;
            }
        }
        if (slot == nullptr) {
            if (s_shaderResourceCount >= kShaderResourceCapacity) {
                s_shaderResourceOverflow.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            slot = &s_shaderResources[s_shaderResourceCount++];
            CopyName(slot->name, resource.name);
            slot->kind = static_cast<std::uint8_t>(resource.kind);
            slot->bindPoint = resource.bindPoint;
            slot->bindCount = resource.bindCount;
        }
        ++slot->shaders;
    }
}

// The engine hands the compiled bytecode to the device, and the bytecode
// carries the reflection chunk. This is the one moment it is readable: D3D11
// keeps no way to get it back off an ID3D11PixelShader afterwards.
HRESULT __stdcall HookedCreatePixelShader(
    void* self,
    const void* bytecode,
    const SIZE_T length,
    ID3D11ClassLinkage* linkage,
    ID3D11PixelShader** shader) noexcept
{
    const auto result = s_origCreatePixelShader(self, bytecode, length, linkage, shader);
    if (SUCCEEDED(result)) {
        RecordShaderReflection(bytecode, length,
            shader != nullptr
                ? reinterpret_cast<std::uintptr_t>(*shader)
                : std::uintptr_t{0});
    }
    return result;
}

void Describe(
    std::span<DescribedBuffer> table,
    ID3D11Buffer* const buffer,
    const std::uint32_t stride,
    const bool countDraw) noexcept
{
    if (buffer == nullptr) return;
    const auto handle = reinterpret_cast<std::uintptr_t>(buffer);
    for (auto& entry : table) {
        auto existing = entry.handle.load(std::memory_order_acquire);
        if (existing == handle) {
            if (countDraw) {
                entry.draws.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        if (existing != 0) continue;
        std::uintptr_t expected = 0;
        if (!entry.handle.compare_exchange_strong(expected, handle,
                std::memory_order_acq_rel)) {
            continue;
        }
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        entry.byteWidth = desc.ByteWidth;
        entry.stride = stride != 0 ? stride : desc.StructureByteStride;
        entry.usage = static_cast<std::uint32_t>(desc.Usage);
        entry.cpuAccessFlags = desc.CPUAccessFlags;
        entry.bindFlags = desc.BindFlags;
        if (countDraw) {
            entry.draws.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
}

// Locates a buffer's entry in the pixel-shader table, claiming a free one if
// this is the first sighting. Returns null when the table is full.
//
// Open-addressed rather than scanned from the front. The engine creates far
// more distinct pixel-shader constant buffers than a small table holds -- at
// 256 entries every slot filled, and the blocks the shaders name were still
// missing, because a linear table keeps whichever buffers bound first and
// those are the per-object ones. A scan also costs its whole length on every
// bind, and there are over a million binds in a minute of play.
// Looks a buffer up without claiming a slot. The write hooks see every
// resource the engine writes -- textures, vertex buffers, staging copies --
// and must not enter them into a table that exists for pixel-shader
// constants. Only a bind proves what a resource is used as.
[[nodiscard]] DescribedBuffer* FindPsConstant(
    const std::uintptr_t handle) noexcept
{
    const std::size_t mask = s_describedPsConstant.size() - 1;
    std::size_t index = static_cast<std::size_t>(handle >> 4) & mask;
    for (std::size_t step = 0; step <= mask; ++step) {
        auto& entry = s_describedPsConstant[index];
        const auto existing = entry.handle.load(std::memory_order_acquire);
        if (existing == handle) {
            return &entry;
        }
        if (existing == 0) {
            return nullptr;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

[[nodiscard]] DescribedBuffer* FindOrClaimPsConstant(
    const std::uintptr_t handle) noexcept
{
    const std::size_t mask = s_describedPsConstant.size() - 1;
    // Buffer addresses are at least sixteen-byte aligned, so the low bits
    // carry no information and hashing on them would pile every buffer into
    // a fraction of the table.
    std::size_t index = static_cast<std::size_t>(handle >> 4) & mask;
    for (std::size_t step = 0; step <= mask; ++step) {
        auto& entry = s_describedPsConstant[index];
        const auto existing = entry.handle.load(std::memory_order_acquire);
        if (existing == handle) {
            return &entry;
        }
        if (existing == 0) {
            std::uintptr_t expected = 0;
            if (entry.handle.compare_exchange_strong(expected, handle,
                    std::memory_order_acq_rel)) {
                return &entry;
            }
            if (expected == handle) {
                return &entry;
            }
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

void DescribePsConstant(ID3D11Buffer* const buffer) noexcept
{
    if (buffer == nullptr) return;
    auto* const entry =
        FindOrClaimPsConstant(reinterpret_cast<std::uintptr_t>(buffer));
    if (entry == nullptr) {
        s_psDescribeOverflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Described once, on the claim that first stored the width as zero. A
    // GetDesc is a vtable call and this runs on every bind.
    if (entry->byteWidth != 0) return;
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    entry->stride = desc.StructureByteStride;
    entry->usage = static_cast<std::uint32_t>(desc.Usage);
    entry->cpuAccessFlags = desc.CPUAccessFlags;
    entry->bindFlags = desc.BindFlags;
    entry->byteWidth = desc.ByteWidth;
}

// Stores a shader's resolved base-colour register. Called once per created
// pixel shader, off the render thread.
void DescribeShader(
    const std::uintptr_t handle, const std::uint32_t baseColorSlot) noexcept
{
    if (handle == 0) return;
    const std::size_t mask = s_describedShaders.size() - 1;
    std::size_t index = static_cast<std::size_t>(handle >> 4) & mask;
    for (std::size_t step = 0; step <= mask; ++step) {
        auto& entry = s_describedShaders[index];
        const auto existing = entry.handle.load(std::memory_order_acquire);
        if (existing == handle) return;
        if (existing == 0) {
            std::uintptr_t expected = 0;
            if (entry.handle.compare_exchange_strong(expected, handle,
                    std::memory_order_acq_rel)) {
                entry.baseColorSlot = baseColorSlot;
                s_shadersDescribed.fetch_add(1, std::memory_order_relaxed);
                if (baseColorSlot != kNoBaseColorSlot) {
                    s_shadersWithBaseColor.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
            if (expected == handle) return;
        }
        index = (index + 1) & mask;
    }
}

// The register this shader binds its base colour at, or kNoBaseColorSlot when
// the shader was never described or declares no material texture.
[[nodiscard]] std::uint32_t FindShaderBaseColorSlot(
    const std::uintptr_t handle) noexcept
{
    if (handle == 0) return kNoBaseColorSlot;
    const std::size_t mask = s_describedShaders.size() - 1;
    std::size_t index = static_cast<std::size_t>(handle >> 4) & mask;
    for (std::size_t step = 0; step <= mask; ++step) {
        const auto& entry = s_describedShaders[index];
        const auto existing = entry.handle.load(std::memory_order_acquire);
        if (existing == handle) return entry.baseColorSlot;
        if (existing == 0) return kNoBaseColorSlot;
        index = (index + 1) & mask;
    }
    return kNoBaseColorSlot;
}

// Whether this shader was ever described at all. Distinct from "described
// and has no base colour": a shader absent from the table is one whose
// bytecode carried no reflection to read.
[[nodiscard]] bool FindShaderDescribed(const std::uintptr_t handle) noexcept
{
    if (handle == 0) return false;
    const std::size_t mask = s_describedShaders.size() - 1;
    std::size_t index = static_cast<std::size_t>(handle >> 4) & mask;
    for (std::size_t step = 0; step <= mask; ++step) {
        const auto& entry = s_describedShaders[index];
        const auto existing = entry.handle.load(std::memory_order_acquire);
        if (existing == handle) return true;
        if (existing == 0) return false;
        index = (index + 1) & mask;
    }
    return false;
}

// The texture behind a bound view, cached so the COM call is paid once per
// distinct view rather than once per draw.
[[nodiscard]] std::uintptr_t ResolveViewResource(void* const view) noexcept
{
    if (view == nullptr) return 0;
    const auto key = reinterpret_cast<std::uintptr_t>(view);
    const std::size_t mask = s_resolvedViews.size() - 1;
    std::size_t index = static_cast<std::size_t>(key >> 4) & mask;
    for (std::size_t step = 0; step <= mask; ++step) {
        auto& entry = s_resolvedViews[index];
        const auto existing = entry.view.load(std::memory_order_acquire);
        if (existing == key) return entry.resource;
        if (existing == 0) {
            ID3D11Resource* resource = nullptr;
            static_cast<ID3D11ShaderResourceView*>(view)->GetResource(&resource);
            if (resource == nullptr) return 0;
            // Through IUnknown, not the ID3D11Resource pointer itself. COM
            // only guarantees that *IUnknown* is identical for a given object
            // across interfaces; a texture reached as ID3D11Resource here and
            // as ID3D11Texture2D at creation may be two different addresses.
            // The residency tracker keys on the same canonical identity, and
            // without this the two tables would never agree on a single
            // texture -- a lookup that silently finds nothing, forever.
            IUnknown* canonical = nullptr;
            const auto queried = resource->QueryInterface(
                __uuidof(IUnknown), reinterpret_cast<void**>(&canonical));
            resource->Release();
            if (FAILED(queried) || canonical == nullptr) return 0;
            const auto identity = reinterpret_cast<std::uintptr_t>(canonical);
            // Released immediately: the pointer is used as an identity, not
            // as a reference. Holding it would keep engine textures alive
            // past the engine's own idea of their lifetime.
            canonical->Release();
            std::uintptr_t expected = 0;
            if (entry.view.compare_exchange_strong(expected, key,
                    std::memory_order_acq_rel)) {
                entry.resource = identity;
                return identity;
            }
            if (expected == key) return entry.resource;
        }
        index = (index + 1) & mask;
    }
    return 0;
}

// The base-colour texture for a draw about to run on this thread, resolved
// through the shader that will shade it.
[[nodiscard]] std::uint64_t CurrentBaseColorTexture() noexcept
{
    // Three different reasons a draw resolves nothing, counted apart because
    // they call for three different responses. A shader that was never
    // described is one whose bytecode carried no reflection chunk -- Fallout 4
    // ships most of its own shaders stripped -- and no amount of work further
    // down this path will recover it. A shader that *was* described but
    // declares no material texture is a post or volumetric pass, which is
    // correct and expected. Only the third is a defect.
    if (t_currentPixelShader == 0) {
        s_drawsNoShader.fetch_add(1, std::memory_order_relaxed);
        if (t_pixelShaderEverSet) {
            // The engine asked for no pixel shader. Depth-only by construction.
            s_drawsShaderExplicitNull.fetch_add(1, std::memory_order_relaxed);
            if (depth::SceneDepthBound()) {
                s_drawsNullSceneDepth.fetch_add(1, std::memory_order_relaxed);
            } else {
                s_drawsNullOtherTarget.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            s_drawsShaderNeverSet.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }
    auto slot = FindShaderBaseColorSlot(t_currentPixelShader);
    auto byConvention = false;
    if (slot == kNoBaseColorSlot) {
        if (FindShaderDescribed(t_currentPixelShader)) {
            // Described, and it declares no material texture. This is a post
            // or volumetric pass and has no albedo to find; guessing one here
            // would attach a depth buffer to a material.
            s_drawsShaderNoBase.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        // The shader carried no reflection chunk. Measured live: 63% of the
        // engine's draws are these, because Fallout 4 ships its own shaders
        // stripped -- so refusing here would refuse essentially the whole
        // world, and the reflection route alone cannot texture the mirror.
        //
        // Register 0 is Bethesda's own convention for the diffuse map, and it
        // is what every reflected material shader here also does: `tex[0]` and
        // the scalar `tex` both bind at 0 across 294 of the 316 shaders that
        // declare one. That makes this a convention corroborated by every
        // shader that can be read, not a guess -- but it is still not the
        // shader's own word, so every draw resolved this way is counted
        // separately and never mixed into the measured total.
        s_drawsShaderUnknown.fetch_add(1, std::memory_order_relaxed);
        slot = 0;
        byConvention = true;
    }
    if (slot >= kTrackedPixelShaderSlots) {
        s_drawsMissingBaseColor.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const auto resource = ResolveViewResource(t_pixelShaderResources[slot]);
    if (resource == 0) {
        // The shader declares a base colour and nothing is bound where it
        // said. Counted rather than ignored: this is the shape a wrong slot
        // rule would take, and it has to be distinguishable from a technique
        // that simply has no albedo.
        s_drawsMissingBaseColor.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (byConvention) {
        s_drawsConventionBaseColor.fetch_add(1, std::memory_order_relaxed);
    } else {
        s_drawsWithBaseColor.fetch_add(1, std::memory_order_relaxed);
    }
    return static_cast<std::uint64_t>(resource);
}

// Records what was written into one pixel-shader constant buffer, tracking the
// per-word range across every write so a field that holds still for a frame can
// be told from one that changes per draw.
//
// Called from both write paths. A DYNAMIC buffer is written through Map and a
// DEFAULT one through UpdateSubresource, and a block observed on only one path
// reads as absent rather than as unwritten.
void SampleConstantWrite(
    const std::uintptr_t handle, const void* const data) noexcept
{
    auto* const found = FindPsConstant(handle);
    if (found != nullptr) {
        auto& entry = *found;
        entry.maps.fetch_add(1, std::memory_order_relaxed);
        // Wide enough to hold a direction, a colour and an ambient, and no
        // wider than the sample can carry. A per-draw material block is
        // smaller than this and a bone palette is far larger, so the window
        // is what separates the per-frame block from both.
        constexpr std::uint32_t kMinimumLightingBytes = 64;
        constexpr std::uint32_t kMaximumLightingBytes =
            sizeof(LightingSample::values);
        if (entry.byteWidth < kMinimumLightingBytes ||
            entry.byteWidth > kMaximumLightingBytes) {
            return;
        }

        const std::lock_guard<std::mutex> guard{s_lightingMutex};
        // One slot per distinct buffer. Keying by size instead put every
        // buffer of a given width into one slot, and the engine binds
        // several 752-byte pixel-shader blocks: their contents interleave,
        // every word's range spans two unrelated meanings, and nothing holds
        // still. A field that is constant for a frame can only be found by
        // watching one buffer at a time.
        for (auto& slot : s_lighting) {
            if (slot.valid && slot.handle != handle) {
                continue;
            }
            const auto* const written = static_cast<const float*>(data);
            const auto words = entry.byteWidth / 4u;
            if (!slot.valid) {
                for (std::uint32_t word = 0; word < words; ++word) {
                    slot.lowest[word] = written[word];
                    slot.highest[word] = written[word];
                }
            } else {
                for (std::uint32_t word = 0; word < words; ++word) {
                    slot.lowest[word] = slot.lowest[word] < written[word]
                        ? slot.lowest[word] : written[word];
                    slot.highest[word] = slot.highest[word] > written[word]
                        ? slot.highest[word] : written[word];
                }
            }
            slot.valid = true;
            slot.handle = static_cast<std::uint64_t>(handle);
            slot.byteWidth = entry.byteWidth;
            slot.maps = entry.maps.load(std::memory_order_relaxed);
            std::memcpy(slot.values, data, entry.byteWidth);
            break;
        }
        s_psSampled.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

// A recorded input layout: the elements the engine declared, stored by the
// layout's own address. Fixed capacity and written once per layout, because
// this runs on the render thread; a layout past capacity is counted rather
// than allocated for, and a draw using it is rejected as unknown instead of
// being decoded with somebody else's format.
struct RecordedLayout
{
    std::atomic<std::uintptr_t> handle{};
    std::atomic<bool> ready{};
    std::uint32_t elementCount{};
    std::array<renderer::mesh::InputElementDesc, kMaximumLayoutElements>
        elements{};
    // The semantic strings belong to the caller's memory, which is not
    // guaranteed to outlive the layout, so they are copied rather than
    // referenced.
    std::array<std::array<char, 24>, kMaximumLayoutElements> names{};
};

std::array<RecordedLayout, kMaximumRecordedLayouts> s_layouts{};
std::atomic<std::uint32_t> s_layoutOverflow{};
std::atomic<std::uint32_t> s_layoutsRecorded{};
std::atomic<std::uint64_t> s_layoutHits{};
std::atomic<std::uint64_t> s_layoutMisses{};
std::atomic<std::uint64_t> s_layoutUnbuildable{};

void RecordInputLayout(
    const std::uintptr_t handle,
    const D3D11_INPUT_ELEMENT_DESC* const elements,
    const UINT elementCount) noexcept
{
    if (handle == 0) return;
    for (auto& entry : s_layouts) {
        auto existing = entry.handle.load(std::memory_order_acquire);
        if (existing == handle) return;
        if (existing != 0) continue;
        std::uintptr_t expected = 0;
        if (!entry.handle.compare_exchange_strong(expected, handle,
                std::memory_order_acq_rel)) {
            continue;
        }
        const auto kept = std::min<UINT>(elementCount, kMaximumLayoutElements);
        for (UINT index = 0; index < kept; ++index) {
            const auto& source = elements[index];
            auto& name = entry.names[index];
            name = {};
            if (source.SemanticName != nullptr) {
                const auto length =
                    std::min<std::size_t>(std::strlen(source.SemanticName),
                        name.size() - 1);
                std::memcpy(name.data(), source.SemanticName, length);
            }
            auto& target = entry.elements[index];
            target.semanticName = std::string_view(name.data());
            target.semanticIndex = source.SemanticIndex;
            target.format = static_cast<std::uint32_t>(source.Format);
            target.inputSlot = source.InputSlot;
            target.alignedByteOffset = source.AlignedByteOffset;
        }
        entry.elementCount = kept;
        // Published last, so a concurrent reader either sees no layout or a
        // complete one, never a half-filled element array.
        entry.ready.store(true, std::memory_order_release);
        s_layoutsRecorded.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s_layoutOverflow.fetch_add(1, std::memory_order_relaxed);
}

void NoteVertexBuffer(const std::uintptr_t buffer) noexcept
{
    if (buffer == 0) return;
    // Multiplicative hash, then linear probing over a bounded window. The
    // window is what keeps the worst case constant on the render thread.
    auto slot = static_cast<std::size_t>(
        (buffer * 0x9E3779B97F4A7C15ull) >> 52) % kBufferTableSize;
    for (std::size_t probe = 0; probe < 8; ++probe) {
        const auto index = (slot + probe) % kBufferTableSize;
        auto existing = s_bufferTable[index].load(std::memory_order_relaxed);
        if (existing == buffer) return;
        if (existing == 0) {
            std::uintptr_t expected = 0;
            if (s_bufferTable[index].compare_exchange_strong(
                    expected, buffer, std::memory_order_relaxed)) {
                s_distinctBuffers.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (expected == buffer) return;
        }
    }
    s_overflowed.fetch_add(1, std::memory_order_relaxed);
}

void RecordDraw(
    const UINT indexCount,
    const UINT startIndex,
    const INT baseVertex,
    const UINT instanceCount) noexcept
{
    auto slot = s_arenaCount.load(std::memory_order_relaxed);
    if (slot >= kDrawArenaCapacity ||
        !s_arenaCount.compare_exchange_strong(slot, slot + 1,
            std::memory_order_relaxed)) {
        s_arenaDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& record = s_arena[slot];
    record.vertexBuffer = static_cast<std::uint64_t>(t_currentVertexBuffer);
    record.indexBuffer = static_cast<std::uint64_t>(t_currentIndexBuffer);
    record.vertexStride = t_currentVertexStride;
    record.vertexByteOffset = t_currentVertexOffset;
    record.inputLayout = static_cast<std::uint64_t>(t_currentInputLayout);
    record.indexCount = indexCount;
    record.startIndex = startIndex;
    record.baseVertex = baseVertex;
    record.instanceCount = instanceCount;
    record.indexFormat = t_currentIndexFormat;
    record.indexOffset = t_currentIndexOffset;
    record.hasTransform = t_transformFresh;
    record.baseColorTexture = CurrentBaseColorTexture();
    record.frontCounterClockwise = t_frontCounterClockwise;
    record.cullMode = t_cullMode;
    if (t_cullMode == renderer::drawstream::kCullModeUnknown) {
        s_drawsUnknownCull.fetch_add(1, std::memory_order_relaxed);
    } else {
        s_drawsKnownCull.fetch_add(1, std::memory_order_relaxed);
    }
    if (t_frontCounterClockwise) {
        s_drawsFrontCcw.fetch_add(1, std::memory_order_relaxed);
    } else {
        s_drawsFrontCw.fetch_add(1, std::memory_order_relaxed);
    }
    t_transformFresh = false;
    std::memcpy(record.model, t_lastTransform, sizeof(record.model));
}

void NoteDraw(const UINT indexCount, const bool instanced) noexcept
{
    if (instanced) {
        s_instancedDrawCalls.fetch_add(1, std::memory_order_relaxed);
    } else {
        s_drawCalls.fetch_add(1, std::memory_order_relaxed);
    }
    s_indices.fetch_add(indexCount, std::memory_order_relaxed);
    auto largest = s_largestIndexCount.load(std::memory_order_relaxed);
    while (indexCount > largest &&
           !s_largestIndexCount.compare_exchange_weak(
               largest, indexCount, std::memory_order_relaxed)) {
    }
    NoteVertexBuffer(t_currentVertexBuffer);
    Describe(s_describedVertex, t_currentVertexBufferObject,
        t_currentVertexStride, true);
}

// Whether this draw is Vulkan's rather than vanilla's.
//
// Called for every draw, so it does no work beyond three relaxed loads and the
// depth classification's thread-local read. Recording happens first and
// unconditionally: the mirror is built out of these draws, so a suppressed
// frame has to observe exactly what an unsuppressed one does or suppression
// would starve the renderer that replaces it.
bool SuppressThisDraw() noexcept
{
    if (!world_suppression::Enabled()) return false;
    renderer::suppression::DrawContext context{};
    context.permitGrants = world_suppression::PermitGrants();
    context.writesWorldTarget = depth::SceneDepthBound();
    context.worldReproduced = world_suppression::WorldReproduced();
    const auto disposition = renderer::suppression::ClassifyDraw(context);
    if (disposition == renderer::suppression::DrawDisposition::Suppressed) {
        world_suppression::NoteSuppressed();
        return true;
    }
    world_suppression::NoteForwarded();
    return false;
}

void __stdcall HookedDrawIndexed(
    void* self,
    const UINT indexCount,
    const UINT startIndex,
    const INT baseVertex) noexcept
{
    NoteDraw(indexCount, false);
    RecordDraw(indexCount, startIndex, baseVertex, 1);
    if (SuppressThisDraw()) return;
    s_origDrawIndexed(self, indexCount, startIndex, baseVertex);
}

void __stdcall HookedDrawIndexedInstanced(
    void* self,
    const UINT indexCountPerInstance,
    const UINT instanceCount,
    const UINT startIndex,
    const INT baseVertex,
    const UINT startInstance) noexcept
{
    NoteDraw(indexCountPerInstance, true);
    RecordDraw(indexCountPerInstance, startIndex, baseVertex, instanceCount);
    if (SuppressThisDraw()) return;
    s_origDrawIndexedInstanced(self, indexCountPerInstance, instanceCount,
        startIndex, baseVertex, startInstance);
}

void __stdcall HookedSetVertexBuffers(
    void* self,
    const UINT startSlot,
    const UINT bufferCount,
    ID3D11Buffer* const* buffers,
    const UINT* strides,
    const UINT* offsets) noexcept
{
    // Slot 0 is the geometry stream; later slots carry instance data, which
    // is not what identifies a mesh.
    if (startSlot == 0 && bufferCount > 0 && buffers != nullptr) {
        t_currentVertexBuffer = reinterpret_cast<std::uintptr_t>(buffers[0]);
        t_currentVertexBufferObject = buffers[0];
        t_currentVertexStride = strides != nullptr ? strides[0] : 0;
        // The offset is the mesh's address inside the pool. Passing it
        // through to the original and not recording it reads every mesh from
        // the pool's base instead, which returns whatever geometry happens to
        // live there: real triangles, drawn from the wrong object.
        t_currentVertexOffset = offsets != nullptr ? offsets[0] : 0;
    }
    s_origSetVertexBuffers(self, startSlot, bufferCount, buffers, strides,
        offsets);
}

std::array<ConstantSample, kMaximumConstantSamples> s_samples{};
std::atomic<std::uint32_t> s_sampleCount{};
thread_local void* t_mappedData = nullptr;
thread_local std::uintptr_t t_unmappedBuffer = 0;
thread_local std::uint32_t t_mappedWidth = 0;

using UnmapFn = void(__stdcall*)(void*, ID3D11Resource*, UINT);
UnmapFn s_origUnmap = nullptr;
void* s_unmapAddress = nullptr;

void __stdcall HookedUnmap(
    void* self,
    ID3D11Resource* resource,
    const UINT subresource) noexcept
{
    // Read before the original commits: the pointer is only valid until then.
    if (t_mappedData != nullptr) {
        auto index = s_sampleCount.load(std::memory_order_relaxed);
        if (index < kMaximumConstantSamples &&
            s_sampleCount.compare_exchange_strong(index, index + 1,
                std::memory_order_relaxed)) {
            const auto bytes = t_mappedWidth < sizeof(s_samples[0].values)
                ? t_mappedWidth
                : static_cast<std::uint32_t>(sizeof(s_samples[0].values));
            s_samples[index].byteWidth = t_mappedWidth;
            s_samples[index].bytes = bytes;
            std::memcpy(s_samples[index].values, t_mappedData, bytes);
        }
        // The first 64 bytes are the row-major world matrix, measured live.
        // Copied on every unmap, not only the sampled ones, because every
        // draw after this one takes its placement from here.
        if (t_mappedWidth >= 64) {
            std::memcpy(t_lastTransform, t_mappedData, sizeof(t_lastTransform));
            t_transformFresh = true;
        }
        t_mappedData = nullptr;
    }
    s_origUnmap(self, resource, subresource);
}

HRESULT __stdcall HookedMap(
    void* self,
    ID3D11Resource* resource,
    const UINT subresource,
    const D3D11_MAP mapType,
    const UINT flags,
    D3D11_MAPPED_SUBRESOURCE* mapped) noexcept
{
    const auto result =
        s_origMap(self, resource, subresource, mapType, flags, mapped);
    // Counted only against buffers already described, so a map never costs a
    // GetDesc on the hot path.
    t_mappedData = nullptr;
    if (resource != nullptr && SUCCEEDED(result) && mapped != nullptr) {
        const auto handle = reinterpret_cast<std::uintptr_t>(resource);
        for (auto& entry : s_describedConstant) {
            if (entry.handle.load(std::memory_order_acquire) != handle) {
                continue;
            }
            entry.maps.fetch_add(1, std::memory_order_relaxed);
            // Only the small, per-draw-sized buffers are sampled. A
            // 65 KB batch array is a different question and a different
            // measurement.
            // Not gated on the sample budget: the sample is a one-off
            // diagnostic, but the transform copy has to happen on every
            // unmap or every draw after the eighth would be placed by a
            // stale matrix.
            if (entry.byteWidth <= 128) {
                t_mappedData = mapped->pData;
                t_mappedWidth = entry.byteWidth;
                t_unmappedBuffer = handle;
            }
            break;
        }
    }
    // The pixel-shader constants, sampled separately. A per-frame lighting
    // block is far larger than a per-draw transform, so it is taken by its own
    // rule rather than by the one above -- which exists to find a matrix and
    // deliberately ignores anything wider.
    if (resource != nullptr && SUCCEEDED(result) && mapped != nullptr &&
        mapped->pData != nullptr) {
        SampleConstantWrite(
            reinterpret_cast<std::uintptr_t>(resource), mapped->pData);
    }
    return result;
}

void __stdcall HookedSetVsConstantBuffers(
    void* self,
    const UINT startSlot,
    const UINT bufferCount,
    ID3D11Buffer* const* buffers) noexcept
{
    // The per-draw world transform lives in one of these. Describing them is
    // the measurement that says which slot is small enough to be per-object
    // rather than per-frame or per-pass.
    if (buffers != nullptr) {
        for (UINT index = 0; index < bufferCount && index < 4; ++index) {
            Describe(s_describedConstant, buffers[index], 0, false);
        }
        if (startSlot == 0 && bufferCount > 0) {
            t_vsConstantSlot0 =
                reinterpret_cast<std::uintptr_t>(buffers[0]);
        }
    }
    s_origSetVsConstantBuffers(self, startSlot, bufferCount, buffers);
}

// The engine's pixel-shader constants. The per-frame lighting block is here:
// the sun's direction and colour, the ambient, and the fog the sky publishes.
// Described rather than assumed, because which slot carries it and how wide it
// is are measurements, and an offset guessed from a screenshot would light the
// scene by numbers nobody checked.
// The second way a constant buffer is written. Map/Unmap only covers buffers
// created DYNAMIC; a DEFAULT-usage buffer cannot be mapped at all and is
// updated through this call instead. Hooking one and not the other measures
// whichever half the engine happens to use for a given block, and reports the
// other half as absent.
void __stdcall HookedUpdateSubresource(
    void* self,
    ID3D11Resource* resource,
    const UINT subresource,
    const D3D11_BOX* box,
    const void* data,
    const UINT rowPitch,
    const UINT depthPitch) noexcept
{
    s_origUpdateSubresource(self, resource, subresource, box, data, rowPitch,
        depthPitch);
    if (resource == nullptr || data == nullptr || subresource != 0 ||
        box != nullptr) {
        return;
    }
    SampleConstantWrite(reinterpret_cast<std::uintptr_t>(resource), data);
}

void __stdcall HookedSetPsConstantBuffers(
    void* self,
    const UINT startSlot,
    const UINT bufferCount,
    ID3D11Buffer* const* buffers) noexcept
{
    if (buffers != nullptr) {
        s_psBinds.fetch_add(1, std::memory_order_relaxed);
        for (UINT index = 0; index < bufferCount && index < 8; ++index) {
            DescribePsConstant(buffers[index]);
        }
    }
    s_origSetPsConstantBuffers(self, startSlot, bufferCount, buffers);
}

void __stdcall HookedSetIndexBuffer(
    void* self,
    void* buffer,
    const UINT format,
    const UINT offset) noexcept
{
    t_currentIndexBuffer = reinterpret_cast<std::uintptr_t>(buffer);
    t_currentIndexFormat = format;
    t_currentIndexOffset = offset;
    s_origSetIndexBuffer(self, buffer, format, offset);
}

// The engine's own declaration of its vertex streams, recorded when the layout
// is created because D3D11 offers no way to read the elements back off an
// ID3D11InputLayout afterwards. Without this the only thing a draw hook knows
// about a vertex is its stride, and a stride wide enough for three floats is
// equally wide enough for four halves plus a pair: the two decode to
// completely different geometry.
HRESULT __stdcall HookedCreateInputLayout(
    void* self,
    const D3D11_INPUT_ELEMENT_DESC* elements,
    const UINT elementCount,
    const void* shaderBytecode,
    const SIZE_T bytecodeLength,
    ID3D11InputLayout** layout) noexcept
{
    const auto result = s_origCreateInputLayout(self, elements, elementCount,
        shaderBytecode, bytecodeLength, layout);
    if (FAILED(result) || layout == nullptr || *layout == nullptr ||
        elements == nullptr) {
        return result;
    }
    RecordInputLayout(reinterpret_cast<std::uintptr_t>(*layout), elements,
        elementCount);
    return result;
}

void __stdcall HookedSetInputLayout(void* self, void* layout) noexcept
{
    t_currentInputLayout = reinterpret_cast<std::uintptr_t>(layout);
    s_origSetInputLayout(self, layout);
}

HRESULT __stdcall HookedCreateRasterizerState(
    void* self,
    const D3D11_RASTERIZER_DESC* desc,
    ID3D11RasterizerState** state) noexcept
{
    const auto result = s_origCreateRasterizerState(self, desc, state);
    // Recorded only once the object exists, so the table never names an
    // address the runtime did not return.
    if (FAILED(result) || desc == nullptr || state == nullptr ||
        *state == nullptr) {
        return result;
    }
    auto slot = s_rasterizerStateCount.load(std::memory_order_relaxed);
    if (slot >= kMaximumRasterizerStates ||
        !s_rasterizerStateCount.compare_exchange_strong(slot, slot + 1,
            std::memory_order_relaxed)) {
        s_rasterizerStateOverflow.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    auto& entry = s_rasterizerStates[slot];
    entry.frontCounterClockwise = desc->FrontCounterClockwise != FALSE;
    entry.cullMode = static_cast<std::uint32_t>(desc->CullMode);
    // Published last: a reader that sees the address sees the fields.
    entry.state.store(reinterpret_cast<std::uintptr_t>(*state),
        std::memory_order_release);
    return result;
}

void __stdcall HookedSetRasterizerState(
    void* self,
    ID3D11RasterizerState* state) noexcept
{
    // A null state restores D3D11's default, which is back-face culling with
    // clockwise front faces -- not "no culling", and not "keep the last one".
    if (state == nullptr) {
        t_frontCounterClockwise = false;
        t_cullMode = renderer::drawstream::kCullModeBack;
    } else {
        const auto handle = reinterpret_cast<std::uintptr_t>(state);
        auto found = false;
        const auto count = std::min<std::size_t>(
            s_rasterizerStateCount.load(std::memory_order_relaxed),
            kMaximumRasterizerStates);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& entry = s_rasterizerStates[index];
            if (entry.state.load(std::memory_order_acquire) != handle) continue;
            t_frontCounterClockwise = entry.frontCounterClockwise;
            t_cullMode = entry.cullMode;
            found = true;
            break;
        }
        if (!found) {
            // Created before the hook was installed. Left unknown rather than
            // guessed, so the assembler can tell "no state seen" from "state
            // seen and it said clockwise".
            t_frontCounterClockwise = false;
            t_cullMode = renderer::drawstream::kCullModeUnknown;
            s_rasterizerBindsUnmatched.fetch_add(1, std::memory_order_relaxed);
        }
    }
    s_rasterizerBinds.fetch_add(1, std::memory_order_relaxed);
    s_origSetRasterizerState(self, state);
}

// Per thread, for the same reason the vertex buffer is: a draw shades with
// whatever shader is bound when it runs, and the engine submits from more
// than one thread.
void __stdcall HookedSetPixelShader(
    void* self,
    ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* instances,
    const UINT instanceCount) noexcept
{
    t_currentPixelShader = reinterpret_cast<std::uintptr_t>(shader);
    // Set even when the shader is null, which is what separates "the engine
    // bound nothing" from "this thread was never observed binding anything".
    t_pixelShaderEverSet = true;
    s_psShaderBinds.fetch_add(1, std::memory_order_relaxed);
    s_origSetPixelShader(self, shader, instances, instanceCount);
}

}

bool FindInputLayout(
    const std::uint64_t handle,
    const std::uint32_t stride,
    renderer::mesh::EngineVertexLayout& layout) noexcept
{
    layout = {};
    if (handle == 0 || stride == 0) return false;
    for (const auto& entry : s_layouts) {
        if (entry.handle.load(std::memory_order_acquire) !=
            static_cast<std::uintptr_t>(handle)) {
            continue;
        }
        // Published after the elements were filled in, so a reader either sees
        // a complete layout or none.
        if (!entry.ready.load(std::memory_order_acquire)) return false;
        const auto elements = std::span<const renderer::mesh::InputElementDesc>(
            entry.elements.data(), entry.elementCount);
        // Slot zero: the geometry stream. Later slots carry instance data,
        // whose offsets are into a different buffer entirely.
        const auto built = renderer::mesh::BuildLayoutFromInputElements(
            elements, stride, 0, layout) ==
            renderer::mesh::VertexLayoutError::None;
        // Counted apart from a miss. "The layout was never seen" and "it was
        // seen and could not be built for this stride" have different causes
        // and different fixes, and one number cannot distinguish them.
        if (built) {
            s_layoutHits.fetch_add(1, std::memory_order_relaxed);
        } else {
            s_layoutUnbuildable.fetch_add(1, std::memory_order_relaxed);
        }
        return built;
    }
    s_layoutMisses.fetch_add(1, std::memory_order_relaxed);
    return false;
}

LayoutStats LayoutCounters() noexcept
{
    LayoutStats stats{};
    stats.recorded = s_layoutsRecorded.load(std::memory_order_relaxed);
    stats.overflow = s_layoutOverflow.load(std::memory_order_relaxed);
    stats.hits = s_layoutHits.load(std::memory_order_relaxed);
    stats.misses = s_layoutMisses.load(std::memory_order_relaxed);
    stats.unbuildable = s_layoutUnbuildable.load(std::memory_order_relaxed);
    return stats;
}

std::uint32_t LayoutOverflowCount() noexcept
{
    return s_layoutOverflow.load(std::memory_order_relaxed);
}

RasterizerCounters RasterizerState() noexcept
{
    RasterizerCounters counters{};
    counters.statesDescribed =
        s_rasterizerStateCount.load(std::memory_order_relaxed);
    counters.binds = s_rasterizerBinds.load(std::memory_order_relaxed);
    counters.bindsUnmatched =
        s_rasterizerBindsUnmatched.load(std::memory_order_relaxed);
    counters.drawsKnownCull =
        s_drawsKnownCull.load(std::memory_order_relaxed);
    counters.drawsUnknownCull =
        s_drawsUnknownCull.load(std::memory_order_relaxed);
    counters.drawsFrontCcw = s_drawsFrontCcw.load(std::memory_order_relaxed);
    counters.drawsFrontCw = s_drawsFrontCw.load(std::memory_order_relaxed);
    counters.stateOverflow =
        s_rasterizerStateOverflow.load(std::memory_order_relaxed);
    return counters;
}

bool PrepareHooks(
    void* const drawIndexed,
    void* const drawIndexedInstanced,
    void* const setVertexBuffers,
    void* const setIndexBuffer,
    void* const setVsConstantBuffers,
    void* const map,
    void* const unmap,
    void* const setInputLayout,
    void* const createInputLayout,
    void* const setPsConstantBuffers,
    void* const createPixelShader,
    void* const updateSubresource,
    void* const setPixelShader,
    void* const createRasterizerState,
    void* const setRasterizerState) noexcept
{
    s_createRasterizerStateAddress = createRasterizerState;
    s_setRasterizerStateAddress = setRasterizerState;
    s_updateSubresourceAddress = updateSubresource;
    s_setPixelShaderAddress = setPixelShader;
    s_setInputLayoutAddress = setInputLayout;
    s_createInputLayoutAddress = createInputLayout;
    s_createPixelShaderAddress = createPixelShader;
    s_mapAddress = map;
    s_unmapAddress = unmap;
    if (drawIndexed == nullptr || drawIndexedInstanced == nullptr ||
        setVertexBuffers == nullptr || setIndexBuffer == nullptr ||
        setVsConstantBuffers == nullptr || map == nullptr ||
        unmap == nullptr) {
        return false;
    }
    s_drawIndexedAddress = drawIndexed;
    s_drawIndexedInstancedAddress = drawIndexedInstanced;
    s_setVertexBuffersAddress = setVertexBuffers;
    s_setIndexBufferAddress = setIndexBuffer;
    s_setVsConstantBuffersAddress = setVsConstantBuffers;
    s_setPsConstantBuffersAddress = setPsConstantBuffers;
    return true;
}

std::size_t CopyLightingSamples(
    LightingSample* const destination,
    const std::size_t capacity) noexcept
{
    if (destination == nullptr) return 0;
    const std::lock_guard<std::mutex> guard{s_lightingMutex};
    std::size_t written = 0;
    for (const auto& slot : s_lighting) {
        if (!slot.valid || written >= capacity) continue;
        destination[written++] = slot;
    }
    return written;
}

std::size_t CopyShaderBufferLayouts(
    ShaderBufferLayout* const destination,
    const std::size_t capacity) noexcept
{
    if (destination == nullptr) return 0;
    const std::lock_guard<std::mutex> guard{s_reflectionMutex};
    const std::size_t written =
        std::min(capacity, static_cast<std::size_t>(s_shaderBufferCount));
    for (std::size_t index = 0; index < written; ++index) {
        destination[index] = s_shaderBuffers[index];
    }
    return written;
}

void NotePixelShaderResources(
    const std::uint32_t startSlot,
    const std::uint32_t count,
    void* const* const views) noexcept
{
    s_psResourceNotices.fetch_add(1, std::memory_order_relaxed);
    if (views == nullptr) {
        // A null array unbinds the whole range. Clearing rather than leaving
        // the previous views in place: a draw that follows must resolve to
        // "nothing bound", not to whatever the last technique left there.
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto slot = startSlot + index;
            if (slot >= kTrackedPixelShaderSlots) break;
            t_pixelShaderResources[slot] = nullptr;
        }
        return;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto slot = startSlot + index;
        if (slot >= kTrackedPixelShaderSlots) break;
        t_pixelShaderResources[slot] = views[index];
    }
}

std::size_t CopyShaderResourceBindings(
    ShaderResourceBinding* const destination,
    const std::size_t capacity) noexcept
{
    if (destination == nullptr) return 0;
    const std::lock_guard<std::mutex> guard{s_reflectionMutex};
    const std::size_t written =
        std::min(capacity, static_cast<std::size_t>(s_shaderResourceCount));
    for (std::size_t index = 0; index < written; ++index) {
        destination[index] = s_shaderResources[index];
    }
    return written;
}

ShaderReflectionStats ShaderReflectionCounters() noexcept
{
    ShaderReflectionStats stats{};
    stats.shaders = s_shadersSeen.load(std::memory_order_relaxed);
    stats.reflected = s_shadersReflected.load(std::memory_order_relaxed);
    stats.failed = s_shadersFailed.load(std::memory_order_relaxed);
    stats.layoutOverflow = s_reflectLayoutOverflow.load(std::memory_order_relaxed);
    stats.fieldOverflow = s_fieldOverflow.load(std::memory_order_relaxed);
    stats.resourceOverflow = s_shaderResourceOverflow.load(std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> guard{s_reflectionMutex};
        stats.layouts = s_shaderBufferCount;
        stats.resources = s_shaderResourceCount;
    }
    return stats;
}

ConstantStats ConstantCounters() noexcept
{
    ConstantStats stats{};
    stats.psBinds = s_psBinds.load(std::memory_order_relaxed);
    stats.psDescribeOverflow = s_psDescribeOverflow.load(std::memory_order_relaxed);
    stats.psResourceNotices = s_psResourceNotices.load(std::memory_order_relaxed);
    stats.drawsWithBaseColor = s_drawsWithBaseColor.load(std::memory_order_relaxed);
    stats.drawsMissingBaseColor = s_drawsMissingBaseColor.load(std::memory_order_relaxed);
    stats.drawsNoShader = s_drawsNoShader.load(std::memory_order_relaxed);
    stats.drawsShaderExplicitNull =
        s_drawsShaderExplicitNull.load(std::memory_order_relaxed);
    stats.drawsShaderNeverSet =
        s_drawsShaderNeverSet.load(std::memory_order_relaxed);
    stats.drawsNullSceneDepth =
        s_drawsNullSceneDepth.load(std::memory_order_relaxed);
    stats.drawsNullOtherTarget =
        s_drawsNullOtherTarget.load(std::memory_order_relaxed);
    stats.drawsShaderUnknown = s_drawsShaderUnknown.load(std::memory_order_relaxed);
    stats.drawsShaderNoBase = s_drawsShaderNoBase.load(std::memory_order_relaxed);
    stats.drawsConventionBaseColor = s_drawsConventionBaseColor.load(std::memory_order_relaxed);
    stats.psShaderBinds = s_psShaderBinds.load(std::memory_order_relaxed);
    stats.shadersDescribed = s_shadersDescribed.load(std::memory_order_relaxed);
    stats.shadersWithBaseColor = s_shadersWithBaseColor.load(std::memory_order_relaxed);
    stats.psSampled = s_psSampled.load(std::memory_order_relaxed);
    for (const auto& entry : s_describedPsConstant) {
        if (entry.handle.load(std::memory_order_acquire) != 0) {
            ++stats.psDescribed;
        }
    }
    return stats;
}

bool Install() noexcept
{
    if (s_enabled) return true;
    if (s_drawIndexedAddress == nullptr) return false;
    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"VISUALFORGE_DRAW_CAPTURE", flag,
            static_cast<DWORD>(std::size(flag))) == 0) {
        return false;
    }
    // The layout pair is required, not optional. Without them a draw knows
    // only its stride, and a stride wide enough for three floats is equally
    // wide enough for four halves plus a pair. Failing to install is better
    // than capturing draws nothing can decode.
    if (s_setInputLayoutAddress == nullptr ||
        s_createInputLayoutAddress == nullptr) {
        return false;
    }
    // Required for the same reason as the layout pair. Without the shader's
    // own declaration of its constant buffers, an offset into one of them is
    // a guess: the engine binds one wide block across techniques that each
    // read it differently, so the numbers alone do not identify a field.
    if (s_createPixelShaderAddress == nullptr ||
        s_updateSubresourceAddress == nullptr ||
        s_setPixelShaderAddress == nullptr ||
        // Required, not optional: without the engine.s own rasterizer state
        // the stream has to assume a winding, and assuming it renders every
        // model inside out.
        s_createRasterizerStateAddress == nullptr ||
        s_setRasterizerStateAddress == nullptr) {
        return false;
    }
    if (MH_CreateHook(s_createInputLayoutAddress,
            reinterpret_cast<void*>(&HookedCreateInputLayout),
            reinterpret_cast<void**>(&s_origCreateInputLayout)) != MH_OK ||
        MH_CreateHook(s_createPixelShaderAddress,
            reinterpret_cast<void*>(&HookedCreatePixelShader),
            reinterpret_cast<void**>(&s_origCreatePixelShader)) != MH_OK ||
        MH_CreateHook(s_updateSubresourceAddress,
            reinterpret_cast<void*>(&HookedUpdateSubresource),
            reinterpret_cast<void**>(&s_origUpdateSubresource)) != MH_OK ||
        MH_CreateHook(s_setPixelShaderAddress,
            reinterpret_cast<void*>(&HookedSetPixelShader),
            reinterpret_cast<void**>(&s_origSetPixelShader)) != MH_OK ||
        MH_CreateHook(s_createRasterizerStateAddress,
            reinterpret_cast<void*>(&HookedCreateRasterizerState),
            reinterpret_cast<void**>(&s_origCreateRasterizerState)) != MH_OK ||
        MH_CreateHook(s_setRasterizerStateAddress,
            reinterpret_cast<void*>(&HookedSetRasterizerState),
            reinterpret_cast<void**>(&s_origSetRasterizerState)) != MH_OK ||
        MH_CreateHook(s_setInputLayoutAddress,
            reinterpret_cast<void*>(&HookedSetInputLayout),
            reinterpret_cast<void**>(&s_origSetInputLayout)) != MH_OK ||
        MH_CreateHook(s_drawIndexedAddress,
            reinterpret_cast<void*>(&HookedDrawIndexed),
            reinterpret_cast<void**>(&s_origDrawIndexed)) != MH_OK ||
        MH_CreateHook(s_drawIndexedInstancedAddress,
            reinterpret_cast<void*>(&HookedDrawIndexedInstanced),
            reinterpret_cast<void**>(&s_origDrawIndexedInstanced)) != MH_OK ||
        MH_CreateHook(s_setVertexBuffersAddress,
            reinterpret_cast<void*>(&HookedSetVertexBuffers),
            reinterpret_cast<void**>(&s_origSetVertexBuffers)) != MH_OK ||
        MH_CreateHook(s_setIndexBufferAddress,
            reinterpret_cast<void*>(&HookedSetIndexBuffer),
            reinterpret_cast<void**>(&s_origSetIndexBuffer)) != MH_OK ||
        MH_CreateHook(s_setVsConstantBuffersAddress,
            reinterpret_cast<void*>(&HookedSetVsConstantBuffers),
            reinterpret_cast<void**>(&s_origSetVsConstantBuffers)) != MH_OK ||
        MH_CreateHook(s_setPsConstantBuffersAddress,
            reinterpret_cast<void*>(&HookedSetPsConstantBuffers),
            reinterpret_cast<void**>(&s_origSetPsConstantBuffers)) != MH_OK ||
        MH_CreateHook(s_mapAddress,
            reinterpret_cast<void*>(&HookedMap),
            reinterpret_cast<void**>(&s_origMap)) != MH_OK ||
        MH_CreateHook(s_unmapAddress,
            reinterpret_cast<void*>(&HookedUnmap),
            reinterpret_cast<void**>(&s_origUnmap)) != MH_OK) {
        log::Write("draw-capture: MH_CreateHook failed");
        return false;
    }
    if (MH_EnableHook(s_createPixelShaderAddress) != MH_OK ||
        MH_EnableHook(s_updateSubresourceAddress) != MH_OK ||
        MH_EnableHook(s_setPixelShaderAddress) != MH_OK ||
        MH_EnableHook(s_createRasterizerStateAddress) != MH_OK ||
        MH_EnableHook(s_setRasterizerStateAddress) != MH_OK ||
        MH_EnableHook(s_drawIndexedAddress) != MH_OK ||
        MH_EnableHook(s_drawIndexedInstancedAddress) != MH_OK ||
        MH_EnableHook(s_setVertexBuffersAddress) != MH_OK ||
        MH_EnableHook(s_setIndexBufferAddress) != MH_OK ||
        MH_EnableHook(s_setVsConstantBuffersAddress) != MH_OK ||
        MH_EnableHook(s_setPsConstantBuffersAddress) != MH_OK ||
        MH_EnableHook(s_mapAddress) != MH_OK ||
        MH_EnableHook(s_unmapAddress) != MH_OK) {
        log::Write("draw-capture: MH_EnableHook failed");
        return false;
    }
    s_enabled = true;
    log::Write("draw-capture: installed");
    return true;
}

bool Enabled() noexcept
{
    return s_enabled;
}

namespace {

std::size_t CopyReports(
    std::span<const DescribedBuffer> table,
    BufferReport* const destination,
    const std::size_t capacity) noexcept
{
    std::size_t written = 0;
    for (const auto& entry : table) {
        if (written >= capacity) break;
        const auto handle = entry.handle.load(std::memory_order_acquire);
        if (handle == 0) continue;
        destination[written].handle = static_cast<std::uint64_t>(handle);
        destination[written].byteWidth = entry.byteWidth;
        destination[written].stride = entry.stride;
        destination[written].usage = entry.usage;
        destination[written].cpuAccessFlags = entry.cpuAccessFlags;
        destination[written].bindFlags = entry.bindFlags;
        destination[written].draws =
            entry.draws.load(std::memory_order_relaxed);
        destination[written].maps =
            entry.maps.load(std::memory_order_relaxed);
        ++written;
    }
    return written;
}

}

std::size_t CollectDraws(
    renderer::drawstream::DrawRecordV1* const destination,
    const std::size_t capacity,
    std::uint64_t& dropped) noexcept
{
    dropped = s_arenaDropped.exchange(0, std::memory_order_relaxed);
    const auto recorded = s_arenaCount.exchange(0, std::memory_order_relaxed);
    if (destination == nullptr) return 0;
    const auto available = static_cast<std::size_t>(recorded);
    const auto count = available < capacity ? available : capacity;
    if (count < available) dropped += available - count;
    for (std::size_t index = 0; index < count; ++index) {
        destination[index] = s_arena[index];
    }
    return count;
}

std::size_t CollectConstantSamples(
    ConstantSample* const destination,
    const std::size_t capacity) noexcept
{
    if (destination == nullptr) return 0;
    const auto available = s_sampleCount.load(std::memory_order_relaxed);
    const auto count = available < capacity ? available : capacity;
    for (std::size_t index = 0; index < count; ++index) {
        destination[index] = s_samples[index];
    }
    return count;
}

std::size_t DescribeVertexBuffers(
    BufferReport* const destination,
    const std::size_t capacity) noexcept
{
    if (destination == nullptr) return 0;
    return CopyReports(s_describedVertex, destination, capacity);
}

std::size_t CopyPsConstantReports(
    BufferReport* const destination,
    const std::size_t capacity) noexcept
{
    return CopyReports(s_describedPsConstant, destination, capacity);
}

std::size_t DescribeConstantBuffers(
    BufferReport* const destination,
    const std::size_t capacity) noexcept
{
    if (destination == nullptr) return 0;
    return CopyReports(s_describedConstant, destination, capacity);
}

FrameSummary EndFrame() noexcept
{
    FrameSummary summary{};
    summary.drawCalls = s_drawCalls.exchange(0, std::memory_order_relaxed);
    summary.instancedDrawCalls =
        s_instancedDrawCalls.exchange(0, std::memory_order_relaxed);
    summary.indices = s_indices.exchange(0, std::memory_order_relaxed);
    summary.largestIndexCount =
        s_largestIndexCount.exchange(0, std::memory_order_relaxed);
    summary.distinctVertexBuffers =
        s_distinctBuffers.exchange(0, std::memory_order_relaxed);
    summary.overflowedVertexBuffers =
        s_overflowed.exchange(0, std::memory_order_relaxed);
    for (auto& entry : s_bufferTable) {
        entry.store(0, std::memory_order_relaxed);
    }
    return summary;
}

}
