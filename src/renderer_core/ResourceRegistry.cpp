#include "renderer_core/ResourceRegistry.h"

#include <algorithm>
#include <tuple>

namespace vf::renderer::resource {

bool ResourceRegistry::Key::operator<(const Key& other) const noexcept
{
    return std::tie(address, kind) < std::tie(other.address, other.kind);
}

ResourceRegistry::ResourceRegistry(
    const std::uint32_t generationLimit) noexcept
    : generationLimit_(generationLimit == 0 ? 1 : generationLimit)
{}

ResourceEventError ResourceRegistry::Create(
    const std::uint64_t address,
    const ResourceKind kind,
    const ResourceUsage usage,
    const std::size_t byteSize,
    const std::uint64_t submissionValue,
    ResourceHandle& handle)
{
    handle = {};
    if (address == 0) {
        return ResourceEventError::NullAddress;
    }
    if (byteSize == 0) {
        return ResourceEventError::EmptyResource;
    }

    const Key key{address, kind};
    if (records_.contains(key)) {
        return ResourceEventError::DuplicateCreate;
    }

    auto& generation = generations_[key];
    // A wrapped generation would let a stale handle alias a live resource.
    if (generation >= generationLimit_) {
        return ResourceEventError::GenerationExhausted;
    }
    ++generation;
    handle = ResourceHandle{address, generation, kind};
    records_.emplace(
        key,
        ResourceRecord{
            handle,
            usage,
            ResourceState::Alive,
            byteSize,
            submissionValue,
            0,
        });
    return ResourceEventError::None;
}

ResourceEventError ResourceRegistry::Resolve(
    const ResourceHandle handle,
    ResourceRecord*& record) noexcept
{
    record = nullptr;
    if (handle.address == 0 || handle.generation == 0) {
        return ResourceEventError::MissingResource;
    }
    const Key key{handle.address, handle.kind};
    const auto found = records_.find(key);
    if (found == records_.end()) {
        const auto generation = generations_.find(key);
        if (generation != generations_.end() &&
            generation->second != handle.generation) {
            return ResourceEventError::StaleHandle;
        }
        return ResourceEventError::MissingResource;
    }
    if (found->second.handle.generation != handle.generation) {
        return ResourceEventError::StaleHandle;
    }
    record = &found->second;
    return ResourceEventError::None;
}

ResourceEventError ResourceRegistry::Update(
    const ResourceHandle handle,
    const std::size_t byteSize,
    const std::uint64_t submissionValue)
{
    ResourceRecord* record{};
    const auto resolved = Resolve(handle, record);
    if (resolved != ResourceEventError::None) {
        return resolved;
    }
    if (record->state == ResourceState::Retiring) {
        return ResourceEventError::ResourceRetiring;
    }
    if (record->usage == ResourceUsage::Immutable) {
        return ResourceEventError::ImmutableUpdate;
    }
    if (byteSize == 0) {
        return ResourceEventError::EmptyResource;
    }
    if (submissionValue < record->lastUseValue) {
        return ResourceEventError::TimelineRegression;
    }
    record->byteSize = byteSize;
    record->lastUseValue = submissionValue;
    return ResourceEventError::None;
}

ResourceEventError ResourceRegistry::Touch(
    const ResourceHandle handle,
    const std::uint64_t submissionValue)
{
    ResourceRecord* record{};
    const auto resolved = Resolve(handle, record);
    if (resolved != ResourceEventError::None) {
        return resolved;
    }
    if (record->state == ResourceState::Retiring) {
        return ResourceEventError::ResourceRetiring;
    }
    if (submissionValue < record->lastUseValue) {
        return ResourceEventError::TimelineRegression;
    }
    record->lastUseValue = submissionValue;
    return ResourceEventError::None;
}

ResourceEventError ResourceRegistry::Destroy(
    const ResourceHandle handle,
    const std::uint64_t retireValue)
{
    ResourceRecord* record{};
    const auto resolved = Resolve(handle, record);
    if (resolved != ResourceEventError::None) {
        return resolved;
    }
    if (record->state == ResourceState::Retiring) {
        return ResourceEventError::ResourceRetiring;
    }
    record->state = ResourceState::Retiring;
    record->retireValue = std::max(retireValue, record->lastUseValue);
    return ResourceEventError::None;
}

std::size_t ResourceRegistry::Retire(const std::uint64_t completedValue) noexcept
{
    std::size_t retired = 0;
    for (auto iterator = records_.begin(); iterator != records_.end();) {
        if (iterator->second.state == ResourceState::Retiring &&
            iterator->second.retireValue <= completedValue) {
            iterator = records_.erase(iterator);
            ++retired;
        } else {
            ++iterator;
        }
    }
    return retired;
}

std::optional<ResourceRecord> ResourceRegistry::Lookup(
    const ResourceHandle handle) const noexcept
{
    const auto found = records_.find(Key{handle.address, handle.kind});
    if (found == records_.end() ||
        found->second.handle.generation != handle.generation) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<ResourceRecord> ResourceRegistry::Lookup(
    const std::uint64_t address,
    const ResourceKind kind) const noexcept
{
    const auto found = records_.find(Key{address, kind});
    if (found == records_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::size_t ResourceRegistry::AliveCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        records_.begin(), records_.end(), [](const auto& entry) {
            return entry.second.state == ResourceState::Alive;
        }));
}

std::size_t ResourceRegistry::RetiringCount() const noexcept
{
    return records_.size() - AliveCount();
}

const char* ToString(const ResourceEventError error) noexcept
{
    switch (error) {
    case ResourceEventError::None: return "none";
    case ResourceEventError::NullAddress: return "null-address";
    case ResourceEventError::EmptyResource: return "empty-resource";
    case ResourceEventError::DuplicateCreate: return "duplicate-create";
    case ResourceEventError::MissingResource: return "missing-resource";
    case ResourceEventError::StaleHandle: return "stale-handle";
    case ResourceEventError::ImmutableUpdate: return "immutable-update";
    case ResourceEventError::ResourceRetiring: return "resource-retiring";
    case ResourceEventError::TimelineRegression: return "timeline-regression";
    case ResourceEventError::GenerationExhausted:
        return "generation-exhausted";
    }
    return "unknown";
}

}
