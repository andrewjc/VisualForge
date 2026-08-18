#include "renderer_core/EngineAcceleration.h"

#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace vf::renderer::accel {

namespace {

// Bytes a build consumes per triangle, per geometry, and as a fixed header.
// These are the mirror's own reservation policy, not a device query: a real
// build queries the driver, and this contract exists so the reservation is
// deterministic and testable offline. The backend still asks Vulkan.
constexpr std::uint64_t kBytesPerTriangle = 64;
constexpr std::uint64_t kBytesPerGeometry = 256;
constexpr std::uint64_t kStructureHeaderBytes = 1024;
constexpr std::uint64_t kScratchPerTriangle = 32;
constexpr std::uint64_t kUpdateScratchPerTriangle = 8;

[[nodiscard]] bool PowerOfTwo(const std::uint32_t value) noexcept
{
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] std::uint64_t AlignUp(
    const std::uint64_t value,
    const std::uint32_t alignment) noexcept
{
    const auto mask = static_cast<std::uint64_t>(alignment) - 1;
    return (value + mask) & ~mask;
}

[[nodiscard]] bool Finite(const std::array<float, 3>& values) noexcept
{
    return std::all_of(values.begin(), values.end(),
        [](const float value) { return std::isfinite(value); });
}

[[nodiscard]] std::uint64_t TriangleCount(const BlasDescV1& blas) noexcept
{
    std::uint64_t triangles = 0;
    for (const auto& geometry : blas.geometries) {
        triangles += geometry.indexCount / 3;
    }
    return triangles;
}

}

AccelError QueryBuildSizes(
    const BlasDescV1& blas,
    const DeviceLimits& limits,
    BuildSizes& sizes) noexcept
{
    sizes = {};
    // Alignments are rounded to by masking, which only works for powers of
    // two, and every device reports one.
    if (!PowerOfTwo(limits.scratchAlignment) ||
        !PowerOfTwo(limits.structureAlignment)) {
        return AccelError::InvalidAlignment;
    }
    if (blas.geometries.empty()) return AccelError::EmptyGeometry;

    const auto triangles = TriangleCount(blas);
    const auto structure = kStructureHeaderBytes +
        triangles * kBytesPerTriangle +
        blas.geometries.size() * kBytesPerGeometry;
    sizes.structureBytes = AlignUp(structure, limits.structureAlignment);
    sizes.buildScratchBytes = AlignUp(
        std::max<std::uint64_t>(triangles * kScratchPerTriangle, 1),
        limits.scratchAlignment);
    // A static structure is never refitted, so it needs no update scratch.
    sizes.updateScratchBytes = blas.dynamic
        ? AlignUp(std::max<std::uint64_t>(
              triangles * kUpdateScratchPerTriangle, 1),
              limits.scratchAlignment)
        : 0;
    return AccelError::None;
}

BuildMode DecideBuild(
    const BlasDescV1& previous,
    const BlasDescV1& next) noexcept
{
    // A structure that has never been built has nothing to refit, and a
    // static one is never refitted whatever changed.
    if (previous.blasId == 0 || !next.dynamic || !previous.dynamic) {
        return BuildMode::Rebuild;
    }
    if (previous.geometries.size() != next.geometries.size()) {
        return BuildMode::Rebuild;
    }
    for (std::size_t index = 0; index < next.geometries.size(); ++index) {
        const auto& before = previous.geometries[index];
        const auto& after = next.geometries[index];
        // Refitting a structure whose triangle count changed produces one
        // that no longer matches its geometry, and the corruption is silent:
        // rays simply miss.
        if (before.indexCount != after.indexCount ||
            before.vertexCount != after.vertexCount ||
            before.geometryId != after.geometryId ||
            before.firstIndex != after.firstIndex ||
            before.vertexOffset != after.vertexOffset ||
            // Opacity decides which hit groups run, so it is part of the
            // structure's flags and cannot be refitted either.
            before.opacity != after.opacity) {
            return BuildMode::Rebuild;
        }
    }
    return BuildMode::Update;
}

bool ShouldCompact(const BlasDescV1& blas) noexcept
{
    // Compacting a structure that is refitted every frame costs more than it
    // saves: it would have to be rebuilt to be compacted again.
    return !blas.dynamic;
}

float InstanceDeterminant(const InstanceDescV1& instance) noexcept
{
    const auto& m = instance.transform;
    return m[0] * (m[5] * m[10] - m[6] * m[9]) -
        m[1] * (m[4] * m[10] - m[6] * m[8]) +
        m[2] * (m[4] * m[9] - m[5] * m[8]);
}

AccelError ValidateInstance(const InstanceDescV1& instance) noexcept
{
    if (instance.instanceId == 0 || instance.blasId == 0) {
        return AccelError::InvalidIdentity;
    }
    if (!std::all_of(instance.transform.begin(), instance.transform.end(),
            [](const float value) { return std::isfinite(value); })) {
        return AccelError::NonFiniteSource;
    }
    // A zero mask makes the instance invisible to every ray, which is almost
    // always a capture bug rather than an intent.
    if (instance.mask == 0 || instance.mask > kInstanceMaskAll) {
        return AccelError::InvalidMask;
    }
    // The custom index is 24 bits in the Vulkan instance record; a larger
    // one truncates silently and points at the wrong material.
    if (instance.customIndex > kMaximumCustomIndex) {
        return AccelError::CustomIndexOutOfRange;
    }
    const auto determinant = InstanceDeterminant(instance);
    // A mirrored instance is legal and keeps its winding reversal; only a
    // singular transform, which collapses the instance to zero volume, is an
    // error.
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-9f) {
        return AccelError::SingularTransform;
    }
    return AccelError::None;
}

AccelError TransformBounds(
    const BlasDescV1& blas,
    const InstanceDescV1& instance,
    Bounds& worldBounds) noexcept
{
    worldBounds = {};
    if (!Finite(blas.boundsMinimum) || !Finite(blas.boundsMaximum)) {
        return AccelError::NonFiniteSource;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (blas.boundsMinimum[axis] > blas.boundsMaximum[axis]) {
            return AccelError::InvalidBounds;
        }
    }
    const auto validated = ValidateInstance(instance);
    if (validated != AccelError::None) return validated;

    // A rotated box's axis-aligned bounds are not its bounds rotated, so all
    // eight corners are transformed. Anything less clips geometry that is
    // really inside the bound.
    auto minimum = std::array<float, 3>{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    auto maximum = std::array<float, 3>{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
    for (int corner = 0; corner < 8; ++corner) {
        const std::array<float, 3> local{
            (corner & 1) ? blas.boundsMaximum[0] : blas.boundsMinimum[0],
            (corner & 2) ? blas.boundsMaximum[1] : blas.boundsMinimum[1],
            (corner & 4) ? blas.boundsMaximum[2] : blas.boundsMinimum[2]};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto value =
                instance.transform[axis * 4 + 0] * local[0] +
                instance.transform[axis * 4 + 1] * local[1] +
                instance.transform[axis * 4 + 2] * local[2] +
                instance.transform[axis * 4 + 3];
            minimum[axis] = std::min(minimum[axis], value);
            maximum[axis] = std::max(maximum[axis], value);
        }
    }
    worldBounds.minimum = minimum;
    worldBounds.maximum = maximum;
    return AccelError::None;
}

bool RequiresAnyHit(const GeometryDescV1& geometry) noexcept
{
    return geometry.opacity != GeometryOpacity::Opaque;
}

bool ParticipatesInShadows(const GeometryDescV1& geometry) noexcept
{
    // Blended geometry is classified but casts no ray-traced shadow in this
    // phase. Letting it default to an opaque occluder would put a solid
    // shadow under every pane of glass.
    return geometry.opacity != GeometryOpacity::Blended;
}

bool ConfirmAlphaCandidate(
    const visibility::AlphaStateV1& alpha,
    const float sampledAlpha,
    const std::uint32_t pixelX,
    const std::uint32_t pixelY) noexcept
{
    visibility::CoverageContext context{};
    context.pixelX = pixelX;
    context.pixelY = pixelY;
    // Deliberately the same function the raster pass uses. A second alpha
    // test would drift, and a shadow silhouette that disagreed with the
    // surface silhouette is exactly the artefact this phase avoids.
    return visibility::EvaluateCoverage(alpha, sampledAlpha, context).covered;
}

std::array<float, 3> OffsetRayOrigin(
    const std::array<float, 3>& position,
    const std::array<float, 3>& geometricNormal,
    const float scale) noexcept
{
    auto normal = geometricNormal;
    const auto lengthSquared = normal[0] * normal[0] +
        normal[1] * normal[1] + normal[2] * normal[2];
    // A degenerate normal cannot offset in any direction, so the origin is
    // returned unchanged rather than moved somewhere arbitrary.
    if (!(lengthSquared > 0.0f) || !std::isfinite(lengthSquared)) {
        return position;
    }
    const auto inverse = 1.0f / std::sqrt(lengthSquared);
    for (auto& component : normal) component *= inverse;

    // Float spacing grows with magnitude, so a fixed epsilon that works near
    // the camera is invisible far away. The offset therefore scales with the
    // point's own distance from the origin.
    const auto magnitude = std::sqrt(position[0] * position[0] +
        position[1] * position[1] + position[2] * position[2]);
    constexpr float kBaseEpsilon = 1.0e-3f;
    constexpr float kRelativeEpsilon = 1.0e-5f;
    const auto offset =
        scale * (kBaseEpsilon + kRelativeEpsilon * magnitude);
    return {position[0] + normal[0] * offset,
        position[1] + normal[1] * offset,
        position[2] + normal[2] * offset};
}

ShadowRay ShadowRayForLight(
    const lighting::GpuLightRecordV1& light,
    const std::array<float, 3>& position,
    const std::array<float, 3>& geometricNormal) noexcept
{
    ShadowRay ray{};
    // The offset origin, never the surface point itself: a ray starting on
    // the surface re-hits the triangle that spawned it, and every lit pixel
    // shadows itself.
    ray.origin = OffsetRayOrigin(position, geometricNormal, 1.0f);
    ray.minimumDistance = 0.0f;

    const auto type = lighting::ClassifyGpuLight(light);
    if (type == lighting::LightType::Directional) {
        // The record stores the direction the light travels, so the vector
        // toward it is the negation.
        ray.direction = {-light.direction[0], -light.direction[1],
            -light.direction[2]};
        ray.maximumDistance = kDirectionalShadowDistance;
        return ray;
    }

    // Measured from the offset origin rather than the surface point. The ray
    // starts there, so measuring from anywhere else would either leave a
    // sliver beside the light untraced or reach past it.
    const std::array<float, 3> toLight{
        light.position[0] - ray.origin[0],
        light.position[1] - ray.origin[1],
        light.position[2] - ray.origin[2]};
    const auto distance = std::sqrt(toLight[0] * toLight[0] +
        toLight[1] * toLight[1] + toLight[2] * toLight[2]);
    if (!(distance > 0.0f) || !std::isfinite(distance)) {
        // No direction to normalize. A zero-length ray traces nothing, which
        // is the honest answer rather than a division by zero.
        ray.maximumDistance = 0.0f;
        return ray;
    }
    const auto inverse = 1.0f / distance;
    ray.direction = {toLight[0] * inverse, toLight[1] * inverse,
        toLight[2] * inverse};
    // Stopping at the light: anything past it is behind the light and cannot
    // shadow the surface, so reaching further would invent occluders.
    ray.maximumDistance = distance;
    return ray;
}

ShadowResult TraceShadowRay(
    const std::span<const ShadowTriangle> triangles,
    const ShadowRay& ray,
    const std::uint32_t pixelX,
    const std::uint32_t pixelY) noexcept
{
    ShadowResult result{};
    result.distance = ray.maximumDistance;
    auto direction = ray.direction;
    const auto directionLengthSquared = direction[0] * direction[0] +
        direction[1] * direction[1] + direction[2] * direction[2];
    if (!(directionLengthSquared > 0.0f) ||
        !std::isfinite(directionLengthSquared)) {
        return result;
    }
    const auto inverse = 1.0f / std::sqrt(directionLengthSquared);
    for (auto& component : direction) component *= inverse;

    constexpr float kParallelEpsilon = 1.0e-8f;
    for (const auto& triangle : triangles) {
        // Blended geometry casts no ray-traced shadow in this phase, so it
        // must not become an opaque occluder by default.
        if (!ParticipatesInShadows({{}, 0, 0, 0, 0, triangle.opacity})) {
            continue;
        }
        ++result.testedTriangles;
        std::array<float, 3> edge1{};
        std::array<float, 3> edge2{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            edge1[axis] = triangle.b[axis] - triangle.a[axis];
            edge2[axis] = triangle.c[axis] - triangle.a[axis];
        }
        const std::array<float, 3> pvec{
            direction[1] * edge2[2] - direction[2] * edge2[1],
            direction[2] * edge2[0] - direction[0] * edge2[2],
            direction[0] * edge2[1] - direction[1] * edge2[0]};
        const auto determinant = edge1[0] * pvec[0] + edge1[1] * pvec[1] +
            edge1[2] * pvec[2];
        // A ray parallel to the triangle's plane never enters it. Both signs
        // are accepted, because an occluder shadows from either face.
        if (std::abs(determinant) < kParallelEpsilon) continue;
        const auto inverseDeterminant = 1.0f / determinant;
        std::array<float, 3> tvec{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            tvec[axis] = ray.origin[axis] - triangle.a[axis];
        }
        const auto u = (tvec[0] * pvec[0] + tvec[1] * pvec[1] +
            tvec[2] * pvec[2]) * inverseDeterminant;
        if (u < 0.0f || u > 1.0f) continue;
        const std::array<float, 3> qvec{
            tvec[1] * edge1[2] - tvec[2] * edge1[1],
            tvec[2] * edge1[0] - tvec[0] * edge1[2],
            tvec[0] * edge1[1] - tvec[1] * edge1[0]};
        const auto v = (direction[0] * qvec[0] + direction[1] * qvec[1] +
            direction[2] * qvec[2]) * inverseDeterminant;
        if (v < 0.0f || u + v > 1.0f) continue;
        const auto distance = (edge2[0] * qvec[0] + edge2[1] * qvec[1] +
            edge2[2] * qvec[2]) * inverseDeterminant;
        if (distance < ray.minimumDistance ||
            distance > ray.maximumDistance) {
            continue;
        }

        if (triangle.opacity == GeometryOpacity::AlphaTested) {
            ++result.alphaCandidates;
            // The texture at the candidate when the surface names one, which
            // is what gives a cutout its own silhouette rather than a
            // three-corner approximation of it. Sampled at level zero: a
            // ray query has no derivatives, so the device can only do the
            // same, and a shadow that picked a different mip from the
            // surface would not match its own geometry.
            auto sampled = (1.0f - u - v) * triangle.alphaAtVertex[0] +
                u * triangle.alphaAtVertex[1] + v * triangle.alphaAtVertex[2];
            if (triangle.baseColor != nullptr) {
                const std::array<float, 3> weights{1.0f - u - v, u, v};
                auto candidateU = 0.0f;
                auto candidateV = 0.0f;
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    candidateU +=
                        weights[corner] * triangle.texCoord[corner][0];
                    candidateV +=
                        weights[corner] * triangle.texCoord[corner][1];
                }
                texture::SampledColor texel{};
                if (texture::SampleTexture2D(*triangle.baseColor, candidateU,
                        candidateV, 0.0f, texel) ==
                    texture::TexturePacketError::None) {
                    sampled = texel.a;
                }
            }
            // Confirmed through the coverage rule, not a second alpha test.
            if (!ConfirmAlphaCandidate(
                    triangle.alpha, sampled, pixelX, pixelY)) {
                continue;
            }
        }

        // Any confirmed hit blocks the light; a shadow ray has no reason to
        // keep looking for a nearer one.
        result.occluded = true;
        result.distance = distance;
        return result;
    }
    return result;
}

bool ShadowTermAvailable() noexcept
{
    return true;
}

bool ShadowMaskRequired() noexcept
{
    // The mask existed only while the term did not. Leaving it on now would
    // hide a broken shadow from every comparison that follows.
    return !ShadowTermAvailable();
}

AccelHandle AccelSchedule::BeginBuild(const std::uint32_t generation) noexcept
{
    try {
        entries_.push_back(Entry{generation, 0, false, false});
    } catch (const std::bad_alloc&) {
        return {};
    }
    return AccelHandle{generation};
}

void AccelSchedule::Retire(const AccelHandle handle) noexcept
{
    for (auto& entry : entries_) {
        if (entry.generation == handle.generation) entry.retired = true;
    }
}

void AccelSchedule::SignalCompleted(
    const AccelHandle handle,
    const std::uint64_t timelineValue) noexcept
{
    for (auto& entry : entries_) {
        if (entry.generation != handle.generation) continue;
        entry.completedValue = timelineValue;
        entry.completed = true;
    }
}

bool AccelSchedule::Traceable(const AccelHandle handle) const noexcept
{
    for (const auto& entry : entries_) {
        if (entry.generation == handle.generation) return !entry.retired;
    }
    return false;
}

bool AccelSchedule::CanDestroy(
    const AccelHandle handle,
    const std::uint64_t completedTimelineValue) const noexcept
{
    for (const auto& entry : entries_) {
        if (entry.generation != handle.generation) continue;
        // A structure still in flight owns scratch the GPU is reading.
        return entry.completed &&
            completedTimelineValue >= entry.completedValue;
    }
    return false;
}

AccelPacketError ValidateAccelPacket(const AccelPacket& packet) noexcept
{
    if (packet.instances.size() > kMaximumInstances) {
        return AccelPacketError::TooManyInstances;
    }
    for (std::size_t index = 0; index < packet.instances.size(); ++index) {
        const auto& instance = packet.instances[index];
        if (ValidateInstance(instance) != AccelError::None) {
            return instance.instanceId == 0 || instance.blasId == 0
                ? AccelPacketError::InvalidIdentity
                : AccelPacketError::InvalidInstance;
        }
        if (instance.reserved0 != 0) {
            return AccelPacketError::NonZeroPadding;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (packet.instances[other].instanceId == instance.instanceId) {
                return AccelPacketError::DuplicateInstance;
            }
        }
    }
    return AccelPacketError::None;
}

AccelPacketError EncodeAccelPacket(
    const AccelPacket& packet,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    const auto validation = ValidateAccelPacket(packet);
    if (validation != AccelPacketError::None) return validation;
    try {
        AccelPacketHeaderV1 header{};
        header.frameId = packet.header.frameId;
        header.viewId = packet.header.viewId;
        header.instanceCount =
            static_cast<std::uint32_t>(packet.instances.size());
        const auto instancesOffset = AlignUp(
            sizeof(AccelPacketHeaderV1),
            static_cast<std::uint32_t>(alignof(InstanceDescV1)));
        const auto instanceBytes =
            packet.instances.size() * sizeof(InstanceDescV1);
        const auto totalSize = instancesOffset + instanceBytes;
        if (totalSize > std::numeric_limits<std::uint32_t>::max()) {
            return AccelPacketError::AllocationFailure;
        }
        header.instancesOffset = static_cast<std::uint32_t>(instancesOffset);
        header.totalSize = static_cast<std::uint32_t>(totalSize);
        bytes.resize(totalSize);
        if (instanceBytes != 0) {
            std::memcpy(bytes.data() + instancesOffset,
                packet.instances.data(), instanceBytes);
        }
        header.payloadCrc32 = trace::Crc32(
            std::span<const std::byte>{bytes}.subspan(sizeof(header)));
        std::memcpy(bytes.data(), &header, sizeof(header));
        return AccelPacketError::None;
    } catch (const std::bad_alloc&) {
        bytes.clear();
        return AccelPacketError::AllocationFailure;
    }
}

AccelPacketError DecodeAccelPacket(
    const std::span<const std::byte> bytes,
    AccelPacket& packet) noexcept
{
    packet = {};
    if (bytes.size() < sizeof(AccelPacketHeaderV1)) {
        return AccelPacketError::TruncatedHeader;
    }
    AccelPacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kAccelPacketMagic) {
        return AccelPacketError::BadMagic;
    }
    if (header.endianMarker != kAccelPacketEndian) {
        return AccelPacketError::WrongEndian;
    }
    if (header.versionMajor != kAccelPacketVersionMajor ||
        header.versionMinor > kAccelPacketVersionMinor) {
        return AccelPacketError::UnsupportedVersion;
    }
    if (header.headerSize != sizeof(AccelPacketHeaderV1) ||
        header.totalSize != bytes.size()) {
        return AccelPacketError::SizeMismatch;
    }
    if (header.instanceCount > kMaximumInstances) {
        return AccelPacketError::TooManyInstances;
    }
    if (header.instancesOffset % alignof(InstanceDescV1) != 0) {
        return AccelPacketError::MisalignedSection;
    }
    const auto instanceBytes = static_cast<std::size_t>(
        header.instanceCount) * sizeof(InstanceDescV1);
    if (header.instancesOffset < sizeof(AccelPacketHeaderV1) ||
        header.instancesOffset + instanceBytes != bytes.size()) {
        return AccelPacketError::SectionOutOfBounds;
    }
    if (trace::Crc32(bytes.subspan(sizeof(header))) != header.payloadCrc32) {
        return AccelPacketError::ChecksumMismatch;
    }
    try {
        packet.header = header;
        packet.instances.resize(header.instanceCount);
        if (instanceBytes != 0) {
            std::memcpy(packet.instances.data(),
                bytes.data() + header.instancesOffset, instanceBytes);
        }
    } catch (const std::bad_alloc&) {
        packet = {};
        return AccelPacketError::AllocationFailure;
    }
    const auto validation = ValidateAccelPacket(packet);
    if (validation != AccelPacketError::None) {
        packet = {};
        return validation;
    }
    return AccelPacketError::None;
}

const char* ToString(const AccelError error) noexcept
{
    switch (error) {
    case AccelError::None: return "None";
    case AccelError::InvalidIdentity: return "InvalidIdentity";
    case AccelError::InvalidAlignment: return "InvalidAlignment";
    case AccelError::EmptyGeometry: return "EmptyGeometry";
    case AccelError::InvalidBounds: return "InvalidBounds";
    case AccelError::SingularTransform: return "SingularTransform";
    case AccelError::NonFiniteSource: return "NonFiniteSource";
    case AccelError::InvalidMask: return "InvalidMask";
    case AccelError::CustomIndexOutOfRange:
        return "CustomIndexOutOfRange";
    case AccelError::TooManyInstances: return "TooManyInstances";
    }
    return "Unknown";
}

const char* ToString(const AccelPacketError error) noexcept
{
    switch (error) {
    case AccelPacketError::None: return "None";
    case AccelPacketError::TruncatedHeader: return "TruncatedHeader";
    case AccelPacketError::BadMagic: return "BadMagic";
    case AccelPacketError::UnsupportedVersion: return "UnsupportedVersion";
    case AccelPacketError::WrongEndian: return "WrongEndian";
    case AccelPacketError::SizeMismatch: return "SizeMismatch";
    case AccelPacketError::ChecksumMismatch: return "ChecksumMismatch";
    case AccelPacketError::SectionOutOfBounds: return "SectionOutOfBounds";
    case AccelPacketError::MisalignedSection: return "MisalignedSection";
    case AccelPacketError::NonZeroPadding: return "NonZeroPadding";
    case AccelPacketError::TooManyInstances: return "TooManyInstances";
    case AccelPacketError::InvalidIdentity: return "InvalidIdentity";
    case AccelPacketError::DuplicateInstance: return "DuplicateInstance";
    case AccelPacketError::InvalidInstance: return "InvalidInstance";
    case AccelPacketError::AllocationFailure: return "AllocationFailure";
    }
    return "Unknown";
}

}
