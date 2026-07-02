#include "renderer_trace/FrameCapture.h"
#include "renderer_trace/TraceCodec.h"
#include "renderer_trace/Crc32.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace vf::renderer::trace;

constexpr std::uint64_t kCaptureId = 0x1122334455667788ull;

int HexValue(const char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

std::vector<std::byte> GoldenMinimalTrace()
{
    constexpr std::string_view hex =
        "564654524143450040000100000000000403020100000000B801000000000000"
        "060000000000000088776655443322119ED2C2134128A1470000000000000000"
        "3800000018000000010001000000000000000000000000007C0FAC9D00000000"
        "8877665544332211809698000000000064000000000000003800000018000000"
        "02000100000000000070000000000000E5F8ACE3000000000700000000000000"
        "6E000000000000002A0000000000000048000000280000000300010000000000"
        "017000000000000074BD1D790000000007000000000000000110000000000000"
        "012000000000000080070000380400001C000000010000004800000028000000"
        "0400010000000000015000000000000017534D63000000000700000000000000"
        "0130000000000000014000000000000001500000000000000100000000000000"
        "40000000200000000600010000000000027000000000000080F627C200000000"
        "0700000000000000780000000000000001000000000000000000000001000000"
        "380000001800000007000100000000000000000000000000412901D400000000"
        "887766554433221101000000000000008200000000000000";

    std::vector<std::byte> result;
    result.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        const auto high = HexValue(hex[index]);
        const auto low = HexValue(hex[index + 1]);
        REQUIRE(high >= 0);
        REQUIRE(low >= 0);
        result.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return result;
}

template <class T>
void WriteLittle(std::vector<std::byte>& bytes, const std::size_t offset, T value)
{
    REQUIRE(offset + sizeof(T) <= bytes.size());
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (static_cast<std::uint64_t>(value) >> (index * 8)) & 0xFFu);
    }
}

void RecomputeHeaderCrc(std::vector<std::byte>& bytes)
{
    REQUIRE(bytes.size() >= kTraceHeaderSize);
    WriteLittle<std::uint32_t>(bytes, 52, 0);
    WriteLittle<std::uint32_t>(
        bytes,
        52,
        Crc32(std::span{bytes}.first(kTraceHeaderSize)));
}

void RecomputePayloadAndHeaderCrc(std::vector<std::byte>& bytes)
{
    REQUIRE(bytes.size() >= kTraceHeaderSize);
    WriteLittle<std::uint32_t>(
        bytes,
        48,
        Crc32(std::span{bytes}.subspan(kTraceHeaderSize)));
    RecomputeHeaderCrc(bytes);
}

void WriteMinimalTrace(TraceWriter& writer)
{
    REQUIRE(writer.Begin({kCaptureId, 10'000'000, 100}) == TraceError::None);
    REQUIRE(writer.Write(FrameBegin{7, 110, 42, 0}, 0x7000) ==
            TraceError::None);
    REQUIRE(writer.Write(
                ViewMetadata{7, 0x1001, 0x2001, 1920, 1080, 28, 1},
                0x7001) == TraceError::None);
    REQUIRE(writer.Write({
                7,
                0x3001,
                0x4001,
                0x5001,
                WriterClassification::World,
                0,
            }, 0x5001) == TraceError::None);
    REQUIRE(writer.Write(FrameEnd{7, 120, 1, 0, 0, 1}, 0x7002) ==
            TraceError::None);
    REQUIRE(writer.Finish({kCaptureId, 1, 130}) == TraceError::None);
}

}

TEST_CASE("P03_writer_matches_the_preexisting_golden_trace", "[unit][phase03]")
{
    TraceWriter writer;
    WriteMinimalTrace(writer);
    REQUIRE(writer.Complete());

    const auto golden = GoldenMinimalTrace();
    CHECK(writer.Bytes().size() == 440);
    CHECK(std::vector<std::byte>(writer.Bytes().begin(), writer.Bytes().end()) ==
          golden);
}

TEST_CASE("P03_trace_serialization_is_byte_deterministic", "[unit][phase03]")
{
    TraceWriter first;
    TraceWriter second;
    WriteMinimalTrace(first);
    WriteMinimalTrace(second);
    CHECK(std::vector<std::byte>(first.Bytes().begin(), first.Bytes().end()) ==
          std::vector<std::byte>(second.Bytes().begin(), second.Bytes().end()));
}

TEST_CASE("P03_reader_inspects_minimal_trace_without_allocating_records", "[unit][phase03]")
{
    const auto golden = GoldenMinimalTrace();
    const auto inspection = InspectTrace(golden);
    REQUIRE(inspection);
    CHECK(inspection.summary.schemaMajor == 1);
    CHECK(inspection.summary.schemaMinor == 0);
    CHECK(inspection.summary.captureId == kCaptureId);
    CHECK(inspection.summary.recordCount == 6);
    CHECK(inspection.summary.frameCount == 1);
    CHECK(inspection.summary.viewCount == 1);
    CHECK(inspection.summary.writerEventCount == 1);
    CHECK(inspection.summary.resizeCount == 0);
    CHECK(inspection.summary.unknownRecordCount == 0);
    CHECK(inspection.summary.firstFrameId == 7);
    CHECK(inspection.summary.lastFrameId == 7);
    CHECK(inspection.summary.renderThreadId == 42);
    // The summary text gained a registry-delta counter in Phase 12. The
    // golden trace bytes are unchanged; only this diagnostic line grew.
    CHECK(FormatTraceSummary(inspection.summary) ==
          "trace schema=1.0 capture=1234605616436508552 records=6 frames=1 "
          "views=1 writers=1 resizes=0 registry-deltas=0 unknown=0 "
          "first-frame=7 last-frame=7 thread=42");
}

TEST_CASE("P03_reader_negotiates_schema_and_endian_marker", "[unit][phase03]")
{
    auto bytes = GoldenMinimalTrace();
    WriteLittle<std::uint16_t>(bytes, 12, 9);
    RecomputeHeaderCrc(bytes);
    auto inspection = InspectTrace(bytes);
    REQUIRE(inspection);
    CHECK(inspection.summary.schemaMinor == 9);

    bytes = GoldenMinimalTrace();
    WriteLittle<std::uint16_t>(bytes, 10, 2);
    RecomputeHeaderCrc(bytes);
    CHECK(InspectTrace(bytes).error == TraceError::UnsupportedSchema);

    bytes = GoldenMinimalTrace();
    WriteLittle<std::uint32_t>(bytes, 16, 0x04030201u);
    RecomputeHeaderCrc(bytes);
    CHECK(InspectTrace(bytes).error == TraceError::WrongEndian);
}

TEST_CASE("P03_reader_rejects_crc_truncation_and_configured_limits", "[unit][phase03]")
{
    auto bytes = GoldenMinimalTrace();
    bytes[40] ^= std::byte{1};
    CHECK(InspectTrace(bytes).error == TraceError::HeaderCrcMismatch);

    bytes = GoldenMinimalTrace();
    bytes[100] ^= std::byte{1};
    CHECK(InspectTrace(bytes).error == TraceError::PayloadCrcMismatch);

    const auto golden = GoldenMinimalTrace();
    CHECK(InspectTrace(std::span{golden}.first(63)).error ==
          TraceError::TruncatedInput);
    CHECK(InspectTrace(std::span{golden}.first(golden.size() - 1)).error ==
          TraceError::FileSizeMismatch);

    CHECK(InspectTrace(golden, TraceLimits{golden.size() - 1, 1024}).error ==
          TraceError::OversizedInput);
    CHECK(InspectTrace(golden, TraceLimits{1024, 8}).error ==
          TraceError::RecordTooLarge);
}

TEST_CASE("P03_unknown_records_skip_and_pointer_flag_is_rejected", "[unit][phase03]")
{
    TraceWriter writer;
    REQUIRE(writer.Begin({9, 1, 2}) == TraceError::None);
    constexpr std::byte payload[]{std::byte{1}, std::byte{2}, std::byte{3}};
    REQUIRE(writer.WriteOpaque(0x8000, 7, 0, 44, payload) == TraceError::None);
    CHECK(writer.WriteOpaque(
              0x8001,
              1,
              static_cast<std::uint32_t>(RecordFlags::ContainsProcessAddress),
              45,
              payload) == TraceError::PointerPayloadRejected);
    REQUIRE(writer.Finish({9, 0, 3}) == TraceError::None);

    const auto inspection = InspectTrace(writer.Bytes());
    REQUIRE(inspection);
    CHECK(inspection.summary.recordCount == 3);
    CHECK(inspection.summary.unknownRecordCount == 1);
}

TEST_CASE("P03_reader_distinguishes_record_crc_pointer_flags_and_padding", "[unit][phase03]")
{
    auto bytes = GoldenMinimalTrace();
    bytes[100] ^= std::byte{1};
    RecomputePayloadAndHeaderCrc(bytes);
    CHECK(InspectTrace(bytes).error == TraceError::RecordCrcMismatch);

    bytes = GoldenMinimalTrace();
    WriteLittle<std::uint32_t>(
        bytes,
        kTraceHeaderSize + 12,
        static_cast<std::uint32_t>(RecordFlags::ContainsProcessAddress));
    RecomputePayloadAndHeaderCrc(bytes);
    CHECK(InspectTrace(bytes).error == TraceError::PointerPayloadRejected);

    TraceWriter writer;
    REQUIRE(writer.Begin({9, 1, 2}) == TraceError::None);
    constexpr std::byte payload[]{std::byte{1}, std::byte{2}, std::byte{3}};
    REQUIRE(writer.WriteOpaque(0x8000, 1, 0, 44, payload) == TraceError::None);
    REQUIRE(writer.Finish({9, 0, 3}) == TraceError::None);
    bytes.assign(writer.Bytes().begin(), writer.Bytes().end());
    REQUIRE(bytes.size() == 216);
    bytes[159] = std::byte{1};
    RecomputePayloadAndHeaderCrc(bytes);
    CHECK(InspectTrace(bytes).error == TraceError::NonZeroPadding);
}

TEST_CASE("P03_writer_arena_limit_fails_without_partial_publication", "[unit][phase03]")
{
    TraceWriter writer(120);
    REQUIRE(writer.Begin({9, 1, 2}) == TraceError::None);
    CHECK(writer.Bytes().size() == 120);
    CHECK(writer.Write(FrameBegin{1, 2, 3, 0}, 4) ==
          TraceError::ArenaLimit);
    CHECK_FALSE(writer.Complete());
    CHECK(writer.Bytes().size() == 120);
}

TEST_CASE("P03_frame_capture_enforces_frame_ownership_and_ids", "[unit][phase03]")
{
    FrameCapture capture;
    CHECK(capture.BeginFrame({1, 2, 3, 0}, 4) ==
          FrameCaptureError::InvalidState);
    REQUIRE(capture.Start({kCaptureId, 10'000'000, 100}) ==
            FrameCaptureError::None);
    REQUIRE(capture.BeginFrame({7, 110, 42, 0}, 0x7000) ==
            FrameCaptureError::None);
    CHECK(capture.SetView({8, 1, 2, 3, 4, 5, 1}, 9) ==
          FrameCaptureError::IdMismatch);
    REQUIRE(capture.SetView({7, 0x1001, 0x2001, 1920, 1080, 28, 1}, 0x7001) ==
            FrameCaptureError::None);
    REQUIRE(capture.RecordWriter({
                7,
                0x3001,
                0x4001,
                0x5001,
                WriterClassification::World,
                0,
            }, 0x5002) == FrameCaptureError::None);
    CHECK(capture.EndFrame({7, 120, 1, 0, 0, 0}, 0x7002) ==
          FrameCaptureError::IdMismatch);
    REQUIRE(capture.EndFrame({7, 120, 1, 0, 0, 1}, 0x7002) ==
            FrameCaptureError::None);
    REQUIRE(capture.Finish(130) == FrameCaptureError::None);

    const auto inspection = InspectTrace(capture.Bytes());
    REQUIRE(inspection);
    CHECK(inspection.summary.frameCount == 1);
    CHECK(inspection.summary.writerEventCount == 1);
}

TEST_CASE("P12_registry_deltas_are_recorded_and_counted_without_addresses",
    "[unit][phase12]")
{
    TraceWriter writer;
    REQUIRE(writer.Begin({kCaptureId, 10'000'000, 100}) == TraceError::None);
    REQUIRE(writer.Write(FrameBegin{7, 110, 42, 0}, 0x7000) ==
        TraceError::None);
    REQUIRE(writer.Write(RegistryDelta{
                7,
                0x0000'0002'0000'0005ull,
                0xABCDull,
                31,
                9,
                2,
                static_cast<std::uint32_t>(RegistryDeltaKind::InstanceAdded),
                0,
            }, 0x8001) == TraceError::None);
    REQUIRE(writer.Write(RegistryDelta{
                7,
                0,
                0xABCDull,
                31,
                9,
                2,
                static_cast<std::uint32_t>(
                    RegistryDeltaKind::GeometryReleased),
                0,
            }, 0x8002) == TraceError::None);
    REQUIRE(writer.Write(FrameEnd{7, 120, 1, 0, 0, 0}, 0x7002) ==
        TraceError::None);
    REQUIRE(writer.Finish({kCaptureId, 1, 130}) == TraceError::None);

    const auto inspection = InspectTrace(writer.Bytes());
    REQUIRE(inspection);
    CHECK(inspection.summary.registryDeltaCount == 2);
    CHECK(inspection.summary.unknownRecordCount == 0);
    CHECK(FormatTraceSummary(inspection.summary).find("registry-deltas=2") !=
        std::string::npos);

    // The record is identity based; a mis-sized payload is never mistaken
    // for a delta.
    TraceWriter malformed;
    REQUIRE(malformed.Begin({kCaptureId, 10'000'000, 100}) ==
        TraceError::None);
    const std::array<std::byte, 8> shortPayload{};
    REQUIRE(malformed.WriteOpaque(
        static_cast<std::uint16_t>(RecordType::RegistryDelta), 1, 0, 0x8003,
        shortPayload) == TraceError::None);
    REQUIRE(malformed.Finish({kCaptureId, 0, 130}) == TraceError::None);
    const auto malformedInspection = InspectTrace(malformed.Bytes());
    REQUIRE(malformedInspection);
    CHECK(malformedInspection.summary.registryDeltaCount == 0);
    CHECK(malformedInspection.summary.unknownRecordCount == 1);
}
