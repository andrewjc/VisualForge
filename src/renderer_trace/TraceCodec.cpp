#include "renderer_trace/TraceCodec.h"

#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <array>
#include <limits>

namespace vf::renderer::trace {

namespace {

constexpr std::uint8_t kWriterIdle = 0;
constexpr std::uint8_t kWriterOpen = 1;
constexpr std::uint8_t kWriterComplete = 2;

std::size_t AlignToEight(const std::size_t value) noexcept
{
    return (value + 7u) & ~std::size_t{7u};
}

void PutU16(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint16_t value) noexcept
{
    bytes[offset] = static_cast<std::byte>(value & 0xFFu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFFu);
}

void PutU32(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint32_t value) noexcept
{
    for (unsigned index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8)) & 0xFFu);
    }
}

void PutU64(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint64_t value) noexcept
{
    for (unsigned index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8)) & 0xFFu);
    }
}

std::uint16_t GetU16(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t GetU32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept
{
    auto result = std::uint32_t{0};
    for (unsigned index = 0; index < 4; ++index) {
        result |= std::to_integer<std::uint32_t>(bytes[offset + index]) <<
            (index * 8);
    }
    return result;
}

std::uint64_t GetU64(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept
{
    auto result = std::uint64_t{0};
    for (unsigned index = 0; index < 8; ++index) {
        result |= std::to_integer<std::uint64_t>(bytes[offset + index]) <<
            (index * 8);
    }
    return result;
}

template <std::size_t Size>
std::array<std::byte, Size> EmptyPayload() noexcept
{
    return {};
}

std::array<std::byte, sizeof(CaptureBegin)> Encode(
    const CaptureBegin& value) noexcept
{
    auto bytes = EmptyPayload<sizeof(CaptureBegin)>();
    PutU64(bytes, 0, value.captureId);
    PutU64(bytes, 8, value.qpcFrequency);
    PutU64(bytes, 16, value.cpuStart);
    return bytes;
}

std::array<std::byte, sizeof(FrameBegin)> Encode(
    const FrameBegin& value) noexcept
{
    auto bytes = EmptyPayload<sizeof(FrameBegin)>();
    PutU64(bytes, 0, value.frameId);
    PutU64(bytes, 8, value.cpuTimestamp);
    PutU32(bytes, 16, value.renderThreadId);
    PutU32(bytes, 20, value.reserved);
    return bytes;
}

std::array<std::byte, sizeof(ViewMetadata)> Encode(
    const ViewMetadata& value) noexcept
{
    auto bytes = EmptyPayload<sizeof(ViewMetadata)>();
    PutU64(bytes, 0, value.frameId);
    PutU64(bytes, 8, value.viewId);
    PutU64(bytes, 16, value.swapchainId);
    PutU32(bytes, 24, value.width);
    PutU32(bytes, 28, value.height);
    PutU32(bytes, 32, value.format);
    PutU32(bytes, 36, value.sampleCount);
    return bytes;
}

std::array<std::byte, sizeof(WriterEvent)> Encode(
    const WriterEvent& value) noexcept
{
    auto bytes = EmptyPayload<sizeof(WriterEvent)>();
    PutU64(bytes, 0, value.frameId);
    PutU64(bytes, 8, value.writerId);
    PutU64(bytes, 16, value.targetId);
    PutU64(bytes, 24, value.gpuCorrelationId);
    PutU32(bytes, 32, static_cast<std::uint32_t>(value.classification));
    PutU32(bytes, 36, value.ordinal);
    return bytes;
}

std::array<std::byte, sizeof(ResizeEvent)> Encode(
    const ResizeEvent& value) noexcept
{
    auto bytes = EmptyPayload<sizeof(ResizeEvent)>();
    PutU64(bytes, 0, value.frameId);
    PutU64(bytes, 8, value.swapchainId);
    PutU32(bytes, 16, value.width);
    PutU32(bytes, 20, value.height);
    PutU32(bytes, 24, value.format);
    PutU32(bytes, 28, value.flags);
    return bytes;
}

std::array<std::byte, sizeof(RegistryDelta)> Encode(
    const RegistryDelta& value) noexcept
{
    auto bytes = EmptyPayload<sizeof(RegistryDelta)>();
    PutU64(bytes, 0, value.frameId);
    PutU64(bytes, 8, value.objectId);
    PutU64(bytes, 16, value.contentHash);
    PutU64(bytes, 24, value.byteSize);
    PutU64(bytes, 32, value.timelineValue);
    PutU32(bytes, 40, value.groupId);
    PutU32(bytes, 44, value.generation);
    PutU32(bytes, 48, value.kind);
    PutU32(bytes, 52, value.reserved);
    return bytes;
}

std::array<std::byte, sizeof(FrameEnd)> Encode(
    const FrameEnd& value) noexcept
{
    auto bytes = EmptyPayload<sizeof(FrameEnd)>();
    PutU64(bytes, 0, value.frameId);
    PutU64(bytes, 8, value.cpuTimestamp);
    PutU32(bytes, 16, value.presentSyncInterval);
    PutU32(bytes, 20, value.presentFlags);
    PutU32(bytes, 24, static_cast<std::uint32_t>(value.presentResult));
    PutU32(bytes, 28, value.writerCount);
    return bytes;
}

std::array<std::byte, sizeof(CaptureEnd)> Encode(
    const CaptureEnd& value) noexcept
{
    auto bytes = EmptyPayload<sizeof(CaptureEnd)>();
    PutU64(bytes, 0, value.captureId);
    PutU64(bytes, 8, value.frameCount);
    PutU64(bytes, 16, value.cpuEnd);
    return bytes;
}

TraceInspection Failure(
    const TraceError error,
    const std::size_t offset = 0) noexcept
{
    return TraceInspection{{}, error, offset};
}

bool HasProcessAddressFlag(const std::uint32_t flags) noexcept
{
    return (flags &
        static_cast<std::uint32_t>(RecordFlags::ContainsProcessAddress)) != 0;
}

}

TraceWriter::TraceWriter(const std::size_t maximumBytes)
    : maximumBytes_(maximumBytes)
{}

TraceError TraceWriter::Begin(const CaptureBegin& begin)
{
    if (state_ != kWriterIdle || begin.captureId == 0 ||
        begin.qpcFrequency == 0 || maximumBytes_ < kTraceHeaderSize) {
        return TraceError::InvalidState;
    }

    try {
        bytes_.clear();
        bytes_.reserve(std::min(maximumBytes_, std::size_t{4096}));
        bytes_.resize(kTraceHeaderSize);
    } catch (...) {
        bytes_.clear();
        return TraceError::ArenaLimit;
    }

    std::copy(kTraceMagic.begin(), kTraceMagic.end(), bytes_.begin());
    captureId_ = begin.captureId;
    recordCount_ = 0;
    state_ = kWriterOpen;
    const auto payload = Encode(begin);
    const auto result = WriteOpaque(
        static_cast<std::uint16_t>(RecordType::CaptureBegin),
        1,
        0,
        0,
        payload);
    if (result != TraceError::None) {
        bytes_.clear();
        captureId_ = 0;
        recordCount_ = 0;
        state_ = kWriterIdle;
    }
    return result;
}

TraceError TraceWriter::Write(
    const FrameBegin& record,
    const std::uint64_t correlationId)
{
    const auto payload = Encode(record);
    return WriteOpaque(
        static_cast<std::uint16_t>(RecordType::FrameBegin),
        1,
        0,
        correlationId,
        payload);
}

TraceError TraceWriter::Write(
    const ViewMetadata& record,
    const std::uint64_t correlationId)
{
    const auto payload = Encode(record);
    return WriteOpaque(
        static_cast<std::uint16_t>(RecordType::ViewMetadata),
        1,
        0,
        correlationId,
        payload);
}

TraceError TraceWriter::Write(
    const WriterEvent& record,
    const std::uint64_t cpuCorrelationId)
{
    const auto payload = Encode(record);
    return WriteOpaque(
        static_cast<std::uint16_t>(RecordType::WriterEvent),
        1,
        0,
        cpuCorrelationId,
        payload);
}

TraceError TraceWriter::Write(
    const ResizeEvent& record,
    const std::uint64_t correlationId)
{
    const auto payload = Encode(record);
    return WriteOpaque(
        static_cast<std::uint16_t>(RecordType::ResizeEvent),
        1,
        0,
        correlationId,
        payload);
}

TraceError TraceWriter::Write(
    const RegistryDelta& record,
    const std::uint64_t correlationId)
{
    const auto payload = Encode(record);
    return WriteOpaque(
        static_cast<std::uint16_t>(RecordType::RegistryDelta),
        1,
        0,
        correlationId,
        payload);
}

TraceError TraceWriter::Write(
    const FrameEnd& record,
    const std::uint64_t correlationId)
{
    const auto payload = Encode(record);
    return WriteOpaque(
        static_cast<std::uint16_t>(RecordType::FrameEnd),
        1,
        0,
        correlationId,
        payload);
}

TraceError TraceWriter::WriteOpaque(
    const std::uint16_t type,
    const std::uint16_t version,
    const std::uint32_t flags,
    const std::uint64_t correlationId,
    const std::span<const std::byte> payload)
{
    if (state_ != kWriterOpen || type == 0 || version == 0) {
        return TraceError::InvalidState;
    }
    if (HasProcessAddressFlag(flags)) {
        return TraceError::PointerPayloadRejected;
    }
    if (payload.size() > kDefaultMaximumRecordPayload ||
        payload.size() >
            std::numeric_limits<std::uint32_t>::max() - kRecordHeaderSize) {
        return TraceError::RecordTooLarge;
    }

    const auto unalignedSize = kRecordHeaderSize + payload.size();
    const auto totalSize = AlignToEight(unalignedSize);
    if (bytes_.size() > maximumBytes_ ||
        totalSize > maximumBytes_ - bytes_.size()) {
        return TraceError::ArenaLimit;
    }

    const auto start = bytes_.size();
    try {
        bytes_.resize(start + totalSize);
    } catch (...) {
        return TraceError::ArenaLimit;
    }

    const auto record = std::span{bytes_}.subspan(start, totalSize);
    PutU32(record, 0, static_cast<std::uint32_t>(totalSize));
    PutU32(record, 4, static_cast<std::uint32_t>(payload.size()));
    PutU16(record, 8, type);
    PutU16(record, 10, version);
    PutU32(record, 12, flags);
    PutU64(record, 16, correlationId);
    PutU32(record, 24, Crc32(payload));
    PutU32(record, 28, 0);
    std::copy(payload.begin(), payload.end(), record.begin() + kRecordHeaderSize);
    ++recordCount_;
    return TraceError::None;
}

TraceError TraceWriter::Finish(const CaptureEnd& end)
{
    if (state_ != kWriterOpen || end.captureId != captureId_) {
        return TraceError::InvalidState;
    }
    const auto payload = Encode(end);
    const auto append = WriteOpaque(
        static_cast<std::uint16_t>(RecordType::CaptureEnd),
        1,
        0,
        0,
        payload);
    if (append != TraceError::None) {
        return append;
    }

    auto header = std::span{bytes_}.first(kTraceHeaderSize);
    PutU16(header, 8, static_cast<std::uint16_t>(kTraceHeaderSize));
    PutU16(header, 10, kSchemaMajor);
    PutU16(header, 12, kSchemaMinor);
    PutU16(header, 14, 0);
    PutU32(header, 16, kEndianMarker);
    PutU32(header, 20, 0);
    PutU64(header, 24, static_cast<std::uint64_t>(bytes_.size()));
    PutU64(header, 32, recordCount_);
    PutU64(header, 40, captureId_);
    PutU32(header, 48, Crc32(std::span{bytes_}.subspan(kTraceHeaderSize)));
    PutU32(header, 52, 0);
    PutU64(header, 56, 0);
    PutU32(header, 52, Crc32(header));
    state_ = kWriterComplete;
    return TraceError::None;
}

std::span<const std::byte> TraceWriter::Bytes() const noexcept
{
    return bytes_;
}

bool TraceWriter::Complete() const noexcept
{
    return state_ == kWriterComplete;
}

TraceInspection InspectTrace(
    const std::span<const std::byte> bytes,
    const TraceLimits limits) noexcept
{
    if (bytes.size() > limits.maximumTraceBytes) {
        return Failure(TraceError::OversizedInput);
    }
    if (bytes.size() < kTraceHeaderSize) {
        return Failure(TraceError::TruncatedInput, bytes.size());
    }
    if (!std::equal(kTraceMagic.begin(), kTraceMagic.end(), bytes.begin())) {
        return Failure(TraceError::InvalidMagic);
    }
    if (GetU16(bytes, 8) != kTraceHeaderSize) {
        return Failure(TraceError::MalformedRecord, 8);
    }
    const auto schemaMajor = GetU16(bytes, 10);
    const auto schemaMinor = GetU16(bytes, 12);
    if (schemaMajor != kSchemaMajor) {
        return Failure(TraceError::UnsupportedSchema, 10);
    }
    if (GetU32(bytes, 16) != kEndianMarker) {
        return Failure(TraceError::WrongEndian, 16);
    }
    if (GetU16(bytes, 14) != 0 || GetU32(bytes, 20) != 0 ||
        GetU64(bytes, 56) != 0) {
        return Failure(TraceError::MalformedRecord);
    }

    const auto declaredSize = GetU64(bytes, 24);
    if (declaredSize > limits.maximumTraceBytes) {
        return Failure(TraceError::OversizedInput, 24);
    }
    if (declaredSize != bytes.size()) {
        return Failure(TraceError::FileSizeMismatch, 24);
    }

    std::array<std::byte, kTraceHeaderSize> header{};
    std::copy_n(bytes.begin(), kTraceHeaderSize, header.begin());
    const auto expectedHeaderCrc = GetU32(bytes, 52);
    PutU32(header, 52, 0);
    if (Crc32(header) != expectedHeaderCrc) {
        return Failure(TraceError::HeaderCrcMismatch, 52);
    }
    if (Crc32(bytes.subspan(kTraceHeaderSize)) != GetU32(bytes, 48)) {
        return Failure(TraceError::PayloadCrcMismatch, 48);
    }

    const auto declaredRecordCount = GetU64(bytes, 32);
    if (declaredRecordCount >
        (bytes.size() - kTraceHeaderSize) / kRecordHeaderSize) {
        return Failure(TraceError::MalformedRecord, 32);
    }

    TraceSummary summary;
    summary.schemaMajor = schemaMajor;
    summary.schemaMinor = schemaMinor;
    summary.captureId = GetU64(bytes, 40);
    summary.recordCount = declaredRecordCount;

    auto offset = kTraceHeaderSize;
    for (std::uint64_t recordIndex = 0;
         recordIndex < declaredRecordCount;
         ++recordIndex) {
        if (offset > bytes.size() ||
            kRecordHeaderSize > bytes.size() - offset) {
            return Failure(TraceError::TruncatedInput, offset);
        }
        const auto record = bytes.subspan(offset);
        const auto totalSize = static_cast<std::size_t>(GetU32(record, 0));
        const auto payloadSize = static_cast<std::size_t>(GetU32(record, 4));
        const auto type = GetU16(record, 8);
        const auto version = GetU16(record, 10);
        const auto flags = GetU32(record, 12);
        if (payloadSize > limits.maximumRecordPayload) {
            return Failure(TraceError::RecordTooLarge, offset + 4);
        }
        if (totalSize < kRecordHeaderSize ||
            totalSize != AlignToEight(kRecordHeaderSize + payloadSize) ||
            (totalSize & 7u) != 0 ||
            totalSize > bytes.size() - offset ||
            GetU32(record, 28) != 0) {
            return Failure(TraceError::MalformedRecord, offset);
        }
        if (HasProcessAddressFlag(flags)) {
            return Failure(TraceError::PointerPayloadRejected, offset + 12);
        }

        const auto payload = record.subspan(kRecordHeaderSize, payloadSize);
        if (Crc32(payload) != GetU32(record, 24)) {
            return Failure(TraceError::RecordCrcMismatch, offset + 24);
        }
        for (const auto padding :
             record.subspan(kRecordHeaderSize + payloadSize,
                            totalSize - kRecordHeaderSize - payloadSize)) {
            if (padding != std::byte{0}) {
                return Failure(TraceError::NonZeroPadding, offset);
            }
        }

        bool known = version == 1;
        if (known) {
            switch (static_cast<RecordType>(type)) {
            case RecordType::CaptureBegin:
                known = payloadSize == sizeof(CaptureBegin);
                if (known && GetU64(payload, 0) != summary.captureId) {
                    return Failure(TraceError::MalformedRecord, offset);
                }
                break;
            case RecordType::FrameBegin:
                known = payloadSize == sizeof(FrameBegin);
                if (known) {
                    const auto frameId = GetU64(payload, 0);
                    if (summary.frameCount == 0) {
                        summary.firstFrameId = frameId;
                    }
                    summary.lastFrameId = frameId;
                    summary.renderThreadId = GetU32(payload, 16);
                    ++summary.frameCount;
                }
                break;
            case RecordType::ViewMetadata:
                known = payloadSize == sizeof(ViewMetadata);
                if (known) {
                    ++summary.viewCount;
                }
                break;
            case RecordType::WriterEvent:
                known = payloadSize == sizeof(WriterEvent);
                if (known) {
                    ++summary.writerEventCount;
                }
                break;
            case RecordType::ResizeEvent:
                known = payloadSize == sizeof(ResizeEvent);
                if (known) {
                    ++summary.resizeCount;
                }
                break;
            case RecordType::RegistryDelta:
                known = payloadSize == sizeof(RegistryDelta);
                if (known) {
                    ++summary.registryDeltaCount;
                }
                break;
            case RecordType::FrameEnd:
                known = payloadSize == sizeof(FrameEnd);
                break;
            case RecordType::CaptureEnd:
                known = payloadSize == sizeof(CaptureEnd);
                if (known && GetU64(payload, 0) != summary.captureId) {
                    return Failure(TraceError::MalformedRecord, offset);
                }
                break;
            default:
                known = false;
                break;
            }
        }
        if (!known) {
            ++summary.unknownRecordCount;
        }
        offset += totalSize;
    }

    if (offset != bytes.size()) {
        return Failure(TraceError::MalformedRecord, offset);
    }
    return TraceInspection{summary, TraceError::None, 0};
}

const char* ToString(const TraceError error) noexcept
{
    switch (error) {
    case TraceError::None:
        return "none";
    case TraceError::InvalidState:
        return "invalid-state";
    case TraceError::ArenaLimit:
        return "arena-limit";
    case TraceError::PointerPayloadRejected:
        return "pointer-payload-rejected";
    case TraceError::InvalidMagic:
        return "invalid-magic";
    case TraceError::UnsupportedSchema:
        return "unsupported-schema";
    case TraceError::WrongEndian:
        return "wrong-endian";
    case TraceError::TruncatedInput:
        return "truncated-input";
    case TraceError::OversizedInput:
        return "oversized-input";
    case TraceError::FileSizeMismatch:
        return "file-size-mismatch";
    case TraceError::HeaderCrcMismatch:
        return "header-crc-mismatch";
    case TraceError::PayloadCrcMismatch:
        return "payload-crc-mismatch";
    case TraceError::RecordCrcMismatch:
        return "record-crc-mismatch";
    case TraceError::RecordTooLarge:
        return "record-too-large";
    case TraceError::MalformedRecord:
        return "malformed-record";
    case TraceError::NonZeroPadding:
        return "non-zero-padding";
    }
    return "unknown";
}

std::string FormatTraceSummary(const TraceSummary& summary)
{
    std::string text = "trace schema=";
    text += std::to_string(summary.schemaMajor);
    text.push_back('.');
    text += std::to_string(summary.schemaMinor);
    text += " capture=";
    text += std::to_string(summary.captureId);
    text += " records=";
    text += std::to_string(summary.recordCount);
    text += " frames=";
    text += std::to_string(summary.frameCount);
    text += " views=";
    text += std::to_string(summary.viewCount);
    text += " writers=";
    text += std::to_string(summary.writerEventCount);
    text += " resizes=";
    text += std::to_string(summary.resizeCount);
    text += " registry-deltas=";
    text += std::to_string(summary.registryDeltaCount);
    text += " unknown=";
    text += std::to_string(summary.unknownRecordCount);
    text += " first-frame=";
    text += std::to_string(summary.firstFrameId);
    text += " last-frame=";
    text += std::to_string(summary.lastFrameId);
    text += " thread=";
    text += std::to_string(summary.renderThreadId);
    return text;
}

}
