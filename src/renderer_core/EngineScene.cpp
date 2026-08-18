#include "renderer_core/EngineScene.h"

#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace vf::renderer::scene {

namespace {

constexpr float kAffineTolerance = 1.0e-5f;
constexpr float kNormalTolerance = 1.0e-3f;

std::size_t AlignUp(
    const std::size_t value,
    const std::size_t alignment) noexcept
{
    if (alignment == 0 || value >
        std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

bool CheckedMultiply(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept
{
    if (left != 0 && right >
        std::numeric_limits<std::size_t>::max() / left) {
        result = std::numeric_limits<std::size_t>::max();
        return false;
    }
    result = left * right;
    return true;
}

bool CheckedAdd(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        result = std::numeric_limits<std::size_t>::max();
        return false;
    }
    result = left + right;
    return true;
}

bool FiniteArray(const float* values, const std::size_t count) noexcept
{
    return std::all_of(values, values + count,
        [](const float value) { return std::isfinite(value); });
}

bool ValidAffine(const float (&matrix)[16]) noexcept
{
    if (!FiniteArray(matrix, 16) ||
        std::abs(matrix[12]) > kAffineTolerance ||
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
    return std::isfinite(determinant) && determinant > 1.0e-10;
}

bool ValidNormal(const float (&normal)[4]) noexcept
{
    if (!FiniteArray(normal, 4) ||
        std::abs(normal[3]) > kAffineTolerance) {
        return false;
    }
    const auto length = std::sqrt(
        normal[0] * normal[0] + normal[1] * normal[1] +
        normal[2] * normal[2]);
    return std::isfinite(length) &&
        std::abs(length - 1.0f) <= kNormalTolerance;
}

bool ValidBounds(const OpaqueObjectV1& object) noexcept
{
    if (!FiniteArray(object.boundsMinimum, 4) ||
        !FiniteArray(object.boundsMaximum, 4) ||
        std::abs(object.boundsMinimum[3]) > kAffineTolerance ||
        std::abs(object.boundsMaximum[3]) > kAffineTolerance) {
        return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (object.boundsMinimum[axis] > object.boundsMaximum[axis]) {
            return false;
        }
    }
    return true;
}

std::array<double, 4> Transform(
    const float (&matrix)[16],
    const std::array<double, 4>& input) noexcept
{
    std::array<double, 4> output{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            output[row] += static_cast<double>(
                matrix[row * 4 + column]) * input[column];
        }
    }
    return output;
}

std::array<double, 4> Transform(
    const view::Matrix4& matrix,
    const std::array<double, 4>& input) noexcept
{
    std::array<double, 4> output{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            output[row] += static_cast<double>(
                matrix.elements[row * 4 + column]) * input[column];
        }
    }
    return output;
}

void WriteId(const std::uint64_t value, std::uint32_t (&words)[2]) noexcept
{
    words[0] = static_cast<std::uint32_t>(value);
    words[1] = static_cast<std::uint32_t>(value >> 32);
}

}

const GBufferPixelV1& GBufferImage::At(
    const std::uint32_t x,
    const std::uint32_t y) const
{
    if (x >= width || y >= height) {
        throw std::out_of_range("G-buffer pixel");
    }
    return pixels[static_cast<std::size_t>(y) * width + x];
}

bool GBufferComparison::Within(
    const float maximumError,
    const double maximumMeanError,
    const std::uint64_t maximumDifferentPixels,
    const std::uint64_t maximumIdentityMismatches) const noexcept
{
    return maximumAbsoluteError <= maximumError &&
        meanAbsoluteError <= maximumMeanError &&
        differingPixels <= maximumDifferentPixels &&
        identityMismatches <= maximumIdentityMismatches;
}

ScenePacketError ValidateScenePacket(const ScenePacket& packet) noexcept
{
    const auto& header = packet.header;
    if (header.magic != kScenePacketMagic ||
        header.versionMajor != kScenePacketVersionMajor ||
        header.versionMinor > kScenePacketVersionMinor ||
        header.endianMarker != kScenePacketEndian ||
        header.headerSize != sizeof(ScenePacketHeaderV1) ||
        header.frameId == 0 || header.viewId == 0 ||
        header.captureSequence == 0 ||
        header.captureThreadId == 0 || header.renderThreadId == 0 ||
        packet.objects.empty() ||
        packet.objects.size() > kMaximumOpaqueObjects ||
        (header.objectCount != 0 &&
            header.objectCount != packet.objects.size())) {
        return ScenePacketError::InvalidIdentity;
    }
    if (header.captureThreadId != header.renderThreadId) {
        return ScenePacketError::WrongThread;
    }
    if (header.flags != 0 || header.reserved0 != 0) {
        return ScenePacketError::InvalidFlags;
    }
    for (std::size_t index = 0; index < packet.objects.size(); ++index) {
        const auto& object = packet.objects[index];
        if (object.objectId == 0 || object.materialId == 0 ||
            object.passSequence == 0) {
            return ScenePacketError::InvalidIdentity;
        }
        if ((object.flags & ~kKnownObjectFlags) != 0 ||
            (object.flags & kKnownObjectFlags) != kKnownObjectFlags) {
            return ScenePacketError::InvalidFlags;
        }
        if (!std::isfinite(object.roughness) || object.roughness < 0.0f ||
            object.roughness > 1.0f) {
            return ScenePacketError::InvalidRoughness;
        }
        if (!ValidAffine(object.model) ||
            !ValidAffine(object.previousModel)) {
            return ScenePacketError::InvalidTransform;
        }
        if (!ValidBounds(object)) return ScenePacketError::InvalidBounds;
        if (!ValidNormal(object.geometricNormal) ||
            !ValidNormal(object.shadingNormal)) {
            return ScenePacketError::InvalidNormal;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (packet.objects[other].objectId == object.objectId) {
                return ScenePacketError::DuplicateObject;
            }
            if (packet.objects[other].drawIndex == object.drawIndex) {
                return ScenePacketError::DuplicateDraw;
            }
        }
    }
    const auto instanceResult = ValidateSceneInstances(packet);
    if (instanceResult != ScenePacketError::None) return instanceResult;
    return ValidateSceneVisibility(packet);
}

ScenePacketError ValidateSceneInstances(const ScenePacket& packet) noexcept
{
    if (packet.instances.empty()) return ScenePacketError::None;
    if (packet.instances.size() > kMaximumSceneInstances ||
        packet.instances.size() < packet.objects.size() ||
        (packet.header.instanceCount != 0 &&
            packet.header.instanceCount != packet.instances.size())) {
        return ScenePacketError::InvalidInstance;
    }
    // Instances form one contiguous run per object so a single draw can
    // cover them, and every object must own at least one run entry.
    std::uint32_t expectedObject = 0;
    for (std::size_t index = 0; index < packet.instances.size(); ++index) {
        const auto& instance = packet.instances[index];
        if (instance.objectId == 0 ||
            instance.objectIndex >= packet.objects.size()) {
            return ScenePacketError::InvalidInstance;
        }
        if (instance.objectIndex != expectedObject) {
            if (instance.objectIndex != expectedObject + 1) {
                return ScenePacketError::InvalidInstance;
            }
            expectedObject = instance.objectIndex;
        }
        if ((instance.flags & ~kKnownInstanceFlags) != 0 ||
            (instance.flags & kKnownInstanceFlags) != kKnownInstanceFlags) {
            return ScenePacketError::InvalidFlags;
        }
        if (!ValidAffine(instance.model) ||
            !ValidAffine(instance.previousModel)) {
            return ScenePacketError::InvalidTransform;
        }
        if (!FiniteArray(instance.parameters, 4) ||
            std::any_of(std::begin(instance.parameters),
                std::end(instance.parameters),
                [](const float value) {
                    return value < 0.0f || value > 1.0f;
                })) {
            return ScenePacketError::InvalidParameters;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (packet.instances[other].objectId == instance.objectId) {
                return ScenePacketError::DuplicateInstance;
            }
        }
    }
    if (expectedObject + 1 != packet.objects.size()) {
        return ScenePacketError::UncoveredObject;
    }
    return ScenePacketError::None;
}

ScenePacketError ValidateSceneVisibility(const ScenePacket& packet) noexcept
{
    if (packet.visibility.empty()) return ScenePacketError::None;
    // A partial table cannot be resolved per object, so it is refused rather
    // than silently defaulted for the objects it does not cover.
    if (packet.visibility.size() != packet.objects.size() ||
        (packet.header.visibilityCount != 0 &&
            packet.header.visibilityCount != packet.visibility.size())) {
        return ScenePacketError::UncoveredObject;
    }
    for (std::size_t index = 0; index < packet.visibility.size(); ++index) {
        const auto& record = packet.visibility[index];
        const auto validation = visibility::ValidateVisibilityRecord(record);
        if (validation == visibility::VisibilityError::UnclassifiedAlpha) {
            // An unclassified world writer is the one condition that must
            // never reach the mirror.
            return ScenePacketError::UnclassifiedWorldWriter;
        }
        if (validation != visibility::VisibilityError::None) {
            return ScenePacketError::InvalidVisibility;
        }
        // Records are positional so a consumer can index them by object.
        if (record.objectId != packet.objects[index].objectId ||
            record.materialId != packet.objects[index].materialId) {
            return ScenePacketError::InvalidVisibility;
        }
    }
    for (const auto& transparent : packet.transparent) {
        // A stencil value is eight bits wide. The field is thirty-two only
        // because that is what the record aligns to, and a consumer that
        // narrows it -- which every consumer does, because that is the width
        // stencil actually has -- would otherwise drop the high bits of a
        // value the packet claimed was meaningful.
        if (transparent.stencilReceiverMask > 0xFFu ||
            transparent.stencilReference > 0xFFu) {
            return ScenePacketError::InvalidTransparentDraw;
        }
    }
    return ScenePacketError::None;
}

visibility::VisibilityRecordV1 ResolveVisibility(
    const ScenePacket& packet,
    const std::size_t objectIndex) noexcept
{
    visibility::VisibilityRecordV1 record{};
    if (objectIndex >= packet.objects.size()) return record;
    if (!packet.visibility.empty()) return packet.visibility[objectIndex];
    record.objectId = packet.objects[objectIndex].objectId;
    record.materialId = packet.objects[objectIndex].materialId;
    record.alpha.classification = visibility::AlphaClass::Opaque;
    record.alpha.source = visibility::AlphaSource::None;
    record.alpha.constantAlpha = 1.0f;
    record.alpha.fade = 1.0f;
    record.faceMode = visibility::FaceMode::FrontOnly;
    record.modelDeterminant = 1.0f;
    return record;
}

InstanceRange ObjectInstanceRange(
    const ScenePacket& packet,
    const std::size_t objectIndex) noexcept
{
    InstanceRange range{};
    if (objectIndex >= packet.objects.size()) return range;
    if (packet.instances.empty()) {
        range.first = static_cast<std::uint32_t>(objectIndex);
        range.count = 1;
        return range;
    }
    bool started = false;
    for (std::size_t index = 0; index < packet.instances.size(); ++index) {
        if (packet.instances[index].objectIndex != objectIndex) {
            if (started) break;
            continue;
        }
        if (!started) {
            range.first = static_cast<std::uint32_t>(index);
            started = true;
        }
        ++range.count;
    }
    return range;
}

InstanceV1 NarrowInstance(
    const InstanceV1& instance,
    const std::span<const double, 3> cameraOrigin) noexcept
{
    auto narrowed = instance;
    for (const auto axis : cameraOrigin) {
        if (!std::isfinite(axis)) return narrowed;
    }
    // Row-major with column vectors, so the translation is the fourth column
    // of each of the first three rows. The fourth row is the affine one and
    // has nothing to move.
    constexpr std::size_t kTranslation[3]{3, 7, 11};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto slot = kTranslation[axis];
        narrowed.model[slot] = static_cast<float>(
            static_cast<double>(instance.model[slot]) - cameraOrigin[axis]);
        // The previous transform moves by the same origin. Narrowing one and
        // not the other would make every static object appear to travel the
        // camera's distance from the world origin between frames, which is
        // motion the whole frame would be smeared by.
        narrowed.previousModel[slot] = static_cast<float>(
            static_cast<double>(instance.previousModel[slot]) -
            cameraOrigin[axis]);
    }
    return narrowed;
}

InstanceV1 ResolveInstance(
    const ScenePacket& packet,
    const std::size_t objectIndex,
    const std::uint32_t instanceIndex) noexcept
{
    if (!packet.instances.empty() &&
        instanceIndex < packet.instances.size()) {
        return packet.instances[instanceIndex];
    }
    InstanceV1 implicit{};
    if (objectIndex >= packet.objects.size()) return implicit;
    const auto& object = packet.objects[objectIndex];
    implicit.objectId = object.objectId;
    implicit.objectIndex = static_cast<std::uint32_t>(objectIndex);
    implicit.flags = InstanceStatic;
    std::copy(std::begin(object.model), std::end(object.model),
        std::begin(implicit.model));
    std::copy(std::begin(object.previousModel),
        std::end(object.previousModel), std::begin(implicit.previousModel));
    return implicit;
}

ScenePacketError ValidateSceneAgainstRaster(
    const ScenePacket& scene,
    const raster::DecodedPacket& rasterPacket,
    const std::uint64_t expectedFrameId,
    const std::uint64_t expectedViewId) noexcept
{
    const auto validation = ValidateScenePacket(scene);
    if (validation != ScenePacketError::None) return validation;
    if (expectedFrameId == 0 || scene.header.frameId != expectedFrameId ||
        rasterPacket.header.frameIndex != expectedFrameId) {
        return ScenePacketError::FrameMismatch;
    }
    if (expectedViewId == 0 || scene.header.viewId != expectedViewId) {
        return ScenePacketError::ViewMismatch;
    }
    if (scene.objects.size() != rasterPacket.draws.size()) {
        return ScenePacketError::InvalidDraw;
    }
    const raster::MaterialRegistry registry{rasterPacket.materials};
    if (registry.HasDuplicateIds()) {
        return ScenePacketError::MissingMaterial;
    }
    for (const auto& object : scene.objects) {
        if (object.drawIndex >= rasterPacket.draws.size()) {
            return ScenePacketError::InvalidDraw;
        }
        const auto& draw = rasterPacket.draws[object.drawIndex];
        if (draw.materialId != object.materialId ||
            registry.Resolve(object.materialId) == nullptr) {
            return ScenePacketError::MissingMaterial;
        }
        const auto drawEnd = static_cast<std::uint64_t>(draw.firstIndex) +
            draw.indexCount;
        if (draw.indexCount == 0 || draw.indexCount % 3 != 0 ||
            drawEnd > rasterPacket.indices.size()) {
            return ScenePacketError::InvalidDraw;
        }
    }
    return ScenePacketError::None;
}

ScenePacketError ValidateSceneAgainstFrame(
    const ScenePacket& scene,
    const view::FramePacket& frame,
    SceneCoverage& coverage) noexcept
{
    coverage = {};
    const auto validation = ValidateScenePacket(scene);
    if (validation != ScenePacketError::None) return validation;
    if (frame.header.frameId != scene.header.frameId) {
        return ScenePacketError::FrameMismatch;
    }
    if (std::none_of(frame.views.begin(), frame.views.end(),
            [&scene](const view::ViewRecordV1& record) {
                return record.viewId == scene.header.viewId;
            })) {
        return ScenePacketError::ViewMismatch;
    }
    try {
        // Only this view's passes are accounted here. A pass sequence wider
        // than the record field can never match, so it stays unmirrored
        // instead of aliasing a different pass.
        std::vector<const view::PassRecordV1*> viewPasses;
        for (const auto& pass : frame.passes) {
            if (pass.viewId != scene.header.viewId) continue;
            viewPasses.push_back(&pass);
            if ((pass.flags & view::PassWritesWorldTarget) == 0) continue;
            ++coverage.worldWritingPasses;
            if (pass.category == view::PassCategory::Unknown) {
                ++coverage.unknownWorldWriters;
            } else if (pass.category == view::PassCategory::Opaque) {
                ++coverage.opaquePasses;
            } else {
                ++coverage.deferredClasses;
            }
        }
        std::vector<std::uint64_t> mirrored;
        for (const auto& object : scene.objects) {
            const auto match = std::find_if(
                viewPasses.begin(), viewPasses.end(),
                [&object](const view::PassRecordV1* pass) {
                    return pass->sequence == object.passSequence;
                });
            if (match == viewPasses.end()) {
                return ScenePacketError::UnknownPass;
            }
            const auto& pass = **match;
            if ((pass.flags & view::PassWritesWorldTarget) == 0 ||
                pass.category != view::PassCategory::Opaque) {
                return ScenePacketError::PassClassMismatch;
            }
            if (std::find(mirrored.begin(), mirrored.end(), pass.sequence) ==
                mirrored.end()) {
                mirrored.push_back(pass.sequence);
            }
        }
        coverage.mirroredPasses = static_cast<std::uint32_t>(mirrored.size());
        coverage.unmirroredOpaquePasses =
            coverage.opaquePasses - coverage.mirroredPasses;
        if (coverage.unknownWorldWriters != 0) {
            return ScenePacketError::UnclassifiedWorldWriter;
        }
        if (coverage.unmirroredOpaquePasses != 0) {
            return ScenePacketError::UncoveredPass;
        }
        return ScenePacketError::None;
    } catch (...) {
        coverage = {};
        return ScenePacketError::AllocationFailure;
    }
}

ScenePacketError EncodeScenePacket(
    const ScenePacket& packet,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    const auto validation = ValidateScenePacket(packet);
    if (validation != ScenePacketError::None) return validation;
    try {
        ScenePacketHeaderV1 header{};
        // A scene encodes at the lowest version that can represent it, so
        // existing captures round-trip byte-identically.
        header.versionMinor = std::uint16_t{0};
        if (!packet.instances.empty()) {
            header.versionMinor = kScenePacketInstanceVersionMinor;
        }
        if (!packet.visibility.empty()) {
            header.versionMinor = kScenePacketVisibilityVersionMinor;
        }
        if (!packet.transparent.empty()) {
            header.versionMinor = kScenePacketDecalVersionMinor;
        }
        header.frameId = packet.header.frameId;
        header.viewId = packet.header.viewId;
        header.captureSequence = packet.header.captureSequence;
        header.captureThreadId = packet.header.captureThreadId;
        header.renderThreadId = packet.header.renderThreadId;
        header.flags = packet.header.flags;
        header.objectCount = static_cast<std::uint32_t>(
            packet.objects.size());
        header.instanceCount = static_cast<std::uint32_t>(
            packet.instances.size());
        header.visibilityCount = static_cast<std::uint32_t>(
            packet.visibility.size());
        header.transparentCount = static_cast<std::uint32_t>(
            packet.transparent.size());
        // A draw naming an object the scene does not have would composite
        // over nothing. Refused here rather than at draw time, where the
        // only symptom is an effect that never appears.
        for (const auto& blended : packet.transparent) {
            if (blended.objectIndex >= packet.objects.size()) {
                return ScenePacketError::InvalidDraw;
            }
        }
        const auto objectsOffset = AlignUp(
            sizeof(header), alignof(OpaqueObjectV1));
        std::size_t objectBytes{};
        std::size_t instancesOffset{};
        std::size_t instanceBytes{};
        std::size_t visibilityOffset{};
        std::size_t visibilityBytes{};
        std::size_t totalSize{};
        if (objectsOffset == std::numeric_limits<std::size_t>::max() ||
            !CheckedMultiply(packet.objects.size(), sizeof(OpaqueObjectV1),
                objectBytes) ||
            !CheckedAdd(objectsOffset, objectBytes, instancesOffset)) {
            return ScenePacketError::AllocationFailure;
        }
        const auto objectsEnd = instancesOffset;
        instancesOffset = packet.instances.empty()
            ? 0 : AlignUp(instancesOffset, alignof(InstanceV1));
        if (!CheckedMultiply(packet.instances.size(), sizeof(InstanceV1),
                instanceBytes) ||
            !CheckedAdd(packet.instances.empty()
                    ? objectsEnd : instancesOffset,
                instanceBytes, totalSize) ||
            totalSize > std::numeric_limits<std::uint32_t>::max()) {
            return ScenePacketError::AllocationFailure;
        }
        const auto instancesEnd = totalSize;
        visibilityOffset = packet.visibility.empty()
            ? 0
            : AlignUp(instancesEnd,
                alignof(visibility::VisibilityRecordV1));
        if (!CheckedMultiply(packet.visibility.size(),
                sizeof(visibility::VisibilityRecordV1), visibilityBytes) ||
            !CheckedAdd(packet.visibility.empty()
                    ? instancesEnd : visibilityOffset,
                visibilityBytes, totalSize) ||
            totalSize > std::numeric_limits<std::uint32_t>::max()) {
            return ScenePacketError::AllocationFailure;
        }
        const auto visibilityEnd = totalSize;
        std::size_t transparentOffset{};
        std::size_t transparentBytes{};
        transparentOffset = packet.transparent.empty()
            ? 0
            : AlignUp(visibilityEnd, alignof(TransparentDrawRecordV1));
        if (!CheckedMultiply(packet.transparent.size(),
                sizeof(TransparentDrawRecordV1), transparentBytes) ||
            !CheckedAdd(packet.transparent.empty()
                    ? visibilityEnd : transparentOffset,
                transparentBytes, totalSize) ||
            totalSize > std::numeric_limits<std::uint32_t>::max()) {
            return ScenePacketError::AllocationFailure;
        }
        header.objectsOffset = static_cast<std::uint32_t>(objectsOffset);
        header.instancesOffset = static_cast<std::uint32_t>(instancesOffset);
        header.visibilityOffset = static_cast<std::uint32_t>(
            visibilityOffset);
        header.transparentOffset = static_cast<std::uint32_t>(
            transparentOffset);
        header.totalSize = static_cast<std::uint32_t>(totalSize);
        bytes.assign(totalSize, std::byte{0});
        std::memcpy(bytes.data() + objectsOffset,
            packet.objects.data(), objectBytes);
        if (!packet.instances.empty()) {
            std::memcpy(bytes.data() + instancesOffset,
                packet.instances.data(), instanceBytes);
        }
        if (!packet.visibility.empty()) {
            std::memcpy(bytes.data() + visibilityOffset,
                packet.visibility.data(), visibilityBytes);
        }
        if (!packet.transparent.empty()) {
            std::memcpy(bytes.data() + transparentOffset,
                packet.transparent.data(), transparentBytes);
        }
        header.payloadCrc32 = trace::Crc32(
            std::span<const std::byte>{bytes}.subspan(sizeof(header)));
        std::memcpy(bytes.data(), &header, sizeof(header));
        return ScenePacketError::None;
    } catch (...) {
        bytes.clear();
        return ScenePacketError::AllocationFailure;
    }
}

ScenePacketError DecodeScenePacket(
    const std::span<const std::byte> bytes,
    ScenePacket& packet) noexcept
{
    packet = {};
    if (bytes.size() < sizeof(ScenePacketHeaderV1)) {
        return ScenePacketError::TruncatedHeader;
    }
    ScenePacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kScenePacketMagic) return ScenePacketError::BadMagic;
    if (header.versionMajor != kScenePacketVersionMajor ||
        header.versionMinor > kScenePacketVersionMinor) {
        return ScenePacketError::UnsupportedVersion;
    }
    if (header.endianMarker != kScenePacketEndian) {
        return ScenePacketError::WrongEndian;
    }
    if (header.headerSize != sizeof(header)) {
        return ScenePacketError::SectionOutOfBounds;
    }
    if (header.totalSize != bytes.size()) {
        return ScenePacketError::SizeMismatch;
    }
    if (header.flags != 0 || header.reserved0 != 0 ||
        false) {
        return ScenePacketError::NonZeroPadding;
    }
    if (header.objectCount == 0 ||
        header.objectCount > kMaximumOpaqueObjects ||
        header.instanceCount > kMaximumSceneInstances) {
        return ScenePacketError::SectionOutOfBounds;
    }
    // Version 1.0 never carries an instance section.
    if (header.versionMinor < kScenePacketInstanceVersionMinor &&
        (header.instanceCount != 0 || header.instancesOffset != 0)) {
        return ScenePacketError::UnsupportedVersion;
    }
    // Versions below 1.2 never carry a visibility section.
    if (header.versionMinor < kScenePacketVisibilityVersionMinor &&
        (header.visibilityCount != 0 || header.visibilityOffset != 0)) {
        return ScenePacketError::UnsupportedVersion;
    }
    // Versions below 1.3 never carry a transparent section. A reader that
    // predates it must refuse the packet rather than read those bytes at
    // whatever they used to mean.
    // Below 1.3 there was no transparent section at all, and below 1.4 its
    // records were narrower. Both are refused for the same reason: the bytes
    // mean something else at those versions, and reading them at this stride
    // would not fail -- it would report each draw assembled from its
    // neighbour's fields.
    if (header.versionMinor < kScenePacketDecalVersionMinor &&
        (header.transparentCount != 0 || header.transparentOffset != 0)) {
        return ScenePacketError::UnsupportedVersion;
    }
    if (header.visibilityCount != 0 &&
        header.visibilityCount != header.objectCount) {
        return ScenePacketError::SectionOutOfBounds;
    }
    const auto expectedOffset = AlignUp(
        sizeof(header), alignof(OpaqueObjectV1));
    std::size_t objectBytes{};
    std::size_t objectEnd{};
    std::size_t instanceBytes{};
    std::size_t visibilityBytes{};
    std::size_t transparentBytes{};
    std::size_t instanceEnd{};
    std::size_t visibilityEnd{};
    std::size_t expectedTotal{};
    if (!CheckedMultiply(header.objectCount, sizeof(OpaqueObjectV1),
            objectBytes) ||
        !CheckedAdd(expectedOffset, objectBytes, objectEnd) ||
        !CheckedMultiply(header.instanceCount, sizeof(InstanceV1),
            instanceBytes) ||
        !CheckedMultiply(header.visibilityCount,
            sizeof(visibility::VisibilityRecordV1), visibilityBytes) ||
        !CheckedMultiply(header.transparentCount,
            sizeof(TransparentDrawRecordV1), transparentBytes)) {
        return ScenePacketError::SectionOutOfBounds;
    }
    const auto expectedInstancesOffset = header.instanceCount == 0
        ? std::size_t{0} : AlignUp(objectEnd, alignof(InstanceV1));
    if (!CheckedAdd(header.instanceCount == 0
                ? objectEnd : expectedInstancesOffset,
            instanceBytes, instanceEnd)) {
        return ScenePacketError::SectionOutOfBounds;
    }
    const auto expectedVisibilityOffset = header.visibilityCount == 0
        ? std::size_t{0}
        : AlignUp(instanceEnd, alignof(visibility::VisibilityRecordV1));
    if (!CheckedAdd(header.visibilityCount == 0
                ? instanceEnd : expectedVisibilityOffset,
            visibilityBytes, visibilityEnd)) {
        return ScenePacketError::SectionOutOfBounds;
    }
    const auto expectedTransparentOffset = header.transparentCount == 0
        ? std::size_t{0}
        : AlignUp(visibilityEnd, alignof(TransparentDrawRecordV1));
    if (!CheckedAdd(header.transparentCount == 0
                ? visibilityEnd : expectedTransparentOffset,
            transparentBytes, expectedTotal) ||
        header.objectsOffset != expectedOffset ||
        header.instancesOffset != expectedInstancesOffset ||
        header.visibilityOffset != expectedVisibilityOffset ||
        header.transparentOffset != expectedTransparentOffset ||
        expectedTotal != bytes.size()) {
        return header.objectsOffset % alignof(OpaqueObjectV1) != 0 ||
                header.instancesOffset % alignof(InstanceV1) != 0 ||
                header.visibilityOffset %
                    alignof(visibility::VisibilityRecordV1) != 0
            ? ScenePacketError::MisalignedSection
            : ScenePacketError::SectionOutOfBounds;
    }
    if (!std::all_of(bytes.begin() + sizeof(header),
            bytes.begin() + expectedOffset,
            [](const std::byte value) { return value == std::byte{0}; })) {
        return ScenePacketError::NonZeroPadding;
    }
    if (header.instanceCount != 0 &&
        !std::all_of(bytes.begin() + objectEnd,
            bytes.begin() + expectedInstancesOffset,
            [](const std::byte value) { return value == std::byte{0}; })) {
        return ScenePacketError::NonZeroPadding;
    }
    if (header.visibilityCount != 0 &&
        !std::all_of(bytes.begin() + instanceEnd,
            bytes.begin() + expectedVisibilityOffset,
            [](const std::byte value) { return value == std::byte{0}; })) {
        return ScenePacketError::NonZeroPadding;
    }
    if (trace::Crc32(bytes.subspan(sizeof(header))) !=
        header.payloadCrc32) {
        return ScenePacketError::ChecksumMismatch;
    }
    try {
        ScenePacket candidate{};
        candidate.header = header;
        candidate.objects.resize(header.objectCount);
        std::memcpy(candidate.objects.data(),
            bytes.data() + expectedOffset, objectBytes);
        if (header.instanceCount != 0) {
            candidate.instances.resize(header.instanceCount);
            std::memcpy(candidate.instances.data(),
                bytes.data() + expectedInstancesOffset, instanceBytes);
        }
        if (header.visibilityCount != 0) {
            candidate.visibility.resize(header.visibilityCount);
            std::memcpy(candidate.visibility.data(),
                bytes.data() + expectedVisibilityOffset, visibilityBytes);
        }
        if (header.transparentCount != 0) {
            candidate.transparent.resize(header.transparentCount);
            std::memcpy(candidate.transparent.data(),
                bytes.data() + expectedTransparentOffset, transparentBytes);
        }
        const auto validation = ValidateScenePacket(candidate);
        if (validation != ScenePacketError::None) return validation;
        packet = std::move(candidate);
        return ScenePacketError::None;
    } catch (...) {
        packet = {};
        return ScenePacketError::AllocationFailure;
    }
}

ScenePacketError ProjectScenePacket(
    const raster::DecodedPacket& source,
    const view::ViewRecordV1& capturedView,
    const ScenePacket& scene,
    raster::DecodedPacket& projected) noexcept
{
    return ProjectScenePacket(
        source, capturedView, scene, projected, nullptr, nullptr);
}

ScenePacketError ProjectScenePacket(
    const raster::DecodedPacket& source,
    const view::ViewRecordV1& capturedView,
    const ScenePacket& scene,
    raster::DecodedPacket& projected,
    std::vector<std::array<float, 3>>* const cameraRelativePositions) noexcept
{
    return ProjectScenePacket(source, capturedView, scene, projected,
        cameraRelativePositions, nullptr);
}

ScenePacketError ProjectScenePacket(
    const raster::DecodedPacket& source,
    const view::ViewRecordV1& capturedView,
    const ScenePacket& scene,
    raster::DecodedPacket& projected,
    std::vector<std::array<float, 3>>* const cameraRelativePositions,
    std::vector<float>* const inverseW) noexcept
{
    if (cameraRelativePositions != nullptr) cameraRelativePositions->clear();
    if (inverseW != nullptr) inverseW->clear();
    projected = {};
    if (view::ValidateView(capturedView) != view::ViewError::None) {
        return ScenePacketError::ViewMismatch;
    }
    const auto validation = ValidateSceneAgainstRaster(scene, source,
        source.header.frameIndex, capturedView.viewId);
    if (validation != ScenePacketError::None) return validation;
    try {
        projected.header = source.header;
        projected.materials = source.materials;
        std::size_t totalIndices{};
        for (std::size_t objectIndex = 0;
             objectIndex < scene.objects.size(); ++objectIndex) {
            const auto& draw =
                source.draws[scene.objects[objectIndex].drawIndex];
            const auto range = ObjectInstanceRange(scene, objectIndex);
            std::size_t objectIndices{};
            if (!CheckedMultiply(draw.indexCount, range.count,
                    objectIndices) ||
                !CheckedAdd(totalIndices, objectIndices, totalIndices)) {
                projected = {};
                return ScenePacketError::AllocationFailure;
            }
        }
        projected.vertices.reserve(totalIndices);
        projected.indices.reserve(totalIndices);
        projected.draws.reserve(scene.objects.size());
        // Each object expands into one draw per instance, in packet order.
        for (std::size_t objectIndex = 0;
             objectIndex < scene.objects.size(); ++objectIndex) {
            const auto& object = scene.objects[objectIndex];
            const auto& sourceDraw = source.draws[object.drawIndex];
            const auto range = ObjectInstanceRange(scene, objectIndex);
            for (std::uint32_t local = 0; local < range.count; ++local) {
                const auto instance = ResolveInstance(
                    scene, objectIndex, range.first + local);
                auto draw = sourceDraw;
                draw.firstIndex = static_cast<std::uint32_t>(
                    projected.indices.size());
                draw.vertexOffset = 0;
                for (std::uint32_t element = 0;
                     element < sourceDraw.indexCount; ++element) {
                    const auto sourceIndex64 = static_cast<std::int64_t>(
                        source.indices[sourceDraw.firstIndex + element]) +
                        sourceDraw.vertexOffset;
                    if (sourceIndex64 < 0 ||
                        static_cast<std::uint64_t>(sourceIndex64) >=
                            source.vertices.size() ||
                        projected.vertices.size() >=
                            std::numeric_limits<std::uint32_t>::max()) {
                        projected = {};
                        return ScenePacketError::InvalidDraw;
                    }
                    auto vertex = source.vertices[
                        static_cast<std::size_t>(sourceIndex64)];
                    const std::array<double, 4> localPosition{
                        vertex.position[0], vertex.position[1],
                        vertex.position[2], 1.0};
                    const auto world = Transform(
                        instance.model, localPosition);
                    const auto clip = Transform(
                        capturedView.viewProjection, world);
                    if (!std::all_of(clip.begin(), clip.end(),
                            [](const double value) {
                                return std::isfinite(value);
                            }) || clip[3] <= 1.0e-12) {
                        projected = {};
                        return ScenePacketError::InvalidTransform;
                    }
                    for (std::size_t component = 0;
                         component < 3; ++component) {
                        const auto value = clip[component] / clip[3];
                        if (!std::isfinite(value) ||
                            std::abs(value) >
                                std::numeric_limits<float>::max()) {
                            projected = {};
                            return ScenePacketError::InvalidTransform;
                        }
                        vertex.position[component] =
                            static_cast<float>(value);
                    }
                    projected.indices.push_back(static_cast<std::uint32_t>(
                        projected.vertices.size()));
                    projected.vertices.push_back(vertex);
                    if (cameraRelativePositions != nullptr) {
                        cameraRelativePositions->push_back({
                            static_cast<float>(world[0]),
                            static_cast<float>(world[1]),
                            static_cast<float>(world[2])});
                    }
                    if (inverseW != nullptr) {
                        inverseW->push_back(
                            static_cast<float>(1.0 / clip[3]));
                    }
                }
                projected.draws.push_back(draw);
            }
        }
        projected.header.indexType = projected.vertices.size() <=
            std::numeric_limits<std::uint16_t>::max()
            ? raster::IndexType::Uint16
            : raster::IndexType::Uint32;
        projected.header.vertexCount = static_cast<std::uint32_t>(
            projected.vertices.size());
        projected.header.indexCount = static_cast<std::uint32_t>(
            projected.indices.size());
        projected.header.drawCount = static_cast<std::uint32_t>(
            projected.draws.size());
        projected.header.materialCount = static_cast<std::uint32_t>(
            projected.materials.size());
        return ScenePacketError::None;
    } catch (...) {
        projected = {};
        return ScenePacketError::AllocationFailure;
    }
}

ScenePacketError RenderReferenceGBuffer(
    const raster::DecodedPacket& projected,
    const ScenePacket& scene,
    GBufferImage& image) noexcept
{
    return RenderReferenceGBuffer(projected, scene, nullptr, image);
}

namespace {

// A scene with no family table resolves every object to the ordinary lit
// surface, which is the same implicit record the backend uploads.
const material::FamilyPacket kEmptyFamilies{};

// Mirrors family_shading.glsl's kVfHorizonEpsilon and the lift in
// visibility::ResolveShadingFrame.
constexpr float kReferenceHorizonEpsilon = 1.0e-3f;

[[nodiscard]] bool NormalizeReferenceNormal(
    std::array<float, 3>& value) noexcept
{
    const auto lengthSquared = value[0] * value[0] + value[1] * value[1] +
        value[2] * value[2];
    if (!(lengthSquared > 0.0f) || !std::isfinite(lengthSquared)) {
        return false;
    }
    const auto inverse = 1.0f / std::sqrt(lengthSquared);
    for (auto& component : value) component *= inverse;
    return true;
}

// Duff et al.'s branchless orthonormal basis, mirroring
// vfOrthonormalBasis. The sign trick keeps the denominator away from zero
// for every input normal.
void ReferenceOrthonormalBasis(
    const std::array<float, 3>& n,
    std::array<float, 3>& tangent,
    std::array<float, 3>& bitangent) noexcept
{
    const auto s = n[2] >= 0.0f ? 1.0f : -1.0f;
    const auto a = -1.0f / (s + n[2]);
    const auto c = n[0] * n[1] * a;
    tangent = {1.0f + s * n[0] * n[0] * a, s * c, -s * n[0]};
    bitangent = {c, s + n[1] * n[1] * a, -n[1]};
}


// The interpolated surface normals for one covered pixel. Extracted rather
// than left inline because the height march needs them *before* the base
// colour is sampled -- it decides which texel that sample reads -- while the
// shading that follows needs them after. One copy, called twice at most,
// beats two copies that can drift.
struct ReferenceSurfaceNormals
{
    std::array<float, 3> geometric{};
    std::array<float, 3> shading{};
};

[[nodiscard]] ReferenceSurfaceNormals ComputeReferenceNormals(
    const raster::RasterVertexV3& vertexA,
    const raster::RasterVertexV3& vertexB,
    const raster::RasterVertexV3& vertexC,
    const float weightA,
    const float weightB,
    const float weightC,
    const scene::InstanceV1& instance,
    const OpaqueObjectV1& object,
    const float faceSign) noexcept
{
    ReferenceSurfaceNormals result{};
    // Interpolated across the triangle, exactly as the vertex stage hands it
    // to the fragment stage. Taking the object's own axis instead shades
    // every surface of an object as one flat plane, and the oracle would then
    // disagree with the backend on every curved surface -- a difference that
    // looks like a backend fault rather than like two different normals.
    const std::array<float, 3> localNormal{
        weightA * vertexA.normal[0] + weightB * vertexB.normal[0] +
            weightC * vertexC.normal[0],
        weightA * vertexA.normal[1] + weightB * vertexB.normal[1] +
            weightC * vertexC.normal[1],
        weightA * vertexA.normal[2] + weightB * vertexB.normal[2] +
            weightC * vertexC.normal[2]};
    // Rotated by the instance's upper 3x3, because the vertex normal is in
    // the same local space the vertex position is, and the shader rotates it
    // there too. Interpolating and then forgetting to rotate leaves every
    // rotated object lit as though it had never turned.
    std::array<float, 3> interpolated{};
    for (std::size_t row = 0; row < 3; ++row) {
        interpolated[row] = instance.model[row * 4 + 0] * localNormal[0] +
            instance.model[row * 4 + 1] * localNormal[1] +
            instance.model[row * 4 + 2] * localNormal[2];
    }
    const auto interpolatedLength = std::sqrt(
        interpolated[0] * interpolated[0] +
        interpolated[1] * interpolated[1] +
        interpolated[2] * interpolated[2]);
    // The same threshold the shader applies, so a mesh without usable normals
    // falls back on both sides at the same moment rather than at two
    // different ones.
    const auto useVertexNormal = interpolatedLength > 1.0e-4f;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        result.geometric[axis] = useVertexNormal
            ? interpolated[axis] / interpolatedLength * faceSign
            : object.geometricNormal[axis] * faceSign;
        result.shading[axis] = useVertexNormal
            ? interpolated[axis] / interpolatedLength * faceSign
            : object.shadingNormal[axis] * faceSign;
    }
    static_cast<void>(NormalizeReferenceNormal(result.geometric));
    return result;
}

// Mirrors vfParallaxOffset in shaders/phase16/family_shading.glsl, step for
// step. The march is a loop with an early exit and an interpolation at the
// end, and every one of those is a place the two sides can part company, so
// this is written to follow the shader rather than to be tidy.
[[nodiscard]] std::array<float, 2> ReferenceParallaxOffset(
    const float* const parallax,
    const float minimumStepsIn,
    const float maximumStepsIn,
    const texture::CapturedTexture& heightMap,
    const std::array<float, 2>& texCoord,
    const std::array<float, 3>& geometric,
    const std::array<float, 3>& toViewer) noexcept
{
    std::array<float, 3> tangent{};
    std::array<float, 3> bitangent{};
    ReferenceOrthonormalBasis(geometric, tangent, bitangent);
    const auto project = [&toViewer](const std::array<float, 3>& axis) {
        return toViewer[0] * axis[0] + toViewer[1] * axis[1] +
            toViewer[2] * axis[2];
    };
    const std::array<float, 3> view{
        project(tangent), project(bitangent), project(geometric)};
    if (!(view[2] > 1.0e-4f)) return texCoord;

    const auto minimumSteps = std::max(1.0f, minimumStepsIn);
    const auto maximumSteps = std::max(minimumSteps, maximumStepsIn);
    const auto blend = std::clamp(view[2], 0.0f, 1.0f);
    const auto steps = maximumSteps + (minimumSteps - maximumSteps) * blend;
    const auto layerDepth = 1.0f / steps;
    std::array<float, 2> current{
        texCoord[0] * parallax[2], texCoord[1] * parallax[3]};
    const std::array<float, 2> stepDelta{
        (view[0] / view[2]) * parallax[0] * layerDepth,
        (view[1] / view[2]) * parallax[0] * layerDepth};

    const auto height = [&heightMap, parallax](
        const std::array<float, 2>& at) {
        texture::SampledColor sampled{};
        if (texture::SampleTexture2D(heightMap, at[0], at[1], 0.0f,
                sampled) != texture::TexturePacketError::None) {
            return parallax[1];
        }
        return sampled.r + parallax[1];
    };

    auto currentDepth = 0.0f;
    auto sampled = height(current);
    for (auto step = 0.0f; step < maximumSteps; step += 1.0f) {
        if (sampled <= currentDepth) break;
        current[0] -= stepDelta[0];
        current[1] -= stepDelta[1];
        currentDepth += layerDepth;
        sampled = height(current);
    }

    const std::array<float, 2> previous{
        current[0] + stepDelta[0], current[1] + stepDelta[1]};
    const auto afterDepth = sampled - currentDepth;
    const auto beforeDepth = height(previous) - currentDepth + layerDepth;
    const auto span = beforeDepth - afterDepth;
    const auto weight = std::abs(span) > 1.0e-6f
        ? std::clamp(beforeDepth / span, 0.0f, 1.0f) : 0.0f;
    const std::array<float, 2> marched{
        current[0] + (previous[0] - current[0]) * weight,
        current[1] + (previous[1] - current[1]) * weight};
    return {
        parallax[2] != 0.0f ? marched[0] / parallax[2]
                                   : texCoord[0],
        parallax[3] != 0.0f ? marched[1] / parallax[3]
                                   : texCoord[1]};
}
// Model-space normals must never pass through the tangent-normal path: the
// two decodes read a different number of channels *and* land in different
// spaces. A model-space texel is already absolute; a tangent-space texel
// stores its Z along the surface normal and has to be rotated into the
// surface frame first.
[[nodiscard]] std::array<float, 3> DecodeReferenceNormal(
    const std::uint8_t encoding,
    const texture::SampledColor& sample,
    const std::array<float, 3>& geometric,
    const float faceSign) noexcept
{
    if (encoding == static_cast<std::uint8_t>(
            material::MaterialNormalEncoding::ModelSpaceRgb)) {
        return {(sample.r * 2.0f - 1.0f) * faceSign,
            (sample.g * 2.0f - 1.0f) * faceSign,
            (sample.b * 2.0f - 1.0f) * faceSign};
    }
    const auto x = sample.r * 2.0f - 1.0f;
    const auto y = sample.g * 2.0f - 1.0f;
    const auto z = std::sqrt(std::max(0.0f, 1.0f - (x * x + y * y)));
    std::array<float, 3> tangent{};
    std::array<float, 3> bitangent{};
    ReferenceOrthonormalBasis(geometric, tangent, bitangent);
    return {tangent[0] * x + bitangent[0] * y + geometric[0] * z,
        tangent[1] * x + bitangent[1] * y + geometric[1] * z,
        tangent[2] * x + bitangent[2] * y + geometric[2] * z};
}

void LiftToHorizon(
    const std::array<float, 3>& geometric,
    std::array<float, 3>& shading) noexcept
{
    const auto alignment = geometric[0] * shading[0] +
        geometric[1] * shading[1] + geometric[2] * shading[2];
    if (alignment <= kReferenceHorizonEpsilon) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            shading[axis] +=
                geometric[axis] * (kReferenceHorizonEpsilon - alignment);
        }
    }
    if (!NormalizeReferenceNormal(shading)) shading = geometric;
}

// Skin and hair are the two classes the general specular/smoothness
// evaluator does not describe, so they are the only ones that adjust the
// lobe. Mirrors vfClassRoughness.
[[nodiscard]] float ClassRoughness(
    const material::FamilyRecordV1& family,
    const float roughness) noexcept
{
    if (family.shaderClass ==
        static_cast<std::uint8_t>(material::ShaderClass::Skin)) {
        return std::clamp(roughness + family.subsurface[0] * 0.5f,
            0.0f, 1.0f);
    }
    if (family.shaderClass ==
            static_cast<std::uint8_t>(material::ShaderClass::Hair) &&
        (family.featureFlags & material::GpuFeatureAnisotropy) != 0) {
        return std::clamp(roughness * 0.5f, 0.0f, 1.0f);
    }
    return roughness;
}

// Emission is authorized by a declaration and never by a bright colour.
[[nodiscard]] std::array<float, 3> ReferenceEmission(
    const material::FamilyRecordV1& family,
    const std::array<float, 3>& glowSample) noexcept
{
    if ((family.emissionFlags & material::GpuEmissionEnabled) == 0) {
        return {};
    }
    // Externally driven emission is owned by the reference, not the
    // material, so the material asserts no colour of its own.
    if ((family.emissionFlags & material::GpuEmissionExternal) != 0) {
        return {};
    }
    std::array<float, 3> emission{family.emissionColor[0],
        family.emissionColor[1], family.emissionColor[2]};
    // Mirrors vfEmission. A glow map is a mask over the declared colour, not
    // a colour of its own: a material that declares one and samples black
    // emits nothing, and one that declares none is unmodulated.
    if ((family.emissionFlags & material::GpuEmissionGlowMap) != 0) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            emission[channel] *= glowSample[channel];
        }
    }
    return emission;
}

// Delegates to lighting::ShadeSurfaceGpu, which is the same evaluation the
// shader runs. Without an environment this is the identity, which is what
// keeps the earlier phases' references byte-identical.
// The engine authors specular colour and smoothness, not metalness, so there
// is no metalness channel to read. The environment-map feature is the closest
// declared signal a captured material gives for a metal-like surface, and it
// is used as one until a real channel is captured. `vfMetalness` in
// phase19/reflection.glsl applies the identical rule, so the approximation is
// shared rather than a place the two sides can disagree.
[[nodiscard]] float ReferenceMetalness(
    const material::FamilyRecordV1& family) noexcept
{
    // FamilyRecordV1::featureFlags already carries the GPU feature bits, not
    // the 64-bit property flags. Reading PropertyFlag::EnvironmentMap here
    // tested bit 7 while the shader tested bit 8, so the two sides disagreed
    // about which surfaces are metal and their reflections differed.
    return (family.featureFlags & material::GpuFeatureEnvironment) != 0
        ? 1.0f : 0.0f;
}

[[nodiscard]] std::array<float, 3> ShadeReferenceSurface(
    const ReferenceInputs& inputs,
    const float albedo[3],
    const std::array<float, 3>& position,
    const std::array<float, 3>& normal) noexcept
{
    const std::array<float, 3> base{albedo[0], albedo[1], albedo[2]};
    if (inputs.environment == nullptr) return base;
    if (inputs.occluders.empty()) {
        return lighting::ShadeSurfaceGpu(
            *inputs.environment, inputs.lights, base, position, normal);
    }
    // One term per light, traced through the same ray rule the shader's
    // query uses. A separate rule here would let the oracle and the mirror
    // disagree about the shadow while both looked internally consistent.
    std::array<float, kMaximumReferenceLights> shadow{};
    const auto count = std::min<std::size_t>(
        inputs.lights.size(), shadow.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto ray = accel::ShadowRayForLight(
            inputs.lights[index], position, normal);
        shadow[index] = (ray.maximumDistance > 0.0f &&
            accel::TraceShadowRay(inputs.occluders, ray, 0, 0).occluded)
            ? 0.0f : 1.0f;
    }
    return lighting::ShadeSurfaceGpu(*inputs.environment, inputs.lights,
        base, position, normal,
        std::span<const float>{shadow.data(), count});
}

}

const std::array<float, 4>& HdrImage::At(
    const std::uint32_t x,
    const std::uint32_t y) const
{
    return pixels[static_cast<std::size_t>(y) * width + x];
}

ScenePacketError RenderReferenceGBuffer(
    const raster::DecodedPacket& projected,
    const ScenePacket& scene,
    const texture::CapturedTexture* const baseColor,
    GBufferImage& image) noexcept
{
    ReferenceInputs inputs{};
    inputs.baseColor = baseColor;
    return RenderReferenceGBuffer(projected, scene, inputs, image, nullptr);
}

ScenePacketError RenderReferenceGBuffer(
    const raster::DecodedPacket& projected,
    const ScenePacket& scene,
    const ReferenceInputs& inputs,
    GBufferImage& image,
    HdrImage* const hdr) noexcept
{
    const auto* const baseColor = inputs.baseColor;
    image = {};
    if (hdr != nullptr) *hdr = {};
    const auto expectedDraws = scene.instances.empty()
        ? scene.objects.size() : scene.instances.size();
    if (ValidateScenePacket(scene) != ScenePacketError::None ||
        projected.header.width == 0 || projected.header.height == 0 ||
        projected.draws.size() != expectedDraws ||
        projected.vertices.empty() || projected.indices.empty()) {
        return ScenePacketError::InvalidDraw;
    }
    try {
        image.width = projected.header.width;
        image.height = projected.header.height;
        const auto pixelCount = static_cast<std::size_t>(image.width) *
            image.height;
        image.pixels.resize(pixelCount);
        if (hdr != nullptr) {
            hdr->width = image.width;
            hdr->height = image.height;
            // The mirror clears colour to the same value the GPU pass does,
            // so an uncovered pixel is comparable rather than arbitrary.
            hdr->pixels.assign(pixelCount,
                std::array<float, 4>{0.01f, 0.021f, 0.04f, 1.0f});
        }
        for (auto& pixel : image.pixels) {
            pixel.albedo[0] = 0.01f;
            pixel.albedo[1] = 0.021f;
            pixel.albedo[2] = 0.04f;
            pixel.albedo[3] = 1.0f;
            pixel.geometricNormalRoughness[3] = 1.0f;
            pixel.shadingNormalDepth[3] = 1.0f;
        }

        const auto screen = [&projected](const raster::RasterVertexV3& vertex) {
            return std::array<float, 3>{
                projected.header.viewportX +
                    (vertex.position[0] * 0.5f + 0.5f) *
                        projected.header.viewportWidth,
                projected.header.viewportY +
                    (vertex.position[1] * 0.5f + 0.5f) *
                        projected.header.viewportHeight,
                projected.header.viewportMinDepth + vertex.position[2] *
                    (projected.header.viewportMaxDepth -
                        projected.header.viewportMinDepth),
            };
        };
        const auto edge = [](const std::array<float, 3>& a,
                             const std::array<float, 3>& b,
                             const float x,
                             const float y) {
            return (b[0] - a[0]) * (y - a[1]) -
                (b[1] - a[1]) * (x - a[0]);
        };

        const raster::MaterialRegistry registry{projected.materials};
        if (registry.HasDuplicateIds()) {
            image = {};
            return ScenePacketError::MissingMaterial;
        }
        std::size_t drawIndex = 0;
        for (std::size_t objectIndex = 0;
             objectIndex < scene.objects.size(); ++objectIndex) {
            const auto& object = scene.objects[objectIndex];
            const auto range = ObjectInstanceRange(scene, objectIndex);
            for (std::uint32_t element = 0; element < range.count;
                 ++element, ++drawIndex) {
            const auto instance = ResolveInstance(
                scene, objectIndex, range.first + element);
            const auto& draw = projected.draws[drawIndex];
            const auto* material = registry.Resolve(draw.materialId);
            if (material == nullptr || draw.materialId != object.materialId) {
                image = {};
                return ScenePacketError::MissingMaterial;
            }
            for (std::uint32_t local = 0; local < draw.indexCount; local += 3) {
                const auto indexA = static_cast<std::size_t>(
                    projected.indices[draw.firstIndex + local]);
                const auto indexB = static_cast<std::size_t>(
                    projected.indices[draw.firstIndex + local + 1]);
                const auto indexC = static_cast<std::size_t>(
                    projected.indices[draw.firstIndex + local + 2]);
                if (indexA >= projected.vertices.size() ||
                    indexB >= projected.vertices.size() ||
                    indexC >= projected.vertices.size()) {
                    image = {};
                    return ScenePacketError::InvalidDraw;
                }
                const auto& vertexA = projected.vertices[indexA];
                const auto& vertexB = projected.vertices[indexB];
                const auto& vertexC = projected.vertices[indexC];
                const auto visible = ResolveVisibility(scene, objectIndex);
                const auto winding = raster::ClassifyTriangle(
                    vertexA, vertexB, vertexC);
                // A mirrored instance reverses winding, so the face a
                // triangle presents follows the captured determinant.
                const auto effectiveFace = visibility::EffectiveFrontFace(
                    draw.frontFace, visible.modelDeterminant);
                const auto expectedWinding =
                    effectiveFace == raster::FrontFace::CounterClockwise
                    ? raster::TriangleWinding::CounterClockwise
                    : raster::TriangleWinding::Clockwise;
                const auto backFacing = winding != expectedWinding;
                // Only a two-sided surface is shaded from its back; a back
                // face of a single-sided surface is culled.
                if (backFacing &&
                    visible.faceMode != visibility::FaceMode::TwoSided) {
                    continue;
                }
                const auto faceSign = backFacing ? -1.0f : 1.0f;
                // Zero when the caller supplied no reciprocals, which the
                // attribute weighting reads as "screen-space", preserving
                // exactly what every earlier fixture compared against.
                const auto inverseWA = indexA < inputs.inverseW.size()
                    ? inputs.inverseW[indexA] : 0.0f;
                const auto inverseWB = indexB < inputs.inverseW.size()
                    ? inputs.inverseW[indexB] : 0.0f;
                const auto inverseWC = indexC < inputs.inverseW.size()
                    ? inputs.inverseW[indexC] : 0.0f;
                const auto a = screen(vertexA);
                const auto b = screen(vertexB);
                const auto c = screen(vertexC);
                const auto area = edge(a, b, c[0], c[1]);
                if (std::abs(area) <=
                    std::numeric_limits<float>::epsilon()) {
                    continue;
                }
                const auto minX = std::max<std::int32_t>(
                    projected.header.scissorX,
                    static_cast<std::int32_t>(std::floor(
                        std::min({a[0], b[0], c[0]}))));
                const auto minY = std::max<std::int32_t>(
                    projected.header.scissorY,
                    static_cast<std::int32_t>(std::floor(
                        std::min({a[1], b[1], c[1]}))));
                const auto maxX = std::min<std::int32_t>(
                    projected.header.scissorX +
                        static_cast<std::int32_t>(
                            projected.header.scissorWidth) - 1,
                    static_cast<std::int32_t>(std::ceil(
                        std::max({a[0], b[0], c[0]}))));
                const auto maxY = std::min<std::int32_t>(
                    projected.header.scissorY +
                        static_cast<std::int32_t>(
                            projected.header.scissorHeight) - 1,
                    static_cast<std::int32_t>(std::ceil(
                        std::max({a[1], b[1], c[1]}))));
                for (auto y = minY; y <= maxY; ++y) {
                    for (auto x = minX; x <= maxX; ++x) {
                        const auto sampleX = static_cast<float>(x) + 0.5f;
                        const auto sampleY = static_cast<float>(y) + 0.5f;
                        const auto screenA = edge(
                            b, c, sampleX, sampleY) / area;
                        const auto screenB = edge(
                            c, a, sampleX, sampleY) / area;
                        const auto screenC = edge(
                            a, b, sampleX, sampleY) / area;
                        constexpr float edgeTolerance = -1.0e-6f;
                        if (screenA < edgeTolerance ||
                            screenB < edgeTolerance ||
                            screenC < edgeTolerance) {
                            continue;
                        }
                        const auto depth = screenA * a[2] +
                            screenB * b[2] + screenC * c[2];
                        // Coverage and depth come from the screen-space
                        // weights -- a pixel is inside the triangle or it is
                        // not, and NDC z is already linear in screen space.
                        // Attributes are the ones that need correcting, so
                        // the two sets are kept apart rather than one being
                        // quietly used for both.
                        auto weightA = screenA;
                        auto weightB = screenB;
                        auto weightC = screenC;
                        if (inverseWA > 0.0f) {
                            const auto sum = screenA * inverseWA +
                                screenB * inverseWB + screenC * inverseWC;
                            if (sum > 0.0f) {
                                weightA = screenA * inverseWA / sum;
                                weightB = screenB * inverseWB / sum;
                                weightC = screenC * inverseWC / sum;
                            }
                        }
                        auto& destination = image.pixels[
                            static_cast<std::size_t>(y) * image.width +
                            static_cast<std::size_t>(x)];
                        const auto oldDepth =
                            destination.shadingNormalDepth[3];
                        const auto depthPass =
                            draw.depthCompare == raster::DepthCompare::Always ||
                            (draw.depthCompare == raster::DepthCompare::Less &&
                                depth < oldDepth) ||
                            (draw.depthCompare ==
                                raster::DepthCompare::LessOrEqual &&
                                depth <= oldDepth);
                        if (!depthPass) continue;
                        // Alpha coverage is evaluated through the same
                        // contract the shader uses, so a cutout silhouette
                        // cannot differ by interpretation.
                        // A null texture means every surface samples opaque
                        // white, which is what makes the alpha-unaware
                        // overload a pure special case of this one.
                        // Before the base colour is sampled, because a height
                        // march decides which texel that sample reads. The
                        // family names the height map and the normals give
                        // the march its surface frame, so both are resolved
                        // first and reused below rather than recomputed.
                        const auto family = inputs.families != nullptr
                            ? material::ResolveFamilyRecord(
                                *inputs.families, instance.objectId)
                            : material::ResolveFamilyRecord(
                                kEmptyFamilies, instance.objectId);
                        const auto surfaceNormals = ComputeReferenceNormals(
                            vertexA, vertexB, vertexC, weightA, weightB,
                            weightC, instance, object, faceSign);
                        auto shadedU = weightA * vertexA.texCoord[0] +
                            weightB * vertexB.texCoord[0] +
                            weightC * vertexC.texCoord[0];
                        auto shadedV = weightA * vertexA.texCoord[1] +
                            weightB * vertexB.texCoord[1] +
                            weightC * vertexC.texCoord[1];
                        if (inputs.heightMap != nullptr &&
                            (family.featureFlags &
                                material::GpuFeatureParallaxOcclusion) != 0) {
                            // The direction back to the camera at this pixel,
                            // which is what the march travels along. The
                            // camera sits at the origin of this space, so the
                            // surface point negated is the direction to it.
                            std::array<float, 3> toViewer{0.0f, 0.0f, 1.0f};
                            if (inputs.vertexPositions.size() > indexC) {
                                for (std::size_t axis = 0; axis < 3; ++axis) {
                                    toViewer[axis] = -(
                                        weightA *
                                            inputs.vertexPositions[indexA][axis] +
                                        weightB *
                                            inputs.vertexPositions[indexB][axis] +
                                        weightC *
                                            inputs.vertexPositions[indexC][axis]);
                                }
                                static_cast<void>(
                                    NormalizeReferenceNormal(toViewer));
                            }
                            const auto marched = ReferenceParallaxOffset(
                                family.parallax, family.wetnessHigh[2],
                                family.wetnessHigh[3], *inputs.heightMap,
                                {shadedU, shadedV},
                                surfaceNormals.geometric, toViewer);
                            shadedU = marched[0];
                            shadedV = marched[1];
                        }
                        float sampledColor[4]{1.0f, 1.0f, 1.0f, 1.0f};
                        if (baseColor != nullptr) {
                            const auto u = shadedU;
                            const auto v = shadedV;
                            texture::SampledColor sampled{};
                            if (texture::SampleTexture2D(*baseColor, u, v,
                                    0.0f, sampled) ==
                                texture::TexturePacketError::None) {
                                sampledColor[0] = sampled.r;
                                sampledColor[1] = sampled.g;
                                sampledColor[2] = sampled.b;
                                sampledColor[3] = sampled.a;
                            }
                        }
                        const auto surfaceAlpha = sampledColor[3] *
                            material->baseColor[3] * instance.parameters[3];
                        visibility::CoverageContext coverage{};
                        coverage.pixelX = static_cast<std::uint32_t>(x);
                        coverage.pixelY = static_cast<std::uint32_t>(y);
                        const auto covered = visibility::EvaluateCoverage(
                            visible.alpha, surfaceAlpha, coverage);
                        if (!covered.covered) {
                            continue;
                        }
                        float shaded[3]{};
                        for (std::size_t channel = 0; channel < 3; ++channel) {
                            const auto interpolated =
                                weightA * vertexA.color[channel] +
                                weightB * vertexB.color[channel] +
                                weightC * vertexC.color[channel];
                            // Per-instance material data modulates only its
                            // own instance.
                            shaded[channel] =
                                interpolated * material->baseColor[channel] *
                                sampledColor[channel] *
                                instance.parameters[channel];
                            // Tint is a declaration, not an observation: a
                            // captured tint colour alone never tints.
                            if (family.tintColor[3] != 0.0f) {
                                shaded[channel] *= family.tintColor[channel];
                            }
                            destination.albedo[channel] =
                                std::clamp(shaded[channel], 0.0f, 1.0f);
                        }
                        // A back face of a two-sided surface is shaded from
                        // the side the viewer is on, so both normals flip
                        // together and stay in the same hemisphere.
                        auto geometric = surfaceNormals.geometric;
                        auto shading = surfaceNormals.shading;
                        if ((family.featureFlags &
                                material::GpuFeatureNormalMap) != 0 &&
                            inputs.normalMap != nullptr) {
                            const auto u = shadedU;
                            const auto v = shadedV;
                            texture::SampledColor normalSample{};
                            if (texture::SampleTexture2D(*inputs.normalMap,
                                    u, v, 0.0f, normalSample) ==
                                texture::TexturePacketError::None) {
                                // The frame is built from the already
                                // face-signed geometric normal, so the
                                // tangent decode needs no second flip.
                                auto decoded = DecodeReferenceNormal(
                                    family.normalEncoding, normalSample,
                                    geometric, faceSign);
                                if (NormalizeReferenceNormal(decoded)) {
                                    shading = decoded;
                                }
                            }
                        }
                        LiftToHorizon(geometric, shading);
                        for (std::size_t channel = 0; channel < 3; ++channel) {
                            destination.geometricNormalRoughness[channel] =
                                geometric[channel];
                            destination.shadingNormalDepth[channel] =
                                shading[channel];
                        }
                        // The stored opacity is the coverage decision's own
                        // opacity. An opaque surface is opaque whatever its
                        // base texture holds in that channel, and a cutout
                        // fragment that survived the test is fully opaque.
                        destination.albedo[3] =
                            std::clamp(covered.coverage, 0.0f, 1.0f);
                        destination.geometricNormalRoughness[3] =
                            ClassRoughness(family, object.roughness);
                        destination.shadingNormalDepth[3] = depth;
                        // The reactive mask. This reference draws the opaque
                        // scene, which decides none of it: opaque geometry
                        // is the stable part of the frame, the part an
                        // upscaler reprojects from history, and marking it
                        // reactive would tell the upscaler to distrust the
                        // whole image. Only the transparent pass raises it.
                        destination.reactive = 0.0f;
                        // Mirrors the reactive plane's spare lanes in
                        // family_scene.frag: the reflection's classification
                        // and the geometry it found. Zero on both means a
                        // pixel whose reflection was never evaluated, which
                        // is what an unreflective surface reports on the
                        // device too.
                        destination.reserved[0] = 0.0f;
                        destination.reserved[1] = 0.0f;
                        destination.reserved[2] = 0.0f;
                        if (hdr != nullptr) {
                            // The glow mask at this point, sampled through
                            // the same coordinates the base colour used, so
                            // an emission mask and the surface it masks
                            // cannot disagree about where they are.
                            std::array<float, 3> glowSample{1.0f, 1.0f, 1.0f};
                            if (inputs.glowMap != nullptr) {
                                const auto glowU =
                                    weightA * vertexA.texCoord[0] +
                                    weightB * vertexB.texCoord[0] +
                                    weightC * vertexC.texCoord[0];
                                const auto glowV =
                                    weightA * vertexA.texCoord[1] +
                                    weightB * vertexB.texCoord[1] +
                                    weightC * vertexC.texCoord[1];
                                texture::SampledColor masked{};
                                if (texture::SampleTexture2D(*inputs.glowMap,
                                        glowU, glowV, 0.0f, masked) ==
                                    texture::TexturePacketError::None) {
                                    glowSample = {masked.r, masked.g,
                                        masked.b};
                                }
                            }
                            const auto emission =
                                ReferenceEmission(family, glowSample);

                            // The shaded point in the same camera-relative
                            // space the light positions were narrowed into.
                            // The projected vertices hold clip space, so
                            // this reads the world positions projection
                            // preserved alongside them.
                            std::array<float, 3> point{};
                            if (inputs.vertexPositions.size() > indexC) {
                                const auto& worldA =
                                    inputs.vertexPositions[indexA];
                                const auto& worldB =
                                    inputs.vertexPositions[indexB];
                                const auto& worldC =
                                    inputs.vertexPositions[indexC];
                                for (std::size_t axis = 0; axis < 3; ++axis) {
                                    point[axis] = weightA * worldA[axis] +
                                        weightB * worldB[axis] +
                                        weightC * worldC[axis];
                                }
                            }
                            const auto lit = ShadeReferenceSurface(
                                inputs, shaded, point, shading);
                            // One bounce of specular, traced against the same
                            // geometry the shadow ray uses. Added rather than
                            // blended because Fresnel has already scaled it
                            // by how much light this surface reflects.
                            std::array<float, 3> reflection{};
                            if (!inputs.reflectionGeometry.empty()) {
                                reflect::ReflectionSurface reflective{};
                                reflective.position = point;
                                reflective.geometricNormal = geometric;
                                reflective.shadingNormal = shading;
                                std::array<float, 3> toViewer{
                                    -point[0], -point[1], -point[2]};
                                reflective.viewDirection = toViewer;
                                reflective.baseColor = {shaded[0], shaded[1],
                                    shaded[2]};
                                reflective.roughness =
                                    ClassRoughness(family, object.roughness);
                                reflective.metalness =
                                    ReferenceMetalness(family);
                                // The lobe over as many directions as the
                                // policy asks for, walking the same sequence
                                // the shader walks so the two integrate one
                                // set of directions rather than two
                                // estimates of the same integral.
                                const auto samples = std::max<std::uint32_t>(
                                    1, inputs.reflectionPolicy
                                        .samplesPerPixel);
                                for (std::uint32_t sampleIndex = 0;
                                     sampleIndex < samples; ++sampleIndex) {
                                    const auto reflected =
                                        reflect::EvaluateReflection(reflective,
                                            inputs.reflectionPolicy,
                                            inputs.reflectionGeometry,
                                            inputs.lights,
                                            inputs.environment != nullptr
                                                ? *inputs.environment
                                                : lighting::GpuEnvironmentV1{},
                                            reflect::SampleSequence(x, y, 0,
                                                sampleIndex),
                                            {0.0f, 0.0f, 0.0f}, false);
                                    for (std::size_t channel = 0;
                                         channel < 3; ++channel) {
                                        reflection[channel] +=
                                            reflected.radiance[channel] /
                                            static_cast<float>(samples);
                                    }
                                    if (sampleIndex != 0) continue;
                                    // What the first ray found, recorded
                                    // beside the radiance so a contract can
                                    // ask whether the two intersectors agree
                                    // about the ray before it asks whether
                                    // they agree about the light. The first
                                    // rather than an average, which would
                                    // name no geometry at all.
                                    destination.reserved[0] =
                                        static_cast<float>(reflected.source);
                                    destination.reserved[1] =
                                        static_cast<float>(
                                            reflected.hitObjectIndex);
                                    destination.reserved[2] =
                                        static_cast<float>(
                                            reflected.hitPrimitiveIndex);
                                }
                            }
                            // One bounce of diffuse indirect, over the same
                            // geometry the reflection traces.
                            std::array<float, 3> indirect{};
                            float bounceProbe = 0.0f;
                            if (inputs.indirectEnabled &&
                                !inputs.reflectionGeometry.empty()) {
                                gi::SurfaceSample bounce{};
                                bounce.position = point;
                                bounce.geometricNormal = geometric;
                                bounce.albedo = {shaded[0], shaded[1],
                                    shaded[2]};
                                gi::IndirectSource ignored{};
                                indirect = gi::EvaluateIndirect(bounce,
                                    inputs.indirectRules,
                                    inputs.indirectPreset,
                                    inputs.reflectionGeometry,
                                    inputs.lights,
                                    inputs.environment != nullptr
                                        ? *inputs.environment
                                        : lighting::GpuEnvironmentV1{},
                                    x, y, 0, ignored, &bounceProbe);
                            }
                            // What every bounce ray of this pixel found.
                            destination.reserved[2] = bounceProbe;
                            auto& colour = hdr->pixels[
                                static_cast<std::size_t>(y) * image.width + x];
                            for (std::size_t channel = 0; channel < 3;
                                 ++channel) {
                                colour[channel] = lit[channel] +
                                    emission[channel] + reflection[channel] +
                                    indirect[channel];
                            }
                            colour[3] =
                                std::clamp(covered.coverage, 0.0f, 1.0f);
                        }
                        WriteId(instance.objectId, destination.objectId);
                        WriteId(object.materialId, destination.materialId);
                    }
                }
            }
            }
        }
        return ScenePacketError::None;
    } catch (...) {
        image = {};
        return ScenePacketError::AllocationFailure;
    }
}

GBufferComparison CompareGBuffer(
    const std::span<const GBufferPixelV1> expected,
    const std::span<const GBufferPixelV1> actual) noexcept
{
    GBufferComparison comparison{};
    comparison.comparedPixels = std::max(expected.size(), actual.size());
    if (expected.size() != actual.size()) {
        comparison.differingPixels = comparison.comparedPixels;
        comparison.identityMismatches = comparison.comparedPixels;
        comparison.maximumAbsoluteError =
            std::numeric_limits<float>::infinity();
        comparison.meanAbsoluteError =
            std::numeric_limits<double>::infinity();
        return comparison;
    }
    double totalError{};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& left = expected[index];
        const auto& right = actual[index];
        const auto identityMismatch =
            left.objectId[0] != right.objectId[0] ||
            left.objectId[1] != right.objectId[1] ||
            left.materialId[0] != right.materialId[0] ||
            left.materialId[1] != right.materialId[1];
        if (identityMismatch) ++comparison.identityMismatches;
        bool differs = identityMismatch;
        // The reactive plane is compared with the shading planes rather than
        // left out. A plane nothing reads is a plane nothing verifies: it was
        // added as a fifth attachment and the whole suite went on passing
        // while the device wrote one and the oracle wrote zero.
        const float* leftChannels[]{left.albedo,
            left.geometricNormalRoughness, left.shadingNormalDepth,
            &left.reactive};
        const float* rightChannels[]{right.albedo,
            right.geometricNormalRoughness, right.shadingNormalDepth,
            &right.reactive};
        // The reactive plane carries one meaningful channel; the other three
        // are the padding the sixteen-byte attachment needs and comparing
        // them would compare reserved bytes.
        constexpr std::size_t kGroupChannels[]{4, 4, 4, 1};
        for (std::size_t group = 0; group < 4; ++group) {
            for (std::size_t channel = 0; channel < kGroupChannels[group];
                 ++channel) {
                const auto error = std::abs(
                    leftChannels[group][channel] -
                    rightChannels[group][channel]);
                if (!std::isfinite(error)) {
                    comparison.maximumAbsoluteError =
                        std::numeric_limits<float>::infinity();
                    totalError = std::numeric_limits<double>::infinity();
                    differs = true;
                } else {
                    if (error > comparison.maximumAbsoluteError) {
                        comparison.maximumAbsoluteError = error;
                        // Which plane and channel carried it. A maximum with
                        // no name says two pictures differ without saying in
                        // what, and the field is the first thing any
                        // investigation needs.
                        comparison.worstGroup =
                            static_cast<std::uint32_t>(group);
                        comparison.worstChannel =
                            static_cast<std::uint32_t>(channel);
                        comparison.worstExpected = leftChannels[group][channel];
                        comparison.worstActual = rightChannels[group][channel];
                        comparison.worstObjectId =
                            static_cast<std::uint64_t>(left.objectId[0]) |
                            (static_cast<std::uint64_t>(left.objectId[1])
                                << 32);
                    }
                    totalError += error;
                    differs = differs || error != 0.0f;
                }
            }
        }
        if (differs) ++comparison.differingPixels;
    }
    // Thirteen: twelve shading channels and the one reactive channel.
    const auto channelCount = static_cast<double>(expected.size()) * 13.0;
    comparison.meanAbsoluteError = channelCount == 0.0
        ? 0.0 : totalError / channelCount;
    return comparison;
}

const char* ToString(const ScenePacketError error) noexcept
{
    switch (error) {
    case ScenePacketError::None: return "none";
    case ScenePacketError::NotImplemented: return "not implemented";
    case ScenePacketError::TruncatedHeader: return "truncated header";
    case ScenePacketError::BadMagic: return "bad magic";
    case ScenePacketError::UnsupportedVersion: return "unsupported version";
    case ScenePacketError::WrongEndian: return "wrong endian";
    case ScenePacketError::SizeMismatch: return "size mismatch";
    case ScenePacketError::ChecksumMismatch: return "checksum mismatch";
    case ScenePacketError::SectionOutOfBounds: return "section out of bounds";
    case ScenePacketError::MisalignedSection: return "misaligned section";
    case ScenePacketError::NonZeroPadding: return "nonzero padding";
    case ScenePacketError::WrongThread: return "wrong thread";
    case ScenePacketError::InvalidIdentity: return "invalid identity";
    case ScenePacketError::InvalidFlags: return "invalid flags";
    case ScenePacketError::InvalidTransform: return "invalid transform";
    case ScenePacketError::InvalidBounds: return "invalid bounds";
    case ScenePacketError::InvalidNormal: return "invalid normal";
    case ScenePacketError::InvalidRoughness: return "invalid roughness";
    case ScenePacketError::DuplicateObject: return "duplicate object";
    case ScenePacketError::DuplicateDraw: return "duplicate draw";
    case ScenePacketError::DuplicateInstance: return "duplicate instance";
    case ScenePacketError::InvalidInstance: return "invalid instance";
    case ScenePacketError::InvalidVisibility: return "invalid visibility";
    case ScenePacketError::InvalidParameters: return "invalid parameters";
    case ScenePacketError::UncoveredObject: return "uncovered object";
    case ScenePacketError::InvalidDraw: return "invalid draw";
    case ScenePacketError::MissingMaterial: return "missing material";
    case ScenePacketError::FrameMismatch: return "frame mismatch";
    case ScenePacketError::ViewMismatch: return "view mismatch";
    case ScenePacketError::UnknownPass: return "unknown pass";
    case ScenePacketError::PassClassMismatch: return "pass class mismatch";
    case ScenePacketError::UnclassifiedWorldWriter:
        return "unclassified world writer";
    case ScenePacketError::UncoveredPass: return "uncovered opaque pass";
    case ScenePacketError::InvalidTransparentDraw:
        return "invalid transparent draw";
    case ScenePacketError::AllocationFailure: return "allocation failure";
    }
    return "unknown";
}

}
