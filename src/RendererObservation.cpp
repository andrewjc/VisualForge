#include "RendererObservation.h"

#include "Config.h"
#include "Log.h"
#include "renderer_api/StableId.h"
#include "renderer_trace/FrameCapture.h"
#include "renderer_trace/TraceCodec.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>

namespace vf::renderer_observation {

namespace {

using vf::renderer::MakeStableId;
using vf::renderer::StableIdDomain;
using namespace vf::renderer::trace;

constexpr std::size_t kLiveTraceLimit = 1024 * 1024;

struct ObservationState
{
    SRWLOCK lock = SRWLOCK_INIT;
    std::optional<FrameCapture> capture;
    std::wstring path;
    bool enabled{};
    bool viewReady{};
    bool inFrame{};
    std::uint64_t captureId{};
    std::uint64_t currentFrameId{};
    std::uint64_t swapchainId{};
    std::uint64_t viewId{};
    std::uint64_t targetId{};
    std::uint64_t qpcFrequency{};
    std::uint32_t frameOrdinal{};
    std::uint32_t writerCount{};
    std::uint32_t resourceGeneration{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t format{};
    std::uint32_t sampleCount{1};
};

ObservationState s_state;

class ExclusiveLock
{
public:
    explicit ExclusiveLock(SRWLOCK& lock) noexcept
        : lock_(lock)
    {
        AcquireSRWLockExclusive(&lock_);
    }

    ~ExclusiveLock()
    {
        ReleaseSRWLockExclusive(&lock_);
    }

    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;

private:
    SRWLOCK& lock_;
};

std::uint64_t CounterNow() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

bool TraceRequested() noexcept
{
    wchar_t value[16]{};
    const auto length = GetEnvironmentVariableW(
        L"VISUALFORGE_TRACE_ONCE", value, static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) {
        return false;
    }
    return _wcsicmp(value, L"1") == 0 ||
        _wcsicmp(value, L"true") == 0 ||
        _wcsicmp(value, L"yes") == 0;
}

std::wstring ResolveTracePath()
{
    const auto required = GetEnvironmentVariableW(
        L"VISUALFORGE_TRACE_PATH", nullptr, 0);
    if (required > 1) {
        std::wstring value(required, L'\0');
        const auto written = GetEnvironmentVariableW(
            L"VISUALFORGE_TRACE_PATH", value.data(), required);
        if (written > 0 && written < required) {
            value.resize(written);
            return value;
        }
    }
    return std::wstring{vf::config::PluginDir()} +
        L"VisualForge-frame.vftrace";
}

bool WriteAll(const HANDLE file, const std::span<const std::byte> bytes) noexcept
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = std::min(
            bytes.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD written{};
        if (!WriteFile(
                file,
                bytes.data() + offset,
                static_cast<DWORD>(chunk),
                &written,
                nullptr) ||
            written != chunk) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool PublishTrace(
    const std::wstring& path,
    const std::span<const std::byte> bytes,
    DWORD& error) noexcept
{
    try {
        const auto temporary = path + L".tmp";
        const auto file = CreateFileW(
            temporary.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            return false;
        }

        const auto wrote = WriteAll(file, bytes);
        const auto flushed = wrote && FlushFileBuffers(file) != FALSE;
        if (!CloseHandle(file) || !flushed) {
            error = GetLastError();
            DeleteFileW(temporary.c_str());
            return false;
        }
        if (!MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = GetLastError();
            DeleteFileW(temporary.c_str());
            return false;
        }
        error = ERROR_SUCCESS;
        return true;
    } catch (...) {
        error = ERROR_OUTOFMEMORY;
        return false;
    }
}

void AbortLocked(const char* reason) noexcept
{
    vf::log::Write("renderer-observe: aborted reason=%s", reason);
    s_state.enabled = false;
    s_state.inFrame = false;
    s_state.capture.reset();
}

bool RecordWriterLocked(
    const WriterClassification classification,
    const std::string_view canonicalWriterKey) noexcept
{
    if (!s_state.capture || !s_state.inFrame) {
        return false;
    }
    const auto generation =
        (s_state.frameOrdinal << 8) | (s_state.writerCount & 0xFFu);
    const auto cpuCorrelation = MakeStableId(
        StableIdDomain::Correlation, canonicalWriterKey, generation);
    const auto gpuCorrelation = MakeStableId(
        StableIdDomain::Correlation,
        canonicalWriterKey,
        generation ^ 0x80000000u);
    const WriterEvent event{
        s_state.currentFrameId,
        MakeStableId(
            StableIdDomain::Writer,
            canonicalWriterKey,
            0).value,
        s_state.targetId,
        gpuCorrelation.value,
        classification,
        s_state.writerCount,
    };
    const auto result = s_state.capture->RecordWriter(
        event, cpuCorrelation.value);
    if (result != FrameCaptureError::None) {
        AbortLocked("writer-record");
        return false;
    }
    ++s_state.writerCount;
    return true;
}

}

bool Configure(const bool buildValidated) noexcept
{
    if (!buildValidated || !TraceRequested()) {
        return false;
    }

    ExclusiveLock guard{s_state.lock};
    if (s_state.enabled) {
        return true;
    }
    try {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            return false;
        }
        const auto start = CounterNow();
        const auto captureId = MakeStableId(
            StableIdDomain::Capture,
            "fallout4/live-observe-once",
            static_cast<std::uint32_t>(start));
        s_state.capture.emplace(kLiveTraceLimit);
        if (s_state.capture->Start({
                captureId.value,
                static_cast<std::uint64_t>(frequency.QuadPart),
                start,
            }) != FrameCaptureError::None) {
            s_state.capture.reset();
            return false;
        }

        s_state.path = ResolveTracePath();
        s_state.enabled = true;
        s_state.captureId = captureId.value;
        s_state.qpcFrequency =
            static_cast<std::uint64_t>(frequency.QuadPart);
        s_state.swapchainId = MakeStableId(
            StableIdDomain::Swapchain, "dxgi/primary", 0).value;
        s_state.viewId = MakeStableId(
            StableIdDomain::View, "fallout4/main-view", 0).value;
        s_state.targetId = MakeStableId(
            StableIdDomain::Resource, "dxgi/primary/backbuffer", 0).value;
        vf::log::Write(
            "renderer-observe: armed state=Observing capture=%llu",
            static_cast<unsigned long long>(s_state.captureId));
        return true;
    } catch (...) {
        s_state.capture.reset();
        s_state.enabled = false;
        return false;
    }
}

bool Enabled() noexcept
{
    ExclusiveLock guard{s_state.lock};
    return s_state.enabled;
}

bool ArmTrace(const wchar_t* const path) noexcept
{
    if (path == nullptr || *path == L'\0') return false;
    ExclusiveLock guard{s_state.lock};
    // A trace already recording owns its window; a live request must not
    // truncate it and redirect the output mid-frame.
    if (s_state.enabled || s_state.inFrame) return false;
    try {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) ||
            frequency.QuadPart <= 0) {
            return false;
        }
        const auto start = CounterNow();
        const auto captureId = MakeStableId(
            StableIdDomain::Capture,
            "fallout4/live-observe-request",
            static_cast<std::uint32_t>(start));
        s_state.capture.emplace(kLiveTraceLimit);
        if (s_state.capture->Start({
                captureId.value,
                static_cast<std::uint64_t>(frequency.QuadPart),
                start,
            }) != FrameCaptureError::None) {
            s_state.capture.reset();
            return false;
        }
        s_state.path.assign(path);
        s_state.enabled = true;
        s_state.captureId = captureId.value;
        s_state.qpcFrequency =
            static_cast<std::uint64_t>(frequency.QuadPart);
        s_state.frameOrdinal = 0;
        s_state.writerCount = 0;
        return true;
    } catch (...) {
        s_state.capture.reset();
        s_state.enabled = false;
        return false;
    }
}

void OnSwapchainReady(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t format,
    const std::uint32_t sampleCount) noexcept
{
    ExclusiveLock guard{s_state.lock};
    if (!s_state.enabled) {
        return;
    }
    s_state.width = width;
    s_state.height = height;
    s_state.format = format;
    s_state.sampleCount = sampleCount == 0 ? 1 : sampleCount;
    s_state.viewReady = width != 0 && height != 0;
}

void OnPresentBegin(
    const std::uint32_t,
    const std::uint32_t) noexcept
{
    ExclusiveLock guard{s_state.lock};
    if (!s_state.enabled || !s_state.capture ||
        !s_state.viewReady || s_state.inFrame) {
        return;
    }

    ++s_state.frameOrdinal;
    s_state.currentFrameId = MakeStableId(
        StableIdDomain::Frame,
        "fallout4/present-frame",
        s_state.frameOrdinal).value;
    const auto beginCorrelation = MakeStableId(
        StableIdDomain::Correlation,
        "dxgi/present-begin",
        s_state.frameOrdinal).value;
    const FrameBegin begin{
        s_state.currentFrameId,
        CounterNow(),
        GetCurrentThreadId(),
        0,
    };
    if (s_state.capture->BeginFrame(begin, beginCorrelation) !=
        FrameCaptureError::None) {
        AbortLocked("frame-begin");
        return;
    }

    const auto viewCorrelation = MakeStableId(
        StableIdDomain::Correlation,
        "dxgi/view",
        s_state.frameOrdinal).value;
    const ViewMetadata view{
        s_state.currentFrameId,
        s_state.viewId,
        s_state.swapchainId,
        s_state.width,
        s_state.height,
        s_state.format,
        s_state.sampleCount,
    };
    if (s_state.capture->SetView(view, viewCorrelation) !=
        FrameCaptureError::None) {
        AbortLocked("view");
        return;
    }

    s_state.writerCount = 0;
    s_state.inFrame = true;
    static_cast<void>(RecordWriterLocked(
        WriterClassification::World,
        "fallout4/vanilla-frame"));
}

void RecordWriter(
    const WriterClassification classification,
    const std::string_view canonicalWriterKey) noexcept
{
    ExclusiveLock guard{s_state.lock};
    if (s_state.enabled) {
        static_cast<void>(RecordWriterLocked(
            classification, canonicalWriterKey));
    }
}

void OnPresentEnd(
    const std::int32_t presentResult,
    const std::uint32_t syncInterval,
    const std::uint32_t presentFlags) noexcept
{
    ExclusiveLock guard{s_state.lock};
    if (!s_state.enabled || !s_state.capture || !s_state.inFrame) {
        return;
    }

    const auto endCorrelation = MakeStableId(
        StableIdDomain::Correlation,
        "dxgi/present-end",
        s_state.frameOrdinal).value;
    const FrameEnd end{
        s_state.currentFrameId,
        CounterNow(),
        syncInterval,
        presentFlags,
        presentResult,
        s_state.writerCount,
    };
    if (s_state.capture->EndFrame(end, endCorrelation) !=
            FrameCaptureError::None ||
        s_state.capture->Finish(CounterNow()) !=
            FrameCaptureError::None) {
        AbortLocked("frame-end");
        return;
    }

    const auto inspection = InspectTrace(s_state.capture->Bytes());
    if (!inspection) {
        AbortLocked("self-inspection");
        return;
    }
    DWORD writeError{};
    if (!PublishTrace(
            s_state.path, s_state.capture->Bytes(), writeError)) {
        vf::log::Write(
            "renderer-observe: trace publication failed win32=%lu",
            writeError);
        AbortLocked("publication");
        return;
    }

    vf::log::Write(
        "renderer-observe: trace complete frames=%llu views=%llu writers=%llu",
        static_cast<unsigned long long>(inspection.summary.frameCount),
        static_cast<unsigned long long>(inspection.summary.viewCount),
        static_cast<unsigned long long>(
            inspection.summary.writerEventCount));
    s_state.enabled = false;
    s_state.inFrame = false;
    s_state.capture.reset();
}

void OnResize(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t format,
    const std::uint32_t flags) noexcept
{
    ExclusiveLock guard{s_state.lock};
    if (!s_state.enabled || !s_state.capture) {
        return;
    }

    ++s_state.resourceGeneration;
    s_state.width = width;
    s_state.height = height;
    s_state.format = format;
    s_state.viewReady = width != 0 && height != 0;
    s_state.targetId = MakeStableId(
        StableIdDomain::Resource,
        "dxgi/primary/backbuffer",
        s_state.resourceGeneration).value;
    const auto correlation = MakeStableId(
        StableIdDomain::Correlation,
        "dxgi/resize",
        s_state.resourceGeneration).value;
    const ResizeEvent event{
        s_state.inFrame ? s_state.currentFrameId : 0,
        s_state.swapchainId,
        width,
        height,
        format,
        flags,
    };
    if (s_state.capture->RecordResize(event, correlation) !=
        FrameCaptureError::None) {
        AbortLocked("resize");
    }
}

}
