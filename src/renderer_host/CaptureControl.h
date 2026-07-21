#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vf::renderer::capture {

// Upper bound on a request document. The file is written by the capture
// harness while the game is running, so a truncated or runaway write must be
// rejected rather than parsed.
inline constexpr std::size_t kMaximumRequestBytes = 4096;

enum class CaptureKind : std::uint8_t
{
    None,
    Mesh,
    Texture,
    Trace,
    Frame,
    Scene,
    Deformation,
};

enum class RequestError : std::uint8_t
{
    None,
    Empty,
    TooLarge,
    MalformedLine,
    UnknownField,
    MissingSequence,
    MissingKind,
    MissingPath,
    UnknownKind,
    InvalidPath,
    StaleSequence,
};

enum class CaptureOutcome : std::uint8_t
{
    Complete,
    Rejected,
};

struct CaptureRequest
{
    std::uint64_t sequence{};
    CaptureKind kind{CaptureKind::None};
    std::string path;
};

// Accepts capture requests from a polled document. The gate is the only
// place that decides a request is new, so a one-shot capture can never be
// re-armed by the same document being read again.
class RequestGate
{
public:
    [[nodiscard]] RequestError Accept(
        std::string_view document,
        CaptureRequest& request) noexcept;
    [[nodiscard]] std::uint64_t LastSequence() const noexcept;

private:
    std::uint64_t lastSequence_{};
};

[[nodiscard]] std::string FormatResult(
    const CaptureRequest& request,
    CaptureOutcome outcome,
    std::string_view detail);
[[nodiscard]] const char* ToString(CaptureKind kind) noexcept;
[[nodiscard]] const char* ToString(RequestError error) noexcept;
[[nodiscard]] const char* ExpectedExtension(CaptureKind kind) noexcept;

}
