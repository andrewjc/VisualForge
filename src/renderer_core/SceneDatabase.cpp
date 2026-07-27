#include "renderer_core/SceneDatabase.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vf::renderer::scene {

namespace {

constexpr float kAffineTolerance = 1.0e-5f;

bool FiniteAffine(const std::array<float, 16>& matrix) noexcept
{
    if (!std::all_of(matrix.begin(), matrix.end(),
            [](const float value) { return std::isfinite(value); })) {
        return false;
    }
    if (std::abs(matrix[12]) > kAffineTolerance ||
        std::abs(matrix[13]) > kAffineTolerance ||
        std::abs(matrix[14]) > kAffineTolerance ||
        std::abs(matrix[15] - 1.0f) > kAffineTolerance) {
        return false;
    }
    const double determinant =
        static_cast<double>(matrix[0]) *
            (static_cast<double>(matrix[5]) * matrix[10] -
             static_cast<double>(matrix[6]) * matrix[9]) -
        static_cast<double>(matrix[1]) *
            (static_cast<double>(matrix[4]) * matrix[10] -
             static_cast<double>(matrix[6]) * matrix[8]) +
        static_cast<double>(matrix[2]) *
            (static_cast<double>(matrix[4]) * matrix[9] -
             static_cast<double>(matrix[5]) * matrix[8]);
    return std::isfinite(determinant) && std::abs(determinant) > 1.0e-10;
}

bool FiniteParameters(const std::array<float, 4>& parameters) noexcept
{
    return std::all_of(parameters.begin(), parameters.end(),
        [](const float value) { return std::isfinite(value); });
}

}

SceneDatabase::SceneDatabase(
    const std::uint64_t byteBudget,
    const std::uint32_t instanceCapacity,
    const std::uint32_t generationLimit)
    : byteBudget_(byteBudget)
    , instanceCapacity_(instanceCapacity == 0 ? 1 : instanceCapacity)
    , generationLimit_(generationLimit == 0 ? 1 : generationLimit)
    , registry_(generationLimit == 0 ? 1 : generationLimit)
    // Descriptor index 0 is permanently valid, so the quarantine is sized
    // one larger than the instance capacity it has to serve.
    , descriptors_((instanceCapacity == 0 ? 1 : instanceCapacity) + 1)
{
    slots_.resize(instanceCapacity_);
}

SceneDatabase::GeometryEntry* SceneDatabase::FindGeometry(
    const std::uint64_t address) noexcept
{
    const auto found = geometries_.find(address);
    return found == geometries_.end() ? nullptr : &found->second;
}

const SceneDatabase::GeometryEntry* SceneDatabase::FindGeometry(
    const std::uint64_t address) const noexcept
{
    const auto found = geometries_.find(address);
    return found == geometries_.end() ? nullptr : &found->second;
}

SceneDatabase::GeometryEntry* SceneDatabase::ResolveCanonical(
    GeometryEntry& entry) noexcept
{
    if (entry.owned) return &entry;
    return FindGeometry(entry.canonicalAddress);
}

void SceneDatabase::RecordDelta(SceneDelta delta)
{
    try {
        deltas_.push_back(delta);
    } catch (...) {
        // Delta reporting is diagnostic. Losing a record must never break
        // the lifetime bookkeeping it describes.
    }
}

DatabaseError SceneDatabase::AttachGroup(
    const std::uint32_t groupId,
    const std::uint64_t timelineValue)
{
    if (groupId == 0) return DatabaseError::InvalidIdentity;
    if (groups_.contains(groupId)) return DatabaseError::DuplicateGroup;
    try {
        groups_.emplace(groupId, timelineValue);
    } catch (...) {
        return DatabaseError::AllocationFailure;
    }
    RecordDelta({.kind = DeltaKind::GroupAttached, .groupId = groupId,
        .timelineValue = timelineValue});
    return DatabaseError::None;
}

DatabaseError SceneDatabase::DetachGroup(
    const std::uint32_t groupId,
    const std::uint64_t retireValue)
{
    const auto group = groups_.find(groupId);
    if (group == groups_.end()) return DatabaseError::UnknownGroup;
    groups_.erase(group);
    for (auto& slot : slots_) {
        if (!slot.occupied || slot.record.retiring ||
            slot.record.groupId != groupId) {
            continue;
        }
        RetireInstance(slot, retireValue);
    }
    RecordDelta({.kind = DeltaKind::GroupDetached, .groupId = groupId,
        .timelineValue = retireValue});
    return DatabaseError::None;
}

DatabaseError SceneDatabase::AcquireGeometry(
    const GeometryDesc& geometry,
    const std::uint64_t timelineValue,
    std::uint64_t& resolvedAddress,
    std::uint32_t& resolvedGeneration,
    bool& shared)
{
    resolvedAddress = geometry.address;
    resolvedGeneration = 0;
    shared = false;
    if (auto* existing = FindGeometry(geometry.address); existing != nullptr) {
        const auto record = registry_.Lookup(
            resource::ResourceHandle{existing->address, existing->generation,
                resource::ResourceKind::TriShape});
        if (!record.has_value() ||
            record->state == resource::ResourceState::Retiring) {
            return DatabaseError::GeometryRetiring;
        }
        auto* canonical = ResolveCanonical(*existing);
        if (canonical == nullptr) return DatabaseError::AllocationFailure;
        if (canonical->contentHash != geometry.contentHash ||
            canonical->usage != geometry.usage) {
            return DatabaseError::ContentHashConflict;
        }
        ++existing->references;
        if (!existing->owned) {
            ++canonical->references;
            ++sharedInstances_;
            shared = true;
        }
        resolvedGeneration = existing->generation;
        return DatabaseError::None;
    }

    // Only immutable content may be folded together. Two independently
    // mutable resources can hash identically today and diverge next frame.
    if (geometry.usage == resource::ResourceUsage::Immutable) {
        const auto canonicalAddress = contentIndex_.find(geometry.contentHash);
        if (canonicalAddress != contentIndex_.end()) {
            if (auto* canonical = FindGeometry(canonicalAddress->second);
                canonical != nullptr && canonical->owned &&
                canonical->references != 0) {
                GeometryEntry alias{};
                alias.address = geometry.address;
                alias.generation = canonical->generation;
                alias.contentHash = geometry.contentHash;
                alias.byteSize = 0;
                alias.usage = geometry.usage;
                alias.references = 1;
                alias.owned = false;
                alias.canonicalAddress = canonical->address;
                try {
                    geometries_.emplace(geometry.address, alias);
                } catch (...) {
                    return DatabaseError::AllocationFailure;
                }
                ++canonical->references;
                ++sharedInstances_;
                shared = true;
                resolvedAddress = canonical->address;
                resolvedGeneration = canonical->generation;
                RecordDelta({.kind = DeltaKind::GeometryShared,
                    .generation = canonical->generation,
                    .contentHash = geometry.contentHash,
                    .byteSize = canonical->byteSize,
                    .geometryAddress = geometry.address,
                    .timelineValue = timelineValue});
                return DatabaseError::None;
            }
        }
    }

    if (geometry.byteSize > byteBudget_ ||
        residentBytes_ > byteBudget_ - geometry.byteSize) {
        return DatabaseError::BudgetExceeded;
    }
    resource::ResourceHandle handle{};
    const auto created = registry_.Create(geometry.address,
        resource::ResourceKind::TriShape, geometry.usage,
        static_cast<std::size_t>(geometry.byteSize), timelineValue, handle);
    if (created == resource::ResourceEventError::GenerationExhausted) {
        return DatabaseError::GenerationExhausted;
    }
    if (created != resource::ResourceEventError::None) {
        return DatabaseError::ContentHashConflict;
    }
    GeometryEntry owned{};
    owned.address = geometry.address;
    owned.generation = handle.generation;
    owned.contentHash = geometry.contentHash;
    owned.byteSize = geometry.byteSize;
    owned.usage = geometry.usage;
    owned.references = 1;
    owned.uploadPending = true;
    owned.owned = true;
    try {
        geometries_.emplace(geometry.address, owned);
        if (geometry.usage == resource::ResourceUsage::Immutable) {
            contentIndex_[geometry.contentHash] = geometry.address;
        }
    } catch (...) {
        geometries_.erase(geometry.address);
        static_cast<void>(registry_.Destroy(handle, timelineValue));
        static_cast<void>(registry_.Retire(timelineValue));
        return DatabaseError::AllocationFailure;
    }
    residentBytes_ += geometry.byteSize;
    resolvedGeneration = handle.generation;
    return DatabaseError::None;
}

void SceneDatabase::ReleaseGeometry(
    const std::uint64_t address,
    const std::uint64_t retireValue)
{
    auto* entry = FindGeometry(address);
    if (entry == nullptr || entry->references == 0) return;
    --entry->references;
    auto* canonical = ResolveCanonical(*entry);
    if (!entry->owned) {
        --sharedInstances_;
        if (entry->references == 0) {
            geometries_.erase(address);
        }
        if (canonical != nullptr && canonical->references != 0) {
            --canonical->references;
        }
    }
    if (canonical == nullptr || canonical->references != 0) return;
    if (canonical->uploadPending) {
        // An unload that overtakes its own upload cancels it instead of
        // letting a late completion publish retired content.
        canonical->uploadPending = false;
        RecordDelta({.kind = DeltaKind::UploadCancelled,
            .generation = canonical->generation,
            .contentHash = canonical->contentHash,
            .byteSize = canonical->byteSize,
            .geometryAddress = canonical->address,
            .timelineValue = retireValue});
    }
    const resource::ResourceHandle handle{canonical->address,
        canonical->generation, resource::ResourceKind::TriShape};
    static_cast<void>(registry_.Destroy(handle, retireValue));
}

void SceneDatabase::RetireInstance(
    InstanceSlot& slot,
    const std::uint64_t retireValue)
{
    slot.record.retiring = true;
    slot.retireValue = retireValue;
    static_cast<void>(descriptors_.Retire(
        slot.record.descriptorIndex, retireValue));
    ReleaseGeometry(slot.record.aliasAddress, retireValue);
    RecordDelta({.kind = DeltaKind::InstanceRetired,
        .groupId = slot.record.groupId,
        .generation = slot.record.geometryGeneration,
        .objectId = slot.record.objectId,
        .contentHash = slot.record.contentHash,
        .geometryAddress = slot.record.aliasAddress,
        .timelineValue = retireValue});
}

DatabaseError SceneDatabase::AddInstance(
    const GeometryDesc& geometry,
    const InstanceDesc& instance,
    const std::uint64_t timelineValue,
    InstanceHandle& handle)
{
    handle = {};
    if (geometry.address == 0 || geometry.byteSize == 0 ||
        instance.sourceId == 0 || instance.materialId == 0 ||
        instance.passSequence == 0) {
        return DatabaseError::InvalidIdentity;
    }
    if (!FiniteAffine(instance.model) ||
        !FiniteParameters(instance.parameters)) {
        return DatabaseError::InvalidTransform;
    }
    if (!groups_.contains(instance.groupId)) {
        return DatabaseError::UnknownGroup;
    }

    std::uint32_t slotIndex = instanceCapacity_;
    bool sawExhausted = false;
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].occupied) continue;
        if (slots_[index].exhausted) {
            sawExhausted = true;
            continue;
        }
        slotIndex = index;
        break;
    }
    if (slotIndex == instanceCapacity_) {
        return sawExhausted ? DatabaseError::GenerationExhausted
                            : DatabaseError::CapacityExceeded;
    }
    auto& slot = slots_[slotIndex];
    if (slot.generation >= generationLimit_) {
        slot.exhausted = true;
        return DatabaseError::GenerationExhausted;
    }

    const auto descriptor = descriptors_.Acquire();
    if (!descriptor.has_value()) return DatabaseError::CapacityExceeded;

    std::uint64_t resolvedAddress{};
    std::uint32_t resolvedGeneration{};
    bool shared = false;
    const auto acquired = AcquireGeometry(geometry, timelineValue,
        resolvedAddress, resolvedGeneration, shared);
    if (acquired != DatabaseError::None) {
        // Nothing referenced the descriptor, so it returns to the free list
        // without a quarantine round trip.
        static_cast<void>(descriptors_.Release(*descriptor));
        return acquired;
    }

    ++slot.generation;
    slot.occupied = true;
    slot.retireValue = 0;
    slot.lastUpdateValue = timelineValue;
    auto& record = slot.record;
    record = {};
    record.objectId = (static_cast<std::uint64_t>(slot.generation) << 32) |
        (static_cast<std::uint64_t>(slotIndex) + 1);
    record.sourceId = instance.sourceId;
    record.materialId = instance.materialId;
    record.contentHash = geometry.contentHash;
    record.geometryAddress = resolvedAddress;
    record.aliasAddress = geometry.address;
    record.geometryGeneration = resolvedGeneration;
    record.groupId = instance.groupId;
    record.passSequence = instance.passSequence;
    record.descriptorIndex = *descriptor;
    record.retiring = false;
    record.model = instance.model;
    // A first observation has no motion history of its own.
    record.previousModel = instance.model;
    record.parameters = instance.parameters;
    peakDescriptorIndex_ = std::max(peakDescriptorIndex_, *descriptor);
    handle = InstanceHandle{slotIndex, slot.generation};
    RecordDelta({.kind = DeltaKind::InstanceAdded,
        .groupId = instance.groupId,
        .generation = resolvedGeneration,
        .objectId = record.objectId,
        .contentHash = geometry.contentHash,
        .byteSize = geometry.byteSize,
        .geometryAddress = geometry.address,
        .timelineValue = timelineValue});
    return DatabaseError::None;
}

DatabaseError SceneDatabase::UpdateInstance(
    const InstanceHandle handle,
    const std::array<float, 16>& model,
    const std::array<float, 4>& parameters,
    const std::uint64_t timelineValue)
{
    if (handle.slot >= slots_.size()) return DatabaseError::UnknownInstance;
    auto& slot = slots_[handle.slot];
    if (!slot.occupied || slot.generation != handle.generation ||
        slot.record.retiring) {
        return DatabaseError::StaleHandle;
    }
    if (!FiniteAffine(model) || !FiniteParameters(parameters)) {
        return DatabaseError::InvalidTransform;
    }
    if (timelineValue < slot.lastUpdateValue) {
        return DatabaseError::TimelineRegression;
    }
    slot.record.previousModel = slot.record.model;
    slot.record.model = model;
    slot.record.parameters = parameters;
    slot.lastUpdateValue = timelineValue;
    return DatabaseError::None;
}

DatabaseError SceneDatabase::RemoveInstance(
    const InstanceHandle handle,
    const std::uint64_t retireValue)
{
    if (handle.slot >= slots_.size()) return DatabaseError::UnknownInstance;
    auto& slot = slots_[handle.slot];
    if (!slot.occupied || slot.generation != handle.generation ||
        slot.record.retiring) {
        return DatabaseError::StaleHandle;
    }
    RetireInstance(slot, retireValue);
    return DatabaseError::None;
}

DatabaseError SceneDatabase::CompleteUpload(
    const std::uint64_t geometryAddress,
    const std::uint32_t geometryGeneration,
    const std::uint64_t completedValue)
{
    auto* entry = FindGeometry(geometryAddress);
    if (entry == nullptr) return DatabaseError::StaleHandle;
    auto* canonical = ResolveCanonical(*entry);
    if (canonical == nullptr) return DatabaseError::StaleHandle;
    if (canonical->generation != geometryGeneration) {
        return DatabaseError::StaleHandle;
    }
    if (!canonical->uploadPending) return DatabaseError::UploadNotPending;
    canonical->uploadPending = false;
    canonical->resident = true;
    const resource::ResourceHandle handle{canonical->address,
        canonical->generation, resource::ResourceKind::TriShape};
    static_cast<void>(registry_.Touch(handle, completedValue));
    RecordDelta({.kind = DeltaKind::GeometryResident,
        .generation = canonical->generation,
        .contentHash = canonical->contentHash,
        .byteSize = canonical->byteSize,
        .geometryAddress = canonical->address,
        .timelineValue = completedValue});
    return DatabaseError::None;
}

DatabaseError SceneDatabase::CancelUpload(
    const std::uint64_t geometryAddress,
    const std::uint32_t geometryGeneration)
{
    auto* entry = FindGeometry(geometryAddress);
    if (entry == nullptr) return DatabaseError::StaleHandle;
    auto* canonical = ResolveCanonical(*entry);
    if (canonical == nullptr) return DatabaseError::StaleHandle;
    if (canonical->generation != geometryGeneration) {
        return DatabaseError::StaleHandle;
    }
    if (!canonical->uploadPending) return DatabaseError::UploadNotPending;
    canonical->uploadPending = false;
    RecordDelta({.kind = DeltaKind::UploadCancelled,
        .generation = canonical->generation,
        .contentHash = canonical->contentHash,
        .byteSize = canonical->byteSize,
        .geometryAddress = canonical->address});
    return DatabaseError::None;
}

std::size_t SceneDatabase::Retire(const std::uint64_t completedValue) noexcept
{
    std::size_t retired = 0;
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        if (!slot.occupied || !slot.record.retiring ||
            slot.retireValue > completedValue) {
            continue;
        }
        slot.occupied = false;
        slot.record = {};
        slot.retireValue = 0;
        slot.lastUpdateValue = 0;
        if (slot.generation >= generationLimit_) {
            slot.exhausted = true;
        }
        ++retired;
    }
    descriptors_.Advance(completedValue);
    static_cast<void>(registry_.Retire(completedValue));
    for (auto iterator = geometries_.begin();
         iterator != geometries_.end();) {
        const auto& entry = iterator->second;
        if (!entry.owned) {
            ++iterator;
            continue;
        }
        const auto record = registry_.Lookup(resource::ResourceHandle{
            entry.address, entry.generation,
            resource::ResourceKind::TriShape});
        if (record.has_value()) {
            ++iterator;
            continue;
        }
        residentBytes_ -= std::min(residentBytes_, entry.byteSize);
        const auto content = contentIndex_.find(entry.contentHash);
        if (content != contentIndex_.end() &&
            content->second == entry.address) {
            contentIndex_.erase(content);
        }
        RecordDelta({.kind = DeltaKind::GeometryReleased,
            .generation = entry.generation,
            .contentHash = entry.contentHash,
            .byteSize = entry.byteSize,
            .geometryAddress = entry.address,
            .timelineValue = completedValue});
        iterator = geometries_.erase(iterator);
    }
    return retired;
}

std::optional<InstanceRecord> SceneDatabase::Lookup(
    const InstanceHandle handle) const noexcept
{
    if (handle.slot >= slots_.size()) return std::nullopt;
    const auto& slot = slots_[handle.slot];
    if (!slot.occupied || slot.generation != handle.generation) {
        return std::nullopt;
    }
    auto record = slot.record;
    if (const auto* entry = FindGeometry(record.geometryAddress);
        entry != nullptr) {
        record.resident = entry->resident;
    }
    return record;
}

std::vector<InstanceHandle> SceneDatabase::GroupInstances(
    const std::uint32_t groupId) const
{
    std::vector<InstanceHandle> handles;
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        const auto& slot = slots_[index];
        if (!slot.occupied || slot.record.retiring ||
            slot.record.groupId != groupId) {
            continue;
        }
        handles.push_back(InstanceHandle{index, slot.generation});
    }
    return handles;
}

DatabaseStats SceneDatabase::Stats() const noexcept
{
    DatabaseStats stats{};
    for (const auto& slot : slots_) {
        if (!slot.occupied) continue;
        if (slot.record.retiring) {
            ++stats.retiringInstances;
        } else {
            ++stats.aliveInstances;
        }
    }
    for (const auto& [address, entry] : geometries_) {
        static_cast<void>(address);
        if (!entry.owned) continue;
        ++stats.residentGeometries;
        if (entry.uploadPending) ++stats.pendingUploads;
    }
    stats.sharedInstances = sharedInstances_;
    stats.attachedGroups = static_cast<std::uint32_t>(groups_.size());
    stats.peakDescriptorIndex = peakDescriptorIndex_;
    stats.residentBytes = residentBytes_;
    return stats;
}

std::vector<SceneDelta> SceneDatabase::DrainDeltas()
{
    auto drained = std::move(deltas_);
    deltas_.clear();
    return drained;
}

trace::RegistryDelta ToTraceRecord(
    const SceneDelta& delta,
    const std::uint64_t frameId) noexcept
{
    trace::RegistryDelta record{};
    record.frameId = frameId;
    record.objectId = delta.objectId;
    record.contentHash = delta.contentHash;
    record.byteSize = delta.byteSize;
    record.timelineValue = delta.timelineValue;
    record.groupId = delta.groupId;
    record.generation = delta.generation;
    record.kind = static_cast<std::uint32_t>(delta.kind);
    return record;
}

const char* ToString(const DatabaseError error) noexcept
{
    switch (error) {
    case DatabaseError::None: return "none";
    case DatabaseError::InvalidIdentity: return "invalid identity";
    case DatabaseError::InvalidTransform: return "invalid transform";
    case DatabaseError::UnknownGroup: return "unknown group";
    case DatabaseError::DuplicateGroup: return "duplicate group";
    case DatabaseError::UnknownInstance: return "unknown instance";
    case DatabaseError::StaleHandle: return "stale handle";
    case DatabaseError::GenerationExhausted: return "generation exhausted";
    case DatabaseError::ContentHashConflict: return "content hash conflict";
    case DatabaseError::GeometryRetiring: return "geometry retiring";
    case DatabaseError::BudgetExceeded: return "budget exceeded";
    case DatabaseError::CapacityExceeded: return "capacity exceeded";
    case DatabaseError::UploadNotPending: return "upload not pending";
    case DatabaseError::TimelineRegression: return "timeline regression";
    case DatabaseError::AllocationFailure: return "allocation failure";
    }
    return "unknown";
}

const char* ToString(const DeltaKind kind) noexcept
{
    switch (kind) {
    case DeltaKind::GroupAttached: return "group-attached";
    case DeltaKind::GroupDetached: return "group-detached";
    case DeltaKind::InstanceAdded: return "instance-added";
    case DeltaKind::InstanceRetired: return "instance-retired";
    case DeltaKind::GeometryResident: return "geometry-resident";
    case DeltaKind::GeometryShared: return "geometry-shared";
    case DeltaKind::GeometryReleased: return "geometry-released";
    case DeltaKind::UploadCancelled: return "upload-cancelled";
    }
    return "unknown";
}

}
