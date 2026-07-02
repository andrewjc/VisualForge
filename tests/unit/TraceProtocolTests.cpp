#include "renderer_api/TraceProtocol.h"
#include "renderer_trace/Crc32.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

TEST_CASE("P03_trace_protocol_layout_is_fixed_and_pointer_free", "[unit][phase03]")
{
    using namespace vf::renderer::trace;
    CHECK(kTraceHeaderSize == 64);
    CHECK(kRecordHeaderSize == 32);
    CHECK(kEndianMarker == 0x01020304u);
    CHECK(sizeof(CaptureBegin) == 24);
    CHECK(sizeof(FrameBegin) == 24);
    CHECK(sizeof(ViewMetadata) == 40);
    CHECK(sizeof(WriterEvent) == 40);
    CHECK(sizeof(ResizeEvent) == 32);
    CHECK(sizeof(FrameEnd) == 32);
    CHECK(sizeof(CaptureEnd) == 24);
    CHECK(alignof(CaptureBegin) == 8);
    CHECK(alignof(ViewMetadata) == 8);
    CHECK(alignof(WriterEvent) == 8);
}

TEST_CASE("P03_crc32_matches_the_ieee_reference_vector", "[unit][phase03]")
{
    constexpr std::array input{
        std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
        std::byte{'4'}, std::byte{'5'}, std::byte{'6'},
        std::byte{'7'}, std::byte{'8'}, std::byte{'9'},
    };
    CHECK(vf::renderer::trace::Crc32(input) == 0xCBF43926u);
    CHECK(vf::renderer::trace::Crc32({}) == 0u);
}
