#pragma once

#include "renderer_api/TraceProtocol.h"
#include "renderer_core/ResourceRegistry.h"
#include "renderer_core/TextureResidency.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace vf::renderer::scene {

inline constexpr std::uint32_t kDefaultInstanceCapacity = 65'536;
inline constexpr std::uint32_t kDefaultGenerationLimit = 0xFFFF'FFFFu;

enum class DatabaseError : std::uint8_t
{
    None,
    InvalidIdentity,
    InvalidTransform,
    UnknownGroup,
    DuplicateGroup,
    UnknownInstance,
    StaleHandle,
    GenerationExhausted,
    ContentHashConflict,
    GeometryRetiring,
    BudgetExceeded,
    CapacityExceeded,
    UploadNotPending,
    TimelineRegression,
    AllocationFailure,
};

enum class DeltaKind : std::uint8_t
{
    GroupAttached,
    GroupDetached,
    InstanceAdded,
    InstanceRetired,
    GeometryResident,
    GeometryShared,
    GeometryReleased,
    UploadCancelled,
};

struct GeometryDesc
{
    std::uint64_t address{};
    std::uint64_t contentHash{};
    std::uint64_t byteSize{};
    resource::ResourceUsage usage{resource::ResourceUsage::Immutable};
};

struct InstanceHandle
{
    std::uint32_t slot{};
    std::uint32_t generation{};

    [[nodiscard]] bool Valid() const noexcept { return generation != 0; }
    friend bool operator==(const InstanceHandle&, const InstanceHandle&)
        = default;
};

struct InstanceDesc
{
    std::uint64_t sourceId{};
    std::uint64_t materialId{};
    std::uint32_t groupId{};
    std::uint32_t passSequence{};
    std::array<float, 16> model{};
    std::array<float, 4> parameters{1.0f, 1.0f, 1.0f, 1.0f};
};

struct InstanceRecord
{
    std::uint64_t objectId{};
    std::uint64_t sourceId{};
    std::uint64_t materialId{};
    std::uint64_t contentHash{};
    // The canonical geometry the instance renders from, and the engine
    // address it was submitted with. They differ when content is shared.
    std::uint64_t geometryAddress{};
    std::uint64_t aliasAddress{};
    std::uint32_t geometryGeneration{};
    std::uint32_t groupId{};
    std::uint32_t passSequence{};
    std::uint32_t descriptorIndex{};
    bool resident{};
    bool retiring{};
    std::array<float, 16> model{};
    std::array<float, 16> previousModel{};
    std::array<float, 4> parameters{};
};

struct DatabaseStats
{
    std::uint32_t aliveInstances{};
    std::uint32_t retiringInstances{};
    std::uint32_t residentGeometries{};
    std::uint32_t sharedInstances{};
    std::uint32_t pendingUploads{};
    std::uint32_t attachedGroups{};
    std::uint32_t peakDescriptorIndex{};
    std::uint64_t residentBytes{};
};

struct SceneDelta
{
    DeltaKind kind{DeltaKind::GroupAttached};
    std::uint32_t groupId{};
    std::uint32_t generation{};
    std::uint64_t objectId{};
    std::uint64_t contentHash{};
    std::uint64_t byteSize{};
    // Retained for in-process diagnostics only. It is deliberately dropped
    // when a delta is converted into a trace record.
    std::uint64_t geometryAddress{};
    std::uint64_t timelineValue{};
};

// Mirrors the engine's instance/streaming lifecycle without ever
// dereferencing an engine address. Geometry lifetime, descriptor reuse, and
// instance identity are all timeline gated so a late completion or an
// address reuse can never resurrect retired content.
class SceneDatabase
{
public:
    explicit SceneDatabase(
        std::uint64_t byteBudget,
        std::uint32_t instanceCapacity = kDefaultInstanceCapacity,
        std::uint32_t generationLimit = kDefaultGenerationLimit);

    [[nodiscard]] DatabaseError AttachGroup(
        std::uint32_t groupId,
        std::uint64_t timelineValue);
    [[nodiscard]] DatabaseError DetachGroup(
        std::uint32_t groupId,
        std::uint64_t retireValue);
    [[nodiscard]] DatabaseError AddInstance(
        const GeometryDesc& geometry,
        const InstanceDesc& instance,
        std::uint64_t timelineValue,
        InstanceHandle& handle);
    [[nodiscard]] DatabaseError UpdateInstance(
        InstanceHandle handle,
        const std::array<float, 16>& model,
        const std::array<float, 4>& parameters,
        std::uint64_t timelineValue);
    [[nodiscard]] DatabaseError RemoveInstance(
        InstanceHandle handle,
        std::uint64_t retireValue);
    [[nodiscard]] DatabaseError CompleteUpload(
        std::uint64_t geometryAddress,
        std::uint32_t geometryGeneration,
        std::uint64_t completedValue);
    [[nodiscard]] DatabaseError CancelUpload(
        std::uint64_t geometryAddress,
        std::uint32_t geometryGeneration);
    [[nodiscard]] std::size_t Retire(std::uint64_t completedValue) noexcept;

    [[nodiscard]] std::optional<InstanceRecord> Lookup(
        InstanceHandle handle) const noexcept;
    [[nodiscard]] std::vector<InstanceHandle> GroupInstances(
        std::uint32_t groupId) const;
    [[nodiscard]] DatabaseStats Stats() const noexcept;
    [[nodiscard]] std::vector<SceneDelta> DrainDeltas();

private:
    struct GeometryEntry
    {
        std::uint64_t address{};
        std::uint32_t generation{};
        std::uint64_t contentHash{};
        std::uint64_t byteSize{};
        resource::ResourceUsage usage{resource::ResourceUsage::Immutable};
        std::uint32_t references{};
        bool uploadPending{};
        bool resident{};
        bool owned{};
        std::uint64_t canonicalAddress{};
    };

    struct InstanceSlot
    {
        InstanceRecord record{};
        std::uint32_t generation{};
        bool occupied{};
        bool exhausted{};
        std::uint64_t retireValue{};
        std::uint64_t lastUpdateValue{};
    };

    [[nodiscard]] GeometryEntry* FindGeometry(
        std::uint64_t address) noexcept;
    [[nodiscard]] const GeometryEntry* FindGeometry(
        std::uint64_t address) const noexcept;
    [[nodiscard]] GeometryEntry* ResolveCanonical(
        GeometryEntry& entry) noexcept;
    [[nodiscard]] DatabaseError AcquireGeometry(
        const GeometryDesc& geometry,
        std::uint64_t timelineValue,
        std::uint64_t& resolvedAddress,
        std::uint32_t& resolvedGeneration,
        bool& shared);
    void ReleaseGeometry(
        std::uint64_t address,
        std::uint64_t retireValue);
    void RecordDelta(SceneDelta delta);
    void RetireInstance(InstanceSlot& slot, std::uint64_t retireValue);

    std::uint64_t byteBudget_{};
    std::uint32_t instanceCapacity_{};
    std::uint32_t generationLimit_{};
    std::uint64_t residentBytes_{};
    std::uint32_t sharedInstances_{};
    std::uint32_t peakDescriptorIndex_{};
    std::map<std::uint64_t, GeometryEntry> geometries_;
    std::map<std::uint64_t, std::uint64_t> contentIndex_;
    std::map<std::uint32_t, std::uint64_t> groups_;
    std::vector<InstanceSlot> slots_;
    std::vector<SceneDelta> deltas_;
    resource::ResourceRegistry registry_;
    texture::DescriptorQuarantine descriptors_;
};

// The trace record carries stable identity and content only, so a capture
// can never leak or depend on a live engine address.
[[nodiscard]] trace::RegistryDelta ToTraceRecord(
    const SceneDelta& delta,
    std::uint64_t frameId) noexcept;
[[nodiscard]] const char* ToString(DatabaseError error) noexcept;
[[nodiscard]] const char* ToString(DeltaKind kind) noexcept;

static_assert(static_cast<std::uint32_t>(DeltaKind::GroupAttached) ==
    static_cast<std::uint32_t>(trace::RegistryDeltaKind::GroupAttached));
static_assert(static_cast<std::uint32_t>(DeltaKind::GroupDetached) ==
    static_cast<std::uint32_t>(trace::RegistryDeltaKind::GroupDetached));
static_assert(static_cast<std::uint32_t>(DeltaKind::InstanceAdded) ==
    static_cast<std::uint32_t>(trace::RegistryDeltaKind::InstanceAdded));
static_assert(static_cast<std::uint32_t>(DeltaKind::InstanceRetired) ==
    static_cast<std::uint32_t>(trace::RegistryDeltaKind::InstanceRetired));
static_assert(static_cast<std::uint32_t>(DeltaKind::GeometryResident) ==
    static_cast<std::uint32_t>(trace::RegistryDeltaKind::GeometryResident));
static_assert(static_cast<std::uint32_t>(DeltaKind::GeometryShared) ==
    static_cast<std::uint32_t>(trace::RegistryDeltaKind::GeometryShared));
static_assert(static_cast<std::uint32_t>(DeltaKind::GeometryReleased) ==
    static_cast<std::uint32_t>(trace::RegistryDeltaKind::GeometryReleased));
static_assert(static_cast<std::uint32_t>(DeltaKind::UploadCancelled) ==
    static_cast<std::uint32_t>(trace::RegistryDeltaKind::UploadCancelled));

}
