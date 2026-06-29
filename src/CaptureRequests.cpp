#include "CaptureRequests.h"

#include "EngineCameraCapture.h"
#include "EngineMeshCapture.h"
#include "EngineTextureCapture.h"
#include "Log.h"
#include "RendererObservation.h"
#include "renderer_host/CaptureControl.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace vf::capture_requests {

namespace {

// Polling walks the file system on the render thread, so it runs on a
// coarse frame interval rather than every present.
constexpr unsigned kPollFrameInterval = 15;

struct RequestState
{
    vf::renderer::capture::RequestGate gate;
    std::wstring path;
    unsigned frame{};
    bool enabled{};
};

RequestState s_state;

std::wstring Widen(const std::string& value)
{
    if (value.empty()) return {};
    const auto required = MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0);
    if (required <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    const auto written = MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        wide.data(), required);
    if (written != required) return {};
    return wide;
}

std::wstring RequestPath()
{
    const auto required = GetEnvironmentVariableW(
        L"VISUALFORGE_CAPTURE_REQUEST", nullptr, 0);
    if (required <= 1) return {};
    std::wstring value(required, L'\0');
    const auto written = GetEnvironmentVariableW(
        L"VISUALFORGE_CAPTURE_REQUEST", value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    return value;
}

// Reads the whole document with sharing enabled so a harness writing the
// next request can never be blocked by the game holding the handle.
bool ReadDocument(const std::wstring& path, std::string& document) noexcept
{
    document.clear();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) >
            vf::renderer::capture::kMaximumRequestBytes) {
        CloseHandle(file);
        return false;
    }
    try {
        document.resize(static_cast<std::size_t>(size.QuadPart));
    } catch (...) {
        CloseHandle(file);
        return false;
    }
    DWORD read{};
    const auto ok = ReadFile(file, document.data(),
        static_cast<DWORD>(document.size()), &read, nullptr) &&
        read == document.size();
    CloseHandle(file);
    if (!ok) document.clear();
    return ok;
}

void Dispatch(const vf::renderer::capture::CaptureRequest& request) noexcept
{
    using vf::renderer::capture::CaptureKind;
    using vf::renderer::capture::CaptureOutcome;
    const auto wide = Widen(request.path);
    auto outcome = CaptureOutcome::Rejected;
    const char* detail = "unsupported-kind";
    if (wide.empty()) {
        detail = "path-encoding";
    } else {
        switch (request.kind) {
        case CaptureKind::Mesh:
            if (!vf::engine_mesh_capture::Enabled()) {
                detail = "mesh-capture-not-installed";
            } else if (vf::engine_mesh_capture::Arm(wide.c_str())) {
                outcome = CaptureOutcome::Complete;
                detail = "armed";
            } else {
                detail = "mesh-capture-busy";
            }
            break;
        case CaptureKind::Texture:
            if (!vf::engine_texture_capture::Enabled()) {
                detail = "texture-capture-not-installed";
            } else if (vf::engine_texture_capture::Arm(wide.c_str())) {
                outcome = CaptureOutcome::Complete;
                detail = "armed";
            } else {
                detail = "texture-capture-busy";
            }
            break;
        case CaptureKind::Trace:
            // Enabled() means "a trace is recording right now", so it is the
            // wrong precondition for arming a new one. ArmTrace owns the
            // decision and refuses only while a trace is in flight.
            if (vf::renderer_observation::ArmTrace(wide.c_str())) {
                outcome = CaptureOutcome::Complete;
                detail = "armed";
            } else {
                detail = "trace-busy";
            }
            break;
        case CaptureKind::Frame:
            if (!vf::engine_camera_capture::Enabled()) {
                detail = "camera-capture-not-installed";
            } else if (vf::engine_camera_capture::Arm(wide.c_str())) {
                outcome = CaptureOutcome::Complete;
                detail = "armed";
            } else {
                detail = "camera-capture-busy";
            }
            break;
        case CaptureKind::Scene:
        case CaptureKind::Deformation:
            detail = "kind-not-implemented";
            break;
        case CaptureKind::None:
            break;
        }
    }
    vf::log::Write("%s", vf::renderer::capture::FormatResult(
        request, outcome, detail).c_str());
}

}

bool Configure() noexcept
{
    try {
        s_state.path = RequestPath();
        s_state.enabled = !s_state.path.empty();
        if (s_state.enabled) {
            vf::log::Write(
                "renderer-capture-control: polling interval=%u frames "
                "suppression=off", kPollFrameInterval);
        }
        return s_state.enabled;
    } catch (...) {
        s_state.enabled = false;
        return false;
    }
}

bool Enabled() noexcept
{
    return s_state.enabled;
}

void Poll() noexcept
{
    if (!s_state.enabled) return;
    if (++s_state.frame % kPollFrameInterval != 0) return;
    try {
        std::string document;
        if (!ReadDocument(s_state.path, document)) return;
        vf::renderer::capture::CaptureRequest request{};
        const auto accepted = s_state.gate.Accept(document, request);
        if (accepted == vf::renderer::capture::RequestError::StaleSequence) {
            return;
        }
        if (accepted != vf::renderer::capture::RequestError::None) {
            vf::log::Write(
                "renderer-capture-request: rejected reason=%s suppression=off",
                vf::renderer::capture::ToString(accepted));
            return;
        }
        Dispatch(request);
    } catch (...) {
        // A capture request must never take the frame down.
    }
}

}
