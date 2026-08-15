#include "EngineCameraCapture.h"

#include "Log.h"
#include "renderer_core/CameraStateScan.h"
#include "renderer_core/EngineView.h"
#include "renderer_host/GraphicsStateContract.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace vf::engine_camera_capture {

namespace {

using vf::renderer::camera::CameraError;
using vf::renderer::camera::CameraObservation;
using vf::renderer::camera::ScanCameraState;

// Scanning walks the camera window every frame while armed. If the engine
// never publishes a camera, the attempt budget gives up instead of taxing
// every frame for the rest of the session.
constexpr std::uint32_t kMaximumScanAttempts = 240;

struct CaptureState
{
    SRWLOCK lock = SRWLOCK_INIT;
    std::wstring path;
    std::uintptr_t imageBase{};
    std::uint64_t sequence{};
    std::uint32_t attempts{};
    bool enabled{};
    bool armed{};
    bool publishing{};
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

// The engine can retire the state record between the pointer check and the
// read, so the copy is guarded rather than trusted.
bool SafeCopy(
    const void* const source,
    void* const destination,
    const std::size_t size) noexcept
{
    if (source == nullptr || destination == nullptr) return false;
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string Narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const auto required = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string narrow(static_cast<std::size_t>(required), '\0');
    const auto written = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        narrow.data(), required, nullptr, nullptr);
    if (written != required) return {};
    return narrow;
}

bool PublishBytes(
    const std::wstring& path,
    const std::span<const std::byte> bytes,
    DWORD& error) noexcept
{
    error = 0;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written{};
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, 1u << 20));
        if (!WriteFile(file, bytes.data() + offset, chunk, &written,
                nullptr) || written != chunk) {
            error = GetLastError();
            CloseHandle(file);
            return false;
        }
        offset += written;
    }
    CloseHandle(file);
    return true;
}

}

bool Configure(
    const bool buildValidated,
    const std::uintptr_t imageBase) noexcept
{
    ExclusiveLock guard{s_state.lock};
    // The camera window is a build-specific address, so an unvalidated build
    // never gets read.
    if (!buildValidated || imageBase == 0) {
        vf::log::Write(
            "renderer-camera-capture: inert reason=%s suppression=off",
            buildValidated ? "null-image-base" : "build-not-validated");
        return false;
    }
    s_state.imageBase = imageBase;
    s_state.enabled = true;
    return true;
}

bool Enabled() noexcept
{
    ExclusiveLock guard{s_state.lock};
    return s_state.enabled;
}

bool Arm(const wchar_t* const path) noexcept
{
    if (path == nullptr || *path == L'\0') return false;
    try {
        ExclusiveLock guard{s_state.lock};
        if (!s_state.enabled || s_state.publishing) return false;
        s_state.path.assign(path);
        s_state.armed = true;
        s_state.attempts = 0;
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

// Bytes captured around the graphics state when the scan fails. Large enough
// to contain the camera record wherever it actually sits in this build, so
// the matrices can be located offline by the same self-consistency test.
constexpr std::size_t kStateDumpBytes = 0x4000;

// `BSGraphics::State + 0x140` is the camera-state cache array. The current
// record at 0x160 is embedded by value, but the cache holds references to
// the *other* live cameras, so scanning the state record alone can only ever
// find whichever camera the engine set last. Against a running game that was
// a secondary camera (fov 24, near 15, at the origin, byte-identical between
// a loading screen and loaded gameplay), which is why the world view has not
// been found yet.
//
// The array's element shape is not recorded, so nothing is assumed about it:
// each of the four qwords is reported, and any that is a plausible user-mode
// pointer is followed and scanned. A qword that is not a pointer shows up in
// the log as exactly that, which turns the shape into an observation instead
// of another guess.
// Observed live: the four qwords read `{heap pointer, 8, 5, 0}`, which is an
// array of capacity 8 holding 5 entries. Scanning only the first entry found
// one camera at the record's documented `+0x050` view offset, so the entries
// are `CameraStateData` by value and the remaining four have never been
// looked at. That is where the world view has been hiding.
constexpr std::uint32_t kCameraCacheOffset = 0x140u;
constexpr std::uint32_t kCameraCacheSlots = 4u;
constexpr std::uint32_t kCameraCacheDataSlot = 0u;
constexpr std::uint32_t kCameraCacheCapacitySlot = 1u;
constexpr std::uint32_t kCameraCacheCountSlot = 2u;
// A bound on how much of a possibly-misread array is walked. The count is
// read from the engine, so it is treated as a hint rather than as truth.
constexpr std::uint64_t kMaximumCachedCameras = 16u;

// User-mode x64 addresses have a zero high word and never sit in the first
// page. This rejects small integers, counts, and flags without pretending to
// validate the target.
bool PlausiblePointer(const std::uint64_t value) noexcept
{
    return value >= 0x1'0000ull && (value >> 48) == 0;
}

struct CacheProbe
{
    std::uint64_t value{};
    bool readable{};
    std::uint32_t cameras{};
};

bool DumpStateRegion(
    const std::wstring& path,
    const std::uintptr_t imageBase) noexcept
{
    if (path.empty() || imageBase == 0) return false;
    try {
        const auto base = imageBase + vf::renderer::kGraphicsStateRva;
        std::vector<std::byte> block(kStateDumpBytes);
        if (!SafeCopy(reinterpret_cast<const void*>(base), block.data(),
                block.size())) {
            return false;
        }
        auto target = path;
        target.append(L".state.bin");
        const auto handle = CreateFileW(target.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const auto ok = WriteFile(handle, block.data(),
            static_cast<DWORD>(block.size()), &written, nullptr) != FALSE &&
            written == block.size();
        CloseHandle(handle);
        return ok;
    } catch (...) {
        return false;
    }
}

}

std::vector<vf::renderer::camera::CameraScanResult> ReadLiveCameras(
    const std::uintptr_t imageBase)
{
    std::vector<vf::renderer::camera::CameraScanResult> cameras;
    if (imageBase == 0) return cameras;
    vf::renderer::GraphicsStateWindow window{};
    if (vf::renderer::ResolveGraphicsStateWindow(imageBase, window) !=
        vf::renderer::GraphicsStateError::None) {
        return cameras;
    }
    std::vector<std::byte> block;
    try {
        block.resize(window.size);
    } catch (...) {
        return cameras;
    }
    if (!SafeCopy(reinterpret_cast<const void*>(window.address),
            block.data(), block.size())) {
        return cameras;
    }
    cameras = vf::renderer::camera::ScanCameraStates(block);
    if (block.size() < kCameraCacheOffset + kCameraCacheSlots * 8) {
        return cameras;
    }
    std::array<std::uint64_t, kCameraCacheSlots> slots{};
    for (std::uint32_t slot = 0; slot < kCameraCacheSlots; ++slot) {
        std::memcpy(&slots[slot],
            block.data() + kCameraCacheOffset + slot * 8, sizeof(slots[slot]));
    }
    const auto data = slots[kCameraCacheDataSlot];
    if (!PlausiblePointer(data)) return cameras;
    const auto entries = std::min({slots[kCameraCacheCountSlot],
        slots[kCameraCacheCapacitySlot], kMaximumCachedCameras});
    for (std::uint64_t entry = 0; entry < entries; ++entry) {
        std::vector<std::byte> cached;
        try {
            cached.resize(vf::renderer::kCameraStateDataSize);
        } catch (...) {
            break;
        }
        const auto address = static_cast<std::uintptr_t>(data) +
            static_cast<std::uintptr_t>(entry) *
                vf::renderer::kCameraStateDataSize;
        if (!SafeCopy(reinterpret_cast<const void*>(address),
                cached.data(), cached.size())) {
            continue;
        }
        auto found = vf::renderer::camera::ScanCameraStates(cached);
        for (auto& camera : found) {
            camera.sourceSlot = static_cast<std::uint32_t>(entry) + 1u;
        }
        try {
            cameras.insert(cameras.end(), found.begin(), found.end());
        } catch (...) {
        }
    }
    return cameras;
}

bool SelectWorldCamera(
    const std::vector<vf::renderer::camera::CameraScanResult>& cameras,
    vf::renderer::camera::CameraScanResult& selected) noexcept
{
    // The world view is the one that has to reach the horizon. Selecting on
    // the largest far plane is a measurement rather than a hardcoded slot,
    // so it keeps working if the engine reorders its cache.
    const vf::renderer::camera::CameraScanResult* best = nullptr;
    auto bestFar = -1.0f;
    for (const auto& camera : cameras) {
        if (!camera.found) continue;
        vf::renderer::view::ClipPlanes planes{};
        if (vf::renderer::view::ExtractClipPlanes(camera.projection,
                vf::renderer::view::ProjectionMode::Perspective,
                vf::renderer::view::Handedness::LeftHanded, planes) !=
            vf::renderer::view::ViewError::None) {
            continue;
        }
        if (planes.farPlane > bestFar) {
            bestFar = planes.farPlane;
            best = &camera;
        }
    }
    if (best == nullptr) return false;
    selected = *best;
    return true;
}

void OnPresent(
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    std::wstring path;
    std::uintptr_t imageBase{};
    std::uint64_t sequence{};
    {
        ExclusiveLock guard{s_state.lock};
        if (!s_state.enabled || !s_state.armed || s_state.publishing) return;
        s_state.publishing = true;
        path = s_state.path;
        imageBase = s_state.imageBase;
        sequence = ++s_state.sequence;
    }

    const auto fail = [&](const char* reason) {
        vf::log::Write(
            "renderer-camera-capture: rejected reason=%s suppression=off",
            reason);
        ExclusiveLock guard{s_state.lock};
        s_state.publishing = false;
        s_state.armed = false;
    };

    vf::renderer::GraphicsStateWindow window{};
    const auto resolved = vf::renderer::ResolveGraphicsStateWindow(
        imageBase, window);
    if (resolved != vf::renderer::GraphicsStateError::None) {
        fail(vf::renderer::ToString(resolved));
        return;
    }

    std::vector<std::byte> block;
    try {
        block.resize(window.size);
    } catch (...) {
        fail("allocation");
        return;
    }
    if (!SafeCopy(reinterpret_cast<const void*>(window.address),
            block.data(), block.size())) {
        fail("state-unreadable");
        return;
    }

    auto cameras = vf::renderer::camera::ScanCameraStates(block);
    const auto embeddedCount = cameras.size();

    // Follow the camera-state cache. Every entry is reported whether or not
    // it resolves, so a cache that is not an array of pointers is visible as
    // a fact rather than as silence.
    std::array<CacheProbe, kCameraCacheSlots> probes{};
    std::uint64_t cachedEntries = 0;
    if (block.size() >= kCameraCacheOffset + kCameraCacheSlots * 8) {
        for (std::uint32_t slot = 0; slot < kCameraCacheSlots; ++slot) {
            std::memcpy(&probes[slot].value,
                block.data() + kCameraCacheOffset + slot * 8,
                sizeof(probes[slot].value));
        }
        const auto data = probes[kCameraCacheDataSlot].value;
        const auto capacity = probes[kCameraCacheCapacitySlot].value;
        const auto count = probes[kCameraCacheCountSlot].value;
        // The count is only believed as far as the capacity and a hard cap
        // allow, so a misread header walks a bounded region instead of an
        // arbitrary one.
        cachedEntries = std::min({count, capacity, kMaximumCachedCameras});
        if (PlausiblePointer(data)) {
            for (std::uint64_t entry = 0; entry < cachedEntries; ++entry) {
                std::vector<std::byte> cached;
                try {
                    cached.resize(vf::renderer::kCameraStateDataSize);
                } catch (...) {
                    break;
                }
                const auto address = static_cast<std::uintptr_t>(data) +
                    static_cast<std::uintptr_t>(entry) *
                        vf::renderer::kCameraStateDataSize;
                if (!SafeCopy(reinterpret_cast<const void*>(address),
                        cached.data(), cached.size())) {
                    vf::log::Write(
                        "renderer-camera-capture: cache entry=%llu "
                        "address=0x%016llX unreadable suppression=off",
                        static_cast<unsigned long long>(entry),
                        static_cast<unsigned long long>(address));
                    continue;
                }
                auto found = vf::renderer::camera::ScanCameraStates(cached);
                // Slot 0 is the state record's embedded camera, so cache
                // entries start at 1. Without this every cached camera would
                // claim the same identity and the frame packet would be
                // refused for duplicate views.
                for (auto& camera : found) {
                    camera.sourceSlot =
                        static_cast<std::uint32_t>(entry) + 1u;
                }
                probes[kCameraCacheDataSlot].readable = true;
                probes[kCameraCacheDataSlot].cameras +=
                    static_cast<std::uint32_t>(found.size());
                vf::log::Write(
                    "renderer-camera-capture: cache entry=%llu "
                    "address=0x%016llX cameras=%llu suppression=off",
                    static_cast<unsigned long long>(entry),
                    static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(found.size()));
                try {
                    cameras.insert(cameras.end(), found.begin(), found.end());
                } catch (...) {
                }
            }
        }
    }
    vf::log::Write(
        "renderer-camera-capture: cache data=0x%016llX capacity=%llu "
        "count=%llu walked=%llu suppression=off",
        static_cast<unsigned long long>(probes[kCameraCacheDataSlot].value),
        static_cast<unsigned long long>(
            probes[kCameraCacheCapacitySlot].value),
        static_cast<unsigned long long>(probes[kCameraCacheCountSlot].value),
        static_cast<unsigned long long>(cachedEntries));

    const auto scan = cameras.empty()
        ? vf::renderer::camera::CameraScanResult{} : cameras.front();
    if (!scan.found) {
        // Common before the first world frame: the record exists but holds
        // no camera yet. Stay armed and try again next frame, but only for a
        // bounded number of frames.
        ExclusiveLock guard{s_state.lock};
        s_state.publishing = false;
        if (++s_state.attempts >= kMaximumScanAttempts) {
            s_state.armed = false;
            s_state.attempts = 0;
            // A scan that finds nothing cannot say whether the window is
            // wrong or merely empty. Dumping the region it read turns that
            // into an offline question that can be answered with evidence
            // instead of another guess at an offset.
            const auto dumped = DumpStateRegion(path, imageBase);
            // `cameras` is empty here, so `scan` is default constructed and
            // its candidate count is a hardcoded zero rather than a
            // measurement. Re-scan once to report what was actually
            // examined; a count that cannot be trusted is worse than none.
            const auto diagnostic =
                vf::renderer::camera::ScanCameraState(block);
            vf::log::Write(
                "renderer-camera-capture: rejected reason=no-camera-found "
                "attempts=%u candidates=%u bytes=%zu dump=%s suppression=off",
                kMaximumScanAttempts, diagnostic.candidateCount, block.size(),
                dumped ? "written" : "failed");
        }
        return;
    }

    // Every camera the record holds is published as its own view, so the
    // main world view can be told apart from first-person and shadow
    // cameras by inspection instead of by assumption.
    vf::renderer::camera::CameraSeries series{};
    series.cameras = cameras;
    series.outputWidth = width;
    series.outputHeight = height;
    series.frameId = sequence;
    series.engineFrameId = 0xE000'0000'0000'0000ull | sequence;
    series.captureSequence = sequence;
    series.threadId = GetCurrentThreadId();

    vf::renderer::view::FramePacket packet{};
    const auto built = vf::renderer::camera::BuildFrameSeries(series, packet);
    if (built != CameraError::None) {
        fail(vf::renderer::camera::ToString(built));
        return;
    }
    for (std::size_t index = 0; index < cameras.size() &&
         index < packet.views.size(); ++index) {
        const auto& record = packet.views[index];
        vf::log::Write(
            "renderer-camera-capture: camera index=%llu source=%s "
            "view-offset=0x%03X "
            "projection-offset=0x%03X residual=%.3g near=%.3f far=%.1f "
            "fov=%.2fdeg position=%.1f,%.1f,%.1f suppression=off",
            static_cast<unsigned long long>(index),
            index < embeddedCount ? "state" : "cache",
            cameras[index].viewOffset,
            cameras[index].projectionOffset,
            static_cast<double>(cameras[index].residual),
            static_cast<double>(record.nearPlane),
            static_cast<double>(record.farPlane),
            static_cast<double>(record.verticalFovRadians) * 57.2957795,
            static_cast<double>(cameras[index].view.elements[3]),
            static_cast<double>(cameras[index].view.elements[7]),
            static_cast<double>(cameras[index].view.elements[11]));
    }
    std::vector<std::byte> bytes;
    if (vf::renderer::view::EncodeFramePacket(packet, bytes) !=
        vf::renderer::view::FramePacketError::None) {
        fail("encode");
        return;
    }
    DWORD error{};
    if (!PublishBytes(path, bytes, error)) {
        vf::log::Write(
            "renderer-camera-capture: publication failed win32=%lu "
            "suppression=off", error);
        ExclusiveLock guard{s_state.lock};
        s_state.publishing = false;
        s_state.armed = false;
        return;
    }

    const auto narrow = Narrow(path);
    vf::log::Write(
        "renderer-camera-capture: complete path=%s bytes=%llu cameras=%llu "
        "embedded=%llu views=%llu storage=%s candidates=%u extent=%ux%u "
        "suppression=off",
        narrow.c_str(),
        static_cast<unsigned long long>(bytes.size()),
        static_cast<unsigned long long>(cameras.size()),
        static_cast<unsigned long long>(embeddedCount),
        static_cast<unsigned long long>(packet.views.size()),
        scan.storage == vf::renderer::view::MatrixStorage::RowMajor
            ? "row-major" : "column-major",
        scan.candidateCount,
        width, height);

    ExclusiveLock guard{s_state.lock};
    s_state.publishing = false;
    s_state.armed = false;
}

}
