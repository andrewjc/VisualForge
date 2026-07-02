#pragma once

#include "renderer_api/TraceProtocol.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vf::renderer::trace {

enum class TraceError : std::uint8_t
{
    None,
    InvalidState,
    ArenaLimit,
    PointerPayloadRejected,
    InvalidMagic,
    UnsupportedSchema,
    WrongEndian,
    TruncatedInput,
    OversizedInput,
    FileSizeMismatch,
    HeaderCrcMismatch,
    PayloadCrcMismatch,
    RecordCrcMismatch,
    RecordTooLarge,
    MalformedRecord,
    NonZeroPadding
};

struct TraceLimits
{
    std::size_t maximumTraceBytes{kDefaultMaximumTraceBytes};
    std::size_t maximumRecordPayload{kDefaultMaximumRecordPayload};
};

class TraceWriter
{
public:
    explicit TraceWriter(
        std::size_t maximumBytes = kDefaultMaximumTraceBytes);

    [[nodiscard]] TraceError Begin(const CaptureBegin& begin);
    [[nodiscard]] TraceError Write(
        const FrameBegin& record,
        std::uint64_t correlationId);
    [[nodiscard]] TraceError Write(
        const ViewMetadata& record,
        std::uint64_t correlationId);
    [[nodiscard]] TraceError Write(
        const WriterEvent& record,
        std::uint64_t cpuCorrelationId);
    [[nodiscard]] TraceError Write(
        const ResizeEvent& record,
        std::uint64_t correlationId);
    [[nodiscard]] TraceError Write(
        const RegistryDelta& record,
        std::uint64_t correlationId);
    [[nodiscard]] TraceError Write(
        const FrameEnd& record,
        std::uint64_t correlationId);
    [[nodiscard]] TraceError WriteOpaque(
        std::uint16_t type,
        std::uint16_t version,
        std::uint32_t flags,
        std::uint64_t correlationId,
        std::span<const std::byte> payload);
    [[nodiscard]] TraceError Finish(const CaptureEnd& end);

    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
    [[nodiscard]] bool Complete() const noexcept;

private:
    std::vector<std::byte> bytes_;
    std::size_t maximumBytes_{};
    std::uint64_t captureId_{};
    std::uint64_t recordCount_{};
    std::uint8_t state_{};
};

struct TraceSummary
{
    std::uint16_t schemaMajor{};
    std::uint16_t schemaMinor{};
    std::uint64_t captureId{};
    std::uint64_t recordCount{};
    std::uint64_t frameCount{};
    std::uint64_t viewCount{};
    std::uint64_t writerEventCount{};
    std::uint64_t resizeCount{};
    std::uint64_t registryDeltaCount{};
    std::uint64_t unknownRecordCount{};
    std::uint64_t firstFrameId{};
    std::uint64_t lastFrameId{};
    std::uint32_t renderThreadId{};
};

struct TraceInspection
{
    TraceSummary summary{};
    TraceError error{TraceError::None};
    std::size_t errorOffset{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == TraceError::None;
    }
};

[[nodiscard]] TraceInspection InspectTrace(
    std::span<const std::byte> bytes,
    TraceLimits limits = {}) noexcept;
[[nodiscard]] const char* ToString(TraceError error) noexcept;
[[nodiscard]] std::string FormatTraceSummary(const TraceSummary& summary);

}
