#include "EngineMeshCapture.h"

#include "Config.h"
#include "Log.h"
#include "renderer_core/EngineMesh.h"
#include "renderer_core/EngineVertex.h"
#include "renderer_core/ResourceRegistry.h"
#include "renderer_host/ResourceManagerContract.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace vf::engine_mesh_capture {

namespace {

using namespace vf::renderer;
using namespace vf::renderer::mesh;
using namespace vf::renderer::resource;

constexpr std::size_t kMaximumVertexBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumSnapshotBytes = 64 * 1024 * 1024;
constexpr std::uint32_t kMaximumIndices = 3 * 1024 * 1024;
constexpr std::size_t kMaximumSnapshots = 64;

using CreateTriShapeRendererDataFn = void* (__fastcall *)(
    void*, void*, std::uint64_t, std::uint16_t*, std::uint32_t);
using DecRefTriShapeFn = void (__fastcall *)(void*, void*);
using CreateVertexBufferFn = void* (__fastcall *)(
    void*, std::uint32_t*, void*, std::uint32_t, std::uint64_t);
using DecRefVertexBufferFn = void (__fastcall *)(void*, void*);
using ConcreteCreateTriShapeFn = void* (__fastcall *)(
    void*, std::uint32_t*, void*, std::uint64_t, void*);

struct VertexSnapshot
{
    ResourceHandle handle{};
    std::uint64_t vertexDesc{};
    std::uint32_t stride{};
    std::uint32_t vertexCount{};
    std::vector<std::byte> bytes;
};

struct PublishWork
{
    VertexSnapshot vertex;
    ResourceHandle shape{};
    std::vector<std::uint16_t> indices;
};

struct CaptureState
{
    SRWLOCK lock = SRWLOCK_INIT;
    ResourceRegistry registry;
    std::map<std::uint64_t, VertexSnapshot> vertices;
    std::set<std::uint64_t> observedLayouts;
    std::wstring path;
    void* object{};
    void** originalVtable{};
    void** clonedVtable{};
    CreateTriShapeRendererDataFn createTriShape{};
    DecRefTriShapeFn decRefTriShape{};
    CreateVertexBufferFn createVertexBuffer{};
    DecRefVertexBufferFn decRefVertexBuffer{};
    ConcreteCreateTriShapeFn concreteCreateTriShape{};
    std::uint64_t eventValue{};
    std::uintptr_t imageBase{};
    std::size_t snapshotBytes{};
    bool enabled{};
    bool publishing{};
    bool captured{};
    bool concreteBoundary{};
};

CaptureState s_state;

class ExclusiveLock
{
public:
    explicit ExclusiveLock(SRWLOCK& lock) noexcept : lock_(lock)
    {
        AcquireSRWLockExclusive(&lock_);
    }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
private:
    SRWLOCK& lock_;
};

bool SafeCopy(const void* source, void* destination, const std::size_t size) noexcept
{
    if (source == nullptr || destination == nullptr) return false;
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadPointer(const void* source, void*& value) noexcept
{
    return SafeCopy(source, &value, sizeof(value));
}

bool SafeReadU32(const void* source, std::uint32_t& value) noexcept
{
    return SafeCopy(source, &value, sizeof(value));
}

bool Requested() noexcept
{
    wchar_t value[16]{};
    const auto length = GetEnvironmentVariableW(
        L"VISUALFORGE_CAPTURE_MESH_ONCE", value,
        static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) return false;
    return _wcsicmp(value, L"1") == 0 ||
        _wcsicmp(value, L"true") == 0 ||
        _wcsicmp(value, L"yes") == 0;
}

std::wstring CapturePath()
{
    const auto required = GetEnvironmentVariableW(
        L"VISUALFORGE_CAPTURE_MESH_PATH", nullptr, 0);
    if (required > 1) {
        std::wstring value(required, L'\0');
        const auto written = GetEnvironmentVariableW(
            L"VISUALFORGE_CAPTURE_MESH_PATH", value.data(), required);
        if (written > 0 && written < required) {
            value.resize(written);
            return value;
        }
    }
    return std::wstring{vf::config::PluginDir()} +
        L"VisualForge-mesh.vfmesh";
}

std::string Narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const auto required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "<path-conversion-failed>";
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), required, nullptr, nullptr);
    return result;
}

bool WriteAll(const HANDLE file, const std::span<const std::byte> bytes) noexcept
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = std::min(
            bytes.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD written{};
        if (!WriteFile(file, bytes.data() + offset,
                static_cast<DWORD>(count), &written, nullptr) ||
            written != count) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool PublishBytes(
    const std::wstring& path,
    const std::span<const std::byte> bytes,
    DWORD& error) noexcept
{
    const auto temporary = path + L".tmp";
    const auto file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    const auto wrote = WriteAll(file, bytes);
    const auto flushed = wrote && FlushFileBuffers(file) != FALSE;
    const auto closed = CloseHandle(file) != FALSE;
    if (!wrote || !flushed || !closed) {
        error = GetLastError();
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(temporary.c_str());
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

std::uint64_t ResourceId(const ResourceHandle handle) noexcept
{
    auto mixed = handle.address;
    mixed ^= mixed >> 33;
    mixed *= 0xff51afd7ed558ccdull;
    mixed ^= mixed >> 33;
    mixed ^= static_cast<std::uint64_t>(handle.generation) *
        0x9e3779b97f4a7c15ull;
    return 0x7000'0000'0000'0000ull |
        (mixed & 0x0FFF'FFFF'FFFF'FFFFull);
}

void LogObservedLayout(
    const std::uint64_t raw,
    const std::uint32_t stride,
    const VertexLayoutError error,
    const std::size_t attributes) noexcept
{
    bool first{};
    try {
        ExclusiveLock guard{s_state.lock};
        first = s_state.observedLayouts.insert(raw).second;
    } catch (...) {
        return;
    }
    if (first) {
        vf::log::Write(
            "renderer-mesh-capture: observed desc=0x%016llX stride=%u "
            "layout=%s attributes=%llu suppression=off",
            static_cast<unsigned long long>(raw), stride,
            vf::renderer::mesh::ToString(error),
            static_cast<unsigned long long>(attributes));
    }
}

bool PublishCapture(const PublishWork& work) noexcept
{
    try {
        CapturedMesh mesh{};
        mesh.resourceId = ResourceId(work.shape);
        mesh.generation = work.shape.generation;
        mesh.usage = MeshUsage::Immutable;
        mesh.vertexDesc = work.vertex.vertexDesc;
        mesh.stride = work.vertex.stride;
        mesh.vertexCount = work.vertex.vertexCount;
        mesh.vertexBytes = work.vertex.bytes;
        mesh.indices = work.indices;
        mesh.firstIndex = 0;
        mesh.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
        mesh.baseVertex = 0;

        std::vector<std::byte> raster;
        MeshTranslationReport report{};
        const auto translated = TranslateCapturedMesh(
            mesh, 128, 96, raster, report);
        if (translated != MeshPacketError::None) {
            vf::log::Write(
                "renderer-mesh-capture: candidate rejected reason=%s "
                "desc=0x%016llX vertices=%u indices=%u suppression=off",
                vf::renderer::mesh::ToString(translated),
                static_cast<unsigned long long>(mesh.vertexDesc),
                mesh.vertexCount, mesh.indexCount);
            return false;
        }

        std::vector<std::byte> encoded;
        const auto encodedResult = EncodeCapturedMesh(mesh, encoded);
        if (encodedResult != MeshPacketError::None) {
            vf::log::Write(
                "renderer-mesh-capture: encode failed reason=%s suppression=off",
                vf::renderer::mesh::ToString(encodedResult));
            return false;
        }
        DWORD writeError{};
        if (!PublishBytes(s_state.path, encoded, writeError)) {
            vf::log::Write(
                "renderer-mesh-capture: publication failed win32=%lu suppression=off",
                writeError);
            return false;
        }
        const auto path = Narrow(s_state.path);
        vf::log::Write(
            "renderer-mesh-capture: complete path=%s resource=%llu generation=%u "
            "desc=0x%016llX stride=%u vertices=%u indices=%u attributes=%u "
            "bounds-min=%.6g,%.6g,%.6g bounds-max=%.6g,%.6g,%.6g "
            "winding=%s suppression=off",
            path.c_str(), static_cast<unsigned long long>(mesh.resourceId),
            mesh.generation, static_cast<unsigned long long>(mesh.vertexDesc),
            mesh.stride, mesh.vertexCount, mesh.indexCount,
            report.sourceAttributeCount,
            report.sourceBounds.minimum[0], report.sourceBounds.minimum[1],
            report.sourceBounds.minimum[2], report.sourceBounds.maximum[0],
            report.sourceBounds.maximum[1], report.sourceBounds.maximum[2],
            report.clockwise ? "clockwise" : "counter-clockwise");
        return true;
    } catch (...) {
        vf::log::Write(
            "renderer-mesh-capture: candidate failed reason=exception suppression=off");
        return false;
    }
}

void RecordVertexSnapshot(
    void* buffer,
    VertexSnapshot&& snapshot,
    const VertexLayoutError layoutError,
    const std::size_t attributes) noexcept
{
    LogObservedLayout(
        snapshot.vertexDesc, snapshot.stride, layoutError, attributes);
    if (buffer == nullptr || layoutError != VertexLayoutError::None) return;
    try {
        ExclusiveLock guard{s_state.lock};
        if (s_state.captured || s_state.vertices.size() >= kMaximumSnapshots ||
            snapshot.bytes.size() >
                kMaximumSnapshotBytes - s_state.snapshotBytes) {
            return;
        }
        ResourceHandle handle{};
        const auto event = ++s_state.eventValue;
        const auto created = s_state.registry.Create(
            reinterpret_cast<std::uint64_t>(buffer),
            ResourceKind::VertexBuffer, ResourceUsage::Immutable,
            snapshot.bytes.size(), event, handle);
        if (created != ResourceEventError::None) return;
        snapshot.handle = handle;
        s_state.snapshotBytes += snapshot.bytes.size();
        s_state.vertices.emplace(handle.address, std::move(snapshot));
    } catch (...) {
    }
}

bool PreparePublish(
    void* vertexBuffer,
    void* triShape,
    const std::uint64_t vertexDesc,
    std::vector<std::uint16_t>&& indices,
    PublishWork& work) noexcept
{
    try {
        ExclusiveLock guard{s_state.lock};
        if (s_state.captured || s_state.publishing || triShape == nullptr) {
            return false;
        }
        const auto found = s_state.vertices.find(
            reinterpret_cast<std::uint64_t>(vertexBuffer));
        if (found == s_state.vertices.end() ||
            found->second.vertexDesc != vertexDesc) {
            return false;
        }
        ResourceHandle shape{};
        const auto event = ++s_state.eventValue;
        const auto created = s_state.registry.Create(
            reinterpret_cast<std::uint64_t>(triShape), ResourceKind::TriShape,
            ResourceUsage::Immutable,
            indices.size() * sizeof(std::uint16_t), event, shape);
        if (created != ResourceEventError::None) return false;
        static_cast<void>(s_state.registry.Touch(found->second.handle, event));
        s_state.snapshotBytes -= found->second.bytes.size();
        work.vertex = std::move(found->second);
        s_state.vertices.erase(found);
        work.shape = shape;
        work.indices = std::move(indices);
        s_state.publishing = true;
        return true;
    } catch (...) {
        return false;
    }
}

void FinishPublish(const bool success) noexcept
{
    ExclusiveLock guard{s_state.lock};
    s_state.publishing = false;
    s_state.captured = success;
    if (success) {
        s_state.vertices.clear();
        s_state.snapshotBytes = 0;
    }
}

void ReleaseResource(
    void* address,
    const ResourceKind kind,
    const bool finalReference) noexcept
{
    if (!finalReference || address == nullptr) return;
    try {
        ExclusiveLock guard{s_state.lock};
        const auto numeric = reinterpret_cast<std::uint64_t>(address);
        if (kind == ResourceKind::VertexBuffer) {
            const auto found = s_state.vertices.find(numeric);
            if (found != s_state.vertices.end()) {
                s_state.snapshotBytes -= found->second.bytes.size();
                s_state.vertices.erase(found);
            }
        }
        const auto record = s_state.registry.Lookup(numeric, kind);
        if (!record) return;
        const auto event = ++s_state.eventValue;
        if (s_state.registry.Destroy(record->handle, event) ==
            ResourceEventError::None) {
            static_cast<void>(s_state.registry.Retire(event));
        }
    } catch (...) {
    }
}

void* __fastcall HookCreateVertexBuffer(
    void* self,
    std::uint32_t* dataSize,
    void* data,
    const std::uint32_t stride,
    const std::uint64_t vertexDesc)
{
    VertexSnapshot snapshot{};
    auto layoutError = VertexLayoutError::InvalidStride;
    std::size_t attributeCount{};
    bool copied{};
    try {
        std::uint32_t size{};
        EngineVertexLayout layout;
        layoutError = ParseEngineVertexLayout(vertexDesc, layout);
        attributeCount = layout.attributes.size();
        if (SafeReadU32(dataSize, size) && data != nullptr && stride != 0 &&
            size != 0 && size <= kMaximumVertexBytes && size % stride == 0 &&
            size / stride <= std::numeric_limits<std::uint32_t>::max()) {
            snapshot.vertexDesc = vertexDesc;
            snapshot.stride = stride;
            snapshot.vertexCount = size / stride;
            snapshot.bytes.resize(size);
            copied = SafeCopy(data, snapshot.bytes.data(), size);
            if (!copied) snapshot.bytes.clear();
        }
    } catch (...) {
        snapshot = {};
        copied = false;
    }

    auto* const result = s_state.createVertexBuffer(
        self, dataSize, data, stride, vertexDesc);
    if (copied) {
        RecordVertexSnapshot(
            result, std::move(snapshot), layoutError, attributeCount);
    } else {
        LogObservedLayout(vertexDesc, stride, layoutError, attributeCount);
    }
    return result;
}

void* __fastcall HookCreateTriShapeRendererData(
    void* self,
    void* vertexBuffer,
    const std::uint64_t vertexDesc,
    std::uint16_t* indices,
    const std::uint32_t indexCount)
{
    std::vector<std::uint16_t> copiedIndices;
    bool copied{};
    try {
        if (indices != nullptr && indexCount >= 3 && indexCount % 3 == 0 &&
            indexCount <= kMaximumIndices) {
            copiedIndices.resize(indexCount);
            copied = SafeCopy(
                indices, copiedIndices.data(), copiedIndices.size() *
                    sizeof(std::uint16_t));
            if (!copied) copiedIndices.clear();
        }
    } catch (...) {
        copiedIndices.clear();
        copied = false;
    }

    auto* const result = s_state.createTriShape(
        self, vertexBuffer, vertexDesc, indices, indexCount);
    if (copied) {
        PublishWork work;
        if (PreparePublish(
                vertexBuffer, result, vertexDesc,
                std::move(copiedIndices), work)) {
            const auto success = PublishCapture(work);
            FinishPublish(success);
        }
    }
    return result;
}

void __fastcall HookDecRefVertexBuffer(void* self, void* buffer)
{
    std::uint32_t references{};
    const auto finalReference = buffer != nullptr &&
        SafeReadU32(static_cast<const std::byte*>(buffer) + 0x38, references) &&
        references == 1;
    s_state.decRefVertexBuffer(self, buffer);
    ReleaseResource(buffer, ResourceKind::VertexBuffer, finalReference);
}

void __fastcall HookDecRefTriShape(void* self, void* shape)
{
    std::uint32_t references{};
    const auto finalReference = shape != nullptr &&
        SafeReadU32(static_cast<const std::byte*>(shape) + 0x18, references) &&
        references == 1;
    s_state.decRefTriShape(self, shape);
    ReleaseResource(shape, ResourceKind::TriShape, finalReference);
}

bool PrepareConcretePublish(
    void* triShape,
    VertexSnapshot&& vertex,
    std::vector<std::uint16_t>&& indices,
    PublishWork& work) noexcept
{
    void* vertexBuffer{};
    if (triShape == nullptr ||
        !SafeReadPointer(
            static_cast<const std::byte*>(triShape) + 0x08, vertexBuffer) ||
        vertexBuffer == nullptr) {
        return false;
    }
    try {
        ExclusiveLock guard{s_state.lock};
        if (s_state.captured || s_state.publishing) return false;
        const auto event = ++s_state.eventValue;
        ResourceHandle vertexHandle{};
        if (s_state.registry.Create(
                reinterpret_cast<std::uint64_t>(vertexBuffer),
                ResourceKind::VertexBuffer, ResourceUsage::Immutable,
                vertex.bytes.size(), event, vertexHandle) !=
            ResourceEventError::None) {
            return false;
        }
        ResourceHandle shapeHandle{};
        if (s_state.registry.Create(
                reinterpret_cast<std::uint64_t>(triShape),
                ResourceKind::TriShape, ResourceUsage::Immutable,
                indices.size() * sizeof(std::uint16_t), event,
                shapeHandle) != ResourceEventError::None) {
            static_cast<void>(s_state.registry.Destroy(vertexHandle, event));
            static_cast<void>(s_state.registry.Retire(event));
            return false;
        }
        vertex.handle = vertexHandle;
        work.vertex = std::move(vertex);
        work.shape = shapeHandle;
        work.indices = std::move(indices);
        s_state.publishing = true;
        return true;
    } catch (...) {
        return false;
    }
}

void* __fastcall HookConcreteCreateTriShape(
    void* renderer,
    std::uint32_t* dataSize,
    void* data,
    const std::uint64_t vertexDesc,
    void* indexBuffer)
{
    VertexSnapshot vertex{};
    std::vector<std::uint16_t> indices;
    EngineVertexLayout layout;
    const auto layoutError = ParseEngineVertexLayout(vertexDesc, layout);
    bool copiedVertex{};
    bool copiedIndices{};
    try {
        std::uint32_t size{};
        const auto stride = static_cast<std::uint32_t>(vertexDesc & 0xFu) * 4u;
        if (layoutError == VertexLayoutError::None &&
            SafeReadU32(dataSize, size) && data != nullptr && stride != 0 &&
            size != 0 && size <= kMaximumVertexBytes && size % stride == 0) {
            vertex.vertexDesc = vertexDesc;
            vertex.stride = stride;
            vertex.vertexCount = size / stride;
            vertex.bytes.resize(size);
            copiedVertex = SafeCopy(data, vertex.bytes.data(), size);
            if (!copiedVertex) vertex.bytes.clear();
        }

        void* indexData{};
        std::uint32_t indexBytes{};
        if (indexBuffer != nullptr &&
            SafeReadPointer(
                static_cast<const std::byte*>(indexBuffer) + 0x08,
                indexData) &&
            SafeReadU32(
                static_cast<const std::byte*>(indexBuffer) + 0x34,
                indexBytes) &&
            indexData != nullptr && indexBytes >= 6 && indexBytes % 2 == 0 &&
            indexBytes / 2 <= kMaximumIndices) {
            indices.resize(indexBytes / 2);
            copiedIndices = SafeCopy(
                indexData, indices.data(), indexBytes);
            if (!copiedIndices) indices.clear();
        }
    } catch (...) {
        vertex = {};
        indices.clear();
        copiedVertex = false;
        copiedIndices = false;
    }

    auto* const result = s_state.concreteCreateTriShape(
        renderer, dataSize, data, vertexDesc, indexBuffer);
    LogObservedLayout(
        vertexDesc,
        static_cast<std::uint32_t>(vertexDesc & 0xFu) * 4u,
        layoutError,
        layout.attributes.size());
    if (copiedVertex && copiedIndices) {
        PublishWork work;
        if (PrepareConcretePublish(
                result, std::move(vertex), std::move(indices), work)) {
            const auto success = PublishCapture(work);
            FinishPublish(success);
        }
    }
    return result;
}

bool InstallConcreteBoundary(const std::uintptr_t imageBase) noexcept
{
    auto* const target = reinterpret_cast<void*>(imageBase + 0x01818760u);
    auto status = MH_CreateHook(
        target,
        reinterpret_cast<void*>(&HookConcreteCreateTriShape),
        reinterpret_cast<void**>(&s_state.concreteCreateTriShape));
    if (status != MH_OK) {
        vf::log::Write(
            "renderer-mesh-capture: rejected boundary=concrete reason=minhook-create-%d "
            "suppression=off",
            static_cast<int>(status));
        return false;
    }
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        static_cast<void>(MH_RemoveHook(target));
        s_state.concreteCreateTriShape = nullptr;
        vf::log::Write(
            "renderer-mesh-capture: rejected boundary=concrete reason=minhook-enable-%d "
            "suppression=off",
            static_cast<int>(status));
        return false;
    }
    s_state.concreteBoundary = true;
    s_state.enabled = true;
    const auto path = Narrow(s_state.path);
    vf::log::Write(
        "renderer-mesh-capture: armed boundary=concrete rva=0x01818760 "
        "hooks=1 path=%s suppression=off",
        path.c_str());
    return true;
}

}

bool Configure(
    const bool buildValidated,
    const std::uintptr_t imageBase) noexcept
{
    if (!buildValidated || !Requested()) return false;
    try {
        ExclusiveLock guard{s_state.lock};
        if (s_state.enabled) return true;
        s_state.imageBase = imageBase;
        if (s_state.path.empty()) s_state.path = CapturePath();
        if (imageBase == 0) {
            vf::log::Write(
                "renderer-mesh-capture: rejected reason=null-image-base suppression=off");
            return false;
        }

        void* object{};
        if (!SafeReadPointer(
                reinterpret_cast<const void*>(
                    imageBase + kResourceManagerSingletonRva), object) ||
            object == nullptr) {
            vf::log::Write(
                "renderer-mesh-capture: singleton unavailable; "
                "selecting verified concrete boundary suppression=off");
            return InstallConcreteBoundary(imageBase);
        }
        void* vtablePointer{};
        if (!SafeReadPointer(object, vtablePointer) || vtablePointer == nullptr) {
            vf::log::Write(
                "renderer-mesh-capture: rejected reason=vtable-unavailable suppression=off");
            return false;
        }
        std::array<std::uintptr_t, kResourceManagerVtableSlots> slots{};
        if (!SafeCopy(vtablePointer, slots.data(), sizeof(slots))) {
            vf::log::Write(
                "renderer-mesh-capture: rejected reason=vtable-unreadable suppression=off");
            return false;
        }
        const auto contract = ValidateResourceManagerVtable(
            imageBase, reinterpret_cast<std::uintptr_t>(vtablePointer), slots);
        if (!contract) {
            vf::log::Write(
                "renderer-mesh-capture: rejected reason=%s failed-slot=%u "
                "validated=%llu suppression=off",
                vf::renderer::ToString(contract.error), contract.failedSlot,
                static_cast<unsigned long long>(contract.validatedSlots));
            return false;
        }

        const auto tableBytes = sizeof(void*) * kResourceManagerVtableSlots;
        auto** clone = static_cast<void**>(VirtualAlloc(
            nullptr, tableBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (clone == nullptr) {
            vf::log::Write(
                "renderer-mesh-capture: rejected reason=clone-allocation suppression=off");
            return false;
        }
        std::memcpy(clone, vtablePointer, tableBytes);
        clone[4] = reinterpret_cast<void*>(&HookCreateTriShapeRendererData);
        clone[7] = reinterpret_cast<void*>(&HookDecRefTriShape);
        clone[40] = reinterpret_cast<void*>(&HookCreateVertexBuffer);
        clone[41] = reinterpret_cast<void*>(&HookDecRefVertexBuffer);
        DWORD oldProtection{};
        if (!VirtualProtect(clone, tableBytes, PAGE_READONLY, &oldProtection)) {
            VirtualFree(clone, 0, MEM_RELEASE);
            vf::log::Write(
                "renderer-mesh-capture: rejected reason=clone-protection suppression=off");
            return false;
        }

        s_state.object = object;
        s_state.originalVtable = static_cast<void**>(vtablePointer);
        s_state.clonedVtable = clone;
        s_state.createTriShape = reinterpret_cast<CreateTriShapeRendererDataFn>(
            slots[4]);
        s_state.decRefTriShape = reinterpret_cast<DecRefTriShapeFn>(slots[7]);
        s_state.createVertexBuffer = reinterpret_cast<CreateVertexBufferFn>(
            slots[40]);
        s_state.decRefVertexBuffer = reinterpret_cast<DecRefVertexBufferFn>(
            slots[41]);
        s_state.path = CapturePath();

        auto* const observed = InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(object), clone, vtablePointer);
        if (observed != vtablePointer) {
            s_state.object = nullptr;
            s_state.originalVtable = nullptr;
            s_state.clonedVtable = nullptr;
            s_state.createTriShape = nullptr;
            s_state.decRefTriShape = nullptr;
            s_state.createVertexBuffer = nullptr;
            s_state.decRefVertexBuffer = nullptr;
            VirtualFree(clone, 0, MEM_RELEASE);
            vf::log::Write(
                "renderer-mesh-capture: rejected reason=vtable-race suppression=off");
            return false;
        }
        s_state.enabled = true;
        const auto path = Narrow(s_state.path);
        vf::log::Write(
            "renderer-mesh-capture: armed slots=52 hooks=4 path=%s suppression=off",
            path.c_str());
        return true;
    } catch (...) {
        vf::log::Write(
            "renderer-mesh-capture: rejected reason=exception suppression=off");
        return false;
    }
}

bool Enabled() noexcept
{
    ExclusiveLock guard{s_state.lock};
    return s_state.enabled;
}

bool Arm(const wchar_t* path) noexcept
{
    if (path == nullptr || *path == L'\0') return false;
    try {
        ExclusiveLock guard{s_state.lock};
        if (!s_state.enabled || s_state.publishing) return false;
        s_state.path.assign(path);
        s_state.captured = false;
        s_state.vertices.clear();
        s_state.snapshotBytes = 0;
        return true;
    } catch (...) {
        return false;
    }
}

bool Retry() noexcept
{
    std::uintptr_t imageBase{};
    {
        ExclusiveLock guard{s_state.lock};
        if (s_state.enabled) return true;
        imageBase = s_state.imageBase;
    }
    return imageBase != 0 && Configure(true, imageBase);
}

}
