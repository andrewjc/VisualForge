#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace vf::renderer::resource {

enum class ResourceKind : std::uint8_t
{
    VertexBuffer,
    IndexBuffer,
    TriShape
};

enum class ResourceUsage : std::uint8_t
{
    Immutable,
    Dynamic
};

enum class ResourceState : std::uint8_t
{
    Alive,
    Retiring
};

enum class ResourceEventError : std::uint8_t
{
    None,
    NullAddress,
    EmptyResource,
    DuplicateCreate,
    MissingResource,
    StaleHandle,
    ImmutableUpdate,
    ResourceRetiring,
    TimelineRegression,
    GenerationExhausted
};

inline constexpr std::uint32_t kMaximumResourceGeneration = 0xFFFF'FFFFu;

struct ResourceHandle
{
    std::uint64_t address{};
    std::uint32_t generation{};
    ResourceKind kind{ResourceKind::VertexBuffer};

    [[nodiscard]] bool operator==(const ResourceHandle&) const noexcept = default;
};

struct ResourceRecord
{
    ResourceHandle handle{};
    ResourceUsage usage{ResourceUsage::Immutable};
    ResourceState state{ResourceState::Alive};
    std::size_t byteSize{};
    std::uint64_t lastUseValue{};
    std::uint64_t retireValue{};
};

class ResourceRegistry
{
public:
    explicit ResourceRegistry(
        std::uint32_t generationLimit = kMaximumResourceGeneration) noexcept;

    [[nodiscard]] ResourceEventError Create(
        std::uint64_t address,
        ResourceKind kind,
        ResourceUsage usage,
        std::size_t byteSize,
        std::uint64_t submissionValue,
        ResourceHandle& handle);
    [[nodiscard]] ResourceEventError Update(
        ResourceHandle handle,
        std::size_t byteSize,
        std::uint64_t submissionValue);
    [[nodiscard]] ResourceEventError Touch(
        ResourceHandle handle,
        std::uint64_t submissionValue);
    [[nodiscard]] ResourceEventError Destroy(
        ResourceHandle handle,
        std::uint64_t retireValue);
    [[nodiscard]] std::size_t Retire(std::uint64_t completedValue) noexcept;

    [[nodiscard]] std::optional<ResourceRecord> Lookup(
        ResourceHandle handle) const noexcept;
    [[nodiscard]] std::optional<ResourceRecord> Lookup(
        std::uint64_t address,
        ResourceKind kind) const noexcept;
    [[nodiscard]] std::size_t AliveCount() const noexcept;
    [[nodiscard]] std::size_t RetiringCount() const noexcept;

private:
    struct Key
    {
        std::uint64_t address{};
        ResourceKind kind{ResourceKind::VertexBuffer};

        [[nodiscard]] bool operator<(const Key& other) const noexcept;
    };

    [[nodiscard]] ResourceEventError Resolve(
        ResourceHandle handle,
        ResourceRecord*& record) noexcept;

    std::uint32_t generationLimit_{kMaximumResourceGeneration};
    std::map<Key, ResourceRecord> records_;
    std::map<Key, std::uint32_t> generations_;
};

[[nodiscard]] const char* ToString(ResourceEventError error) noexcept;

}
