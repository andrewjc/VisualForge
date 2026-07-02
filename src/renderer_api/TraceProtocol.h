#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace vf::renderer::trace {

constexpr std::array<std::byte, 8> kTraceMagic{
    std::byte{0x56}, std::byte{0x46}, std::byte{0x54}, std::byte{0x52},
    std::byte{0x41}, std::byte{0x43}, std::byte{0x45}, std::byte{0x00},
};
constexpr std::uint16_t kSchemaMajor = 1;
constexpr std::uint16_t kSchemaMinor = 0;
constexpr std::uint32_t kEndianMarker = 0x01020304u;
constexpr std::size_t kTraceHeaderSize = 64;
constexpr std::size_t kRecordHeaderSize = 32;
constexpr std::size_t kDefaultMaximumTraceBytes = 64u * 1024u * 1024u;
constexpr std::size_t kDefaultMaximumRecordPayload = 1u * 1024u * 1024u;

enum class RecordType : std::uint16_t
{
    CaptureBegin = 1,
    FrameBegin = 2,
    ViewMetadata = 3,
    WriterEvent = 4,
    ResizeEvent = 5,
    FrameEnd = 6,
    CaptureEnd = 7,
    RegistryDelta = 8
};

// Mirrors the scene database lifecycle transitions. The trace deliberately
// records stable identity and content, never an engine address.
enum class RegistryDeltaKind : std::uint32_t
{
    GroupAttached = 0,
    GroupDetached = 1,
    InstanceAdded = 2,
    InstanceRetired = 3,
    GeometryResident = 4,
    GeometryShared = 5,
    GeometryReleased = 6,
    UploadCancelled = 7
};

enum class RecordFlags : std::uint32_t
{
    None = 0,
    ContainsProcessAddress = 1u << 0
};

enum class WriterClassification : std::uint32_t
{
    Unknown,
    World,
    Post,
    Ui,
    Video,
    Middleware,
    Overlay,
    Bridge
};

struct alignas(8) CaptureBegin
{
    std::uint64_t captureId{};
    std::uint64_t qpcFrequency{};
    std::uint64_t cpuStart{};
};

struct alignas(8) FrameBegin
{
    std::uint64_t frameId{};
    std::uint64_t cpuTimestamp{};
    std::uint32_t renderThreadId{};
    std::uint32_t reserved{};
};

struct alignas(8) ViewMetadata
{
    std::uint64_t frameId{};
    std::uint64_t viewId{};
    std::uint64_t swapchainId{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t format{};
    std::uint32_t sampleCount{};
};

struct alignas(8) WriterEvent
{
    std::uint64_t frameId{};
    std::uint64_t writerId{};
    std::uint64_t targetId{};
    std::uint64_t gpuCorrelationId{};
    WriterClassification classification{WriterClassification::Unknown};
    std::uint32_t ordinal{};
};

struct alignas(8) ResizeEvent
{
    std::uint64_t frameId{};
    std::uint64_t swapchainId{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t format{};
    std::uint32_t flags{};
};

struct alignas(8) FrameEnd
{
    std::uint64_t frameId{};
    std::uint64_t cpuTimestamp{};
    std::uint32_t presentSyncInterval{};
    std::uint32_t presentFlags{};
    std::int32_t presentResult{};
    std::uint32_t writerCount{};
};

struct alignas(8) RegistryDelta
{
    std::uint64_t frameId{};
    std::uint64_t objectId{};
    std::uint64_t contentHash{};
    std::uint64_t byteSize{};
    std::uint64_t timelineValue{};
    std::uint32_t groupId{};
    std::uint32_t generation{};
    std::uint32_t kind{};
    std::uint32_t reserved{};
};

struct alignas(8) CaptureEnd
{
    std::uint64_t captureId{};
    std::uint64_t frameCount{};
    std::uint64_t cpuEnd{};
};

static_assert(sizeof(CaptureBegin) == 24);
static_assert(sizeof(FrameBegin) == 24);
static_assert(sizeof(ViewMetadata) == 40);
static_assert(sizeof(WriterEvent) == 40);
static_assert(sizeof(ResizeEvent) == 32);
static_assert(sizeof(FrameEnd) == 32);
static_assert(sizeof(RegistryDelta) == 56);
static_assert(sizeof(CaptureEnd) == 24);
static_assert(std::is_trivially_copyable_v<CaptureBegin>);
static_assert(std::is_trivially_copyable_v<FrameBegin>);
static_assert(std::is_trivially_copyable_v<ViewMetadata>);
static_assert(std::is_trivially_copyable_v<WriterEvent>);
static_assert(std::is_trivially_copyable_v<ResizeEvent>);
static_assert(std::is_trivially_copyable_v<FrameEnd>);
static_assert(std::is_trivially_copyable_v<RegistryDelta>);
static_assert(std::is_trivially_copyable_v<CaptureEnd>);

}
