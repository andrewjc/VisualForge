#include "renderer_core/EngineMaterialFamily.h"

#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace vf::renderer::material {

namespace {

[[nodiscard]] bool Finite(const float value) noexcept
{
    return std::isfinite(value);
}

template <std::size_t N>
[[nodiscard]] bool Finite(const std::array<float, N>& values) noexcept
{
    for (const auto value : values) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

[[nodiscard]] bool HasFlag(
    const std::uint64_t flags,
    const std::uint64_t flag) noexcept
{
    return (flags & flag) != 0;
}

[[nodiscard]] bool IsSkinFamily(const MaterialFamily family) noexcept
{
    return family == MaterialFamily::Face ||
        family == MaterialFamily::SkinTint;
}

[[nodiscard]] bool IsLodFamily(const MaterialFamily family) noexcept
{
    return family == MaterialFamily::LodLandscape ||
        family == MaterialFamily::LodObjects ||
        family == MaterialFamily::LodObjectsHd ||
        family == MaterialFamily::LodLandscapeNoise ||
        family == MaterialFamily::LodLandscapeBlend;
}

[[nodiscard]] bool MarchesHeight(const MaterialFamily family) noexcept
{
    return family == MaterialFamily::ParallaxOcclusion ||
        family == MaterialFamily::MultiLayerParallax;
}

[[nodiscard]] bool ReadsHeight(const MaterialFamily family) noexcept
{
    return family == MaterialFamily::Parallax || MarchesHeight(family);
}

// The engine's own resolution order: the family names the surface, and a
// property flag can additionally claim a slot the family did not. Nothing
// here is derived from whether a texture happens to be authored, because an
// authored texture is not a declaration that it is used.
void AssignSlotRoles(
    const MaterialFamily family,
    const std::uint64_t flags,
    std::array<MaterialSlotRole, kShaderTextureSlots>& roles) noexcept
{
    roles.fill(MaterialSlotRole::Unused);
    roles[0] = MaterialSlotRole::BaseColor;
    roles[1] = MaterialSlotRole::Normal;

    // Role 7 is recorded as backlight-mask *and* smooth-spec. The skin and
    // hair families read it as a backlight mask; everything else reads the
    // smoothness/specular mask.
    roles[7] = (IsSkinFamily(family) || family == MaterialFamily::HairTint)
        ? MaterialSlotRole::BacklightMask
        : MaterialSlotRole::SmoothSpec;

    if (family == MaterialFamily::GlowMap || HasFlag(flags,
            PropertyFlag::GlowMap)) {
        roles[2] = MaterialSlotRole::GlowMap;
    }
    if (ReadsHeight(family)) {
        roles[3] = MaterialSlotRole::Height;
    }
    if (family == MaterialFamily::EnvironmentMap ||
        HasFlag(flags, PropertyFlag::EnvironmentMap) ||
        (family == MaterialFamily::Eye &&
            HasFlag(flags, PropertyFlag::EyeReflection))) {
        roles[4] = MaterialSlotRole::Environment;
    }
    if (family == MaterialFamily::Face ||
        HasFlag(flags, PropertyFlag::Face)) {
        roles[5] = MaterialSlotRole::Wrinkles;
    }
    if (family == MaterialFamily::MultiLayerParallax ||
        HasFlag(flags, PropertyFlag::MultiLayerParallax)) {
        roles[6] = MaterialSlotRole::MultiLayer;
    }
}

[[nodiscard]] bool SlotIsRequired(const MaterialSlotRole role) noexcept
{
    // A family that declares one of these cannot render without it. Silently
    // dropping to a flat surface would hide a broken texture set.
    switch (role) {
    case MaterialSlotRole::BaseColor:
    case MaterialSlotRole::Height:
    case MaterialSlotRole::MultiLayer:
    case MaterialSlotRole::GlowMap:
        return true;
    default:
        return false;
    }
}

}

MaterialFamily ClassifyMaterialFamily(const std::int32_t featureId) noexcept
{
    if (featureId == -1) return MaterialFamily::None;
    if (featureId < 0 || featureId > 20) return MaterialFamily::Unknown;
    return static_cast<MaterialFamily>(static_cast<std::uint8_t>(featureId));
}

std::int32_t FeatureIdOf(const MaterialFamily family) noexcept
{
    if (family == MaterialFamily::None) return -1;
    if (family == MaterialFamily::Unknown) return -2;
    return static_cast<std::int32_t>(static_cast<std::uint8_t>(family));
}

ShaderClass ShaderClassOf(const MaterialFamily family) noexcept
{
    switch (family) {
    case MaterialFamily::Face:
    case MaterialFamily::SkinTint:
        return ShaderClass::Skin;
    case MaterialFamily::HairTint:
        return ShaderClass::Hair;
    case MaterialFamily::Eye:
        return ShaderClass::Eye;
    case MaterialFamily::Parallax:
    case MaterialFamily::ParallaxOcclusion:
        return ShaderClass::Parallax;
    case MaterialFamily::MultiLayerParallax:
        return ShaderClass::MultiLayer;
    case MaterialFamily::Landscape:
        return ShaderClass::Terrain;
    case MaterialFamily::LodLandscape:
    case MaterialFamily::LodObjects:
    case MaterialFamily::LodObjectsHd:
    case MaterialFamily::LodLandscapeNoise:
    case MaterialFamily::LodLandscapeBlend:
        return ShaderClass::Lod;
    case MaterialFamily::Default:
    case MaterialFamily::EnvironmentMap:
    case MaterialFamily::GlowMap:
    case MaterialFamily::Snow:
    case MaterialFamily::TreeAnimation:
    case MaterialFamily::MultiIndexSnow:
    case MaterialFamily::Cloud:
    case MaterialFamily::Dismemberment:
        return ShaderClass::Standard;
    case MaterialFamily::None:
    case MaterialFamily::Unknown:
        break;
    }
    // The declared fallback is an ordinary lit surface. It is reached only
    // through the fallback path, which records that it happened.
    return ShaderClass::Standard;
}

FamilyError TranslateMaterialFamily(
    const FamilyCapture& capture,
    FamilyDescriptor& descriptor) noexcept
{
    descriptor = {};
    if (capture.materialId == 0) return FamilyError::InvalidIdentity;
    if (!Finite(capture.emitColor) || !Finite(capture.emitScale) ||
        !Finite(capture.tintColor) || !Finite(capture.subsurfaceRolloff) ||
        !Finite(capture.rimPower) || !Finite(capture.backlightPower) ||
        !Finite(capture.eyeCenter) || !Finite(capture.eyeRadius) ||
        !Finite(capture.eyeIrisScale) || !Finite(capture.parallaxScale) ||
        !Finite(capture.parallaxBias) || !Finite(capture.parallaxUvScale) ||
        !Finite(capture.layerThickness) || !Finite(capture.layerRefraction) ||
        !Finite(capture.wetness)) {
        return FamilyError::NonFiniteSource;
    }

    const auto family = ClassifyMaterialFamily(capture.featureId);
    const auto flags = capture.propertyFlags;
    const auto resolved = (family == MaterialFamily::Unknown ||
                              family == MaterialFamily::None)
        ? MaterialFamily::Default
        : family;

    descriptor.materialId = capture.materialId;
    descriptor.generation = capture.generation;
    descriptor.revision = capture.revision;
    descriptor.staticRevision = capture.staticRevision;
    descriptor.family = family;
    descriptor.shaderClass = ShaderClassOf(resolved);
    descriptor.usedFallback = family != resolved;
    descriptor.provenance = descriptor.usedFallback
        ? MaterialProvenance::CanonicalFallback
        : MaterialProvenance::RuntimeMaterial;
    descriptor.normalEncoding =
        HasFlag(flags, PropertyFlag::ModelSpaceNormals)
        ? MaterialNormalEncoding::ModelSpaceRgb
        : MaterialNormalEncoding::TangentSpaceBc5;
    descriptor.diagnostic.capturedFeatureId = capture.featureId;
    descriptor.diagnostic.capturedFlags = flags;
    descriptor.diagnostic.baseTechniqueId = capture.baseTechniqueId;

    // A fallback must not invent a specialized lobe, so it resolves as the
    // plain lit surface with no flag-driven slots beyond base and normal.
    std::array<MaterialSlotRole, kShaderTextureSlots> roles{};
    if (descriptor.usedFallback) {
        roles.fill(MaterialSlotRole::Unused);
        roles[0] = MaterialSlotRole::BaseColor;
        roles[1] = MaterialSlotRole::Normal;
    } else {
        AssignSlotRoles(resolved, flags, roles);
    }

    for (std::uint32_t slot = 0; slot < kShaderTextureSlots; ++slot) {
        const auto& source = capture.slots[slot];
        auto& destination = descriptor.slots[slot];
        destination.role = roles[slot];
        if (roles[slot] == MaterialSlotRole::Unused) {
            // Slots 8 and 9 carry no recorded role. An authored one is
            // counted so an unclassified texture set is visible rather than
            // quietly ignored.
            if (source.authored) {
                ++descriptor.diagnostic.unclassifiedAuthoredSlots;
            }
            continue;
        }
        if (!source.authored || source.resourceId == 0) {
            if (SlotIsRequired(roles[slot])) {
                return FamilyError::MissingRequiredSlot;
            }
            destination.role = MaterialSlotRole::Unused;
            continue;
        }
        destination.resourceId = source.resourceId;
        destination.generation = source.generation;
    }

    if (descriptor.usedFallback) {
        // Nothing else is asserted about a family this build cannot name.
        return FamilyError::None;
    }

    auto& features = descriptor.features;
    features.environment = roles[4] == MaterialSlotRole::Environment;
    features.landscape = resolved == MaterialFamily::Landscape ||
        HasFlag(flags, PropertyFlag::MultiTextureLandscape);
    features.treeAnimation = resolved == MaterialFamily::TreeAnimation ||
        HasFlag(flags, PropertyFlag::TreeAnimation);
    features.reducedDetail = IsLodFamily(resolved);
    features.sky = resolved == MaterialFamily::Cloud;
    features.snow = resolved == MaterialFamily::Snow ||
        resolved == MaterialFamily::MultiIndexSnow;
    features.multiIndex = resolved == MaterialFamily::MultiIndexSnow;
    features.dismemberment = resolved == MaterialFamily::Dismemberment ||
        HasFlag(flags, PropertyFlag::Dismemberment);
    features.meatCuff = HasFlag(flags,
        PropertyFlag::DismembermentMeatCuff);
    features.anisotropy = HasFlag(flags,
        PropertyFlag::AnisotropicLighting);
    features.eye = resolved == MaterialFamily::Eye;
    features.multiLayer = roles[6] == MaterialSlotRole::MultiLayer;

    // Wetness is a dynamic control set, and its controls are carried for
    // every lighting material because the ABI stores them on the base. Only
    // the snow families declare the feature; the rest keep the values so a
    // later phase can enable them without a capture change.
    descriptor.wetness.controls = capture.wetness;
    features.wetness = features.snow;

    // A LOD surface never pays for a specialized lobe, whatever its captured
    // scalars hold. A distant object with parallax is both wrong and slow.
    if (!features.reducedDetail) {
        features.subsurface = IsSkinFamily(resolved);
        features.rim = features.subsurface;
        features.backlight = features.subsurface;
        features.parallaxOffset = resolved == MaterialFamily::Parallax;
        features.parallaxOcclusion = MarchesHeight(resolved);
    }

    if (features.subsurface) {
        descriptor.subsurface.rolloff = capture.subsurfaceRolloff;
        descriptor.subsurface.rimPower = capture.rimPower;
        descriptor.subsurface.backlightPower = capture.backlightPower;
    }

    if (features.eye) {
        if (!(capture.eyeRadius > 0.0f) || !(capture.eyeIrisScale > 0.0f)) {
            return FamilyError::InvalidEyeTransform;
        }
        descriptor.eye.center = capture.eyeCenter;
        descriptor.eye.radius = capture.eyeRadius;
        descriptor.eye.irisScale = capture.eyeIrisScale;
        descriptor.eye.reflects = HasFlag(flags,
            PropertyFlag::EyeReflection);
    }

    if (features.parallaxOffset || features.parallaxOcclusion) {
        if (capture.parallaxMinSteps == 0 ||
            capture.parallaxMaxSteps < capture.parallaxMinSteps) {
            return FamilyError::InvalidParallaxRange;
        }
        descriptor.parallax.scale = capture.parallaxScale;
        descriptor.parallax.bias = capture.parallaxBias;
        descriptor.parallax.uvScale = capture.parallaxUvScale;
        descriptor.parallax.minimumSteps = capture.parallaxMinSteps;
        descriptor.parallax.maximumSteps = capture.parallaxMaxSteps;
    }

    if (features.multiLayer) {
        // A refraction index below one is not physical and is not clamped
        // into plausibility.
        if (!(capture.layerRefraction >= 1.0f)) {
            return FamilyError::InvalidLayer;
        }
        descriptor.layer.thickness = capture.layerThickness;
        descriptor.layer.refraction = capture.layerRefraction;
    }

    // Tint is declared by the tinting families or by an explicit tint flag.
    // A captured tint colour alone is not a declaration.
    descriptor.tint.enabled = resolved == MaterialFamily::SkinTint ||
        resolved == MaterialFamily::HairTint ||
        HasFlag(flags, PropertyFlag::Tint) ||
        HasFlag(flags, PropertyFlag::HairTint) ||
        HasFlag(flags, PropertyFlag::FaceGenRgbTint);
    if (descriptor.tint.enabled) {
        descriptor.tint.color = capture.tintColor;
    }

    descriptor.palette.colorFromGreyscale =
        HasFlag(flags, PropertyFlag::GreyscaleToPaletteColor);
    descriptor.palette.alphaFromGreyscale =
        HasFlag(flags, PropertyFlag::GreyscaleToPaletteAlpha);

    // Emission is authorized by a declaration and never by a bright colour.
    // A saturated albedo is ordinary in authored content; reading it as
    // emission makes plain surfaces glow.
    const auto glowMapped = roles[2] == MaterialSlotRole::GlowMap;
    const auto ownEmit = HasFlag(flags, PropertyFlag::OwnEmit);
    const auto external = HasFlag(flags, PropertyFlag::ExternalEmittance);
    descriptor.emission.enabled = glowMapped || ownEmit || external;
    descriptor.emission.usesGlowMap = glowMapped;
    descriptor.emission.externallyDriven = external;
    if (descriptor.emission.enabled && !external) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            descriptor.emission.color[channel] =
                capture.emitColor[channel] * capture.emitScale;
        }
    }

    return FamilyError::None;
}

bool RequiresDescriptorRebuild(
    const FamilyDescriptor& previous,
    const FamilyDescriptor& next) noexcept
{
    if (previous.family != next.family ||
        previous.shaderClass != next.shaderClass ||
        previous.normalEncoding != next.normalEncoding ||
        previous.staticRevision != next.staticRevision) {
        return true;
    }
    for (std::uint32_t slot = 0; slot < kShaderTextureSlots; ++slot) {
        if (previous.slots[slot].role != next.slots[slot].role ||
            previous.slots[slot].resourceId !=
                next.slots[slot].resourceId ||
            previous.slots[slot].generation !=
                next.slots[slot].generation) {
            return true;
        }
    }
    return false;
}

bool RequiresDynamicUpdate(
    const FamilyDescriptor& previous,
    const FamilyDescriptor& next) noexcept
{
    return previous.revision != next.revision;
}

GpuFamilyRecordV1 BuildFamilyGpuRecord(
    const FamilyDescriptor& descriptor) noexcept
{
    GpuFamilyRecordV1 record{};
    record.familyPacked =
        static_cast<std::uint32_t>(descriptor.family) |
        (static_cast<std::uint32_t>(descriptor.shaderClass) << 8) |
        (static_cast<std::uint32_t>(descriptor.normalEncoding) << 16);

    const auto& features = descriptor.features;
    const auto set = [](const bool condition,
                         const std::uint32_t bit) noexcept {
        return condition ? bit : 0u;
    };
    record.featureFlags =
        set(features.subsurface, GpuFeatureSubsurface) |
        set(features.rim, GpuFeatureRim) |
        set(features.backlight, GpuFeatureBacklight) |
        set(features.anisotropy, GpuFeatureAnisotropy) |
        set(features.parallaxOffset, GpuFeatureParallaxOffset) |
        set(features.parallaxOcclusion, GpuFeatureParallaxOcclusion) |
        set(features.multiLayer, GpuFeatureMultiLayer) |
        set(features.eye, GpuFeatureEye) |
        set(features.environment, GpuFeatureEnvironment) |
        set(features.snow, GpuFeatureSnow) |
        set(features.multiIndex, GpuFeatureMultiIndex) |
        set(features.wetness, GpuFeatureWetness) |
        set(features.landscape, GpuFeatureLandscape) |
        set(features.treeAnimation, GpuFeatureTreeAnimation) |
        set(features.dismemberment, GpuFeatureDismemberment) |
        set(features.meatCuff, GpuFeatureMeatCuff) |
        set(features.reducedDetail, GpuFeatureReducedDetail) |
        set(features.sky, GpuFeatureSky) |
        set(descriptor.slots[1].role == MaterialSlotRole::Normal &&
                descriptor.slots[1].resourceId != 0,
            GpuFeatureNormalMap);

    record.emissionFlags =
        set(descriptor.emission.enabled, GpuEmissionEnabled) |
        set(descriptor.emission.usesGlowMap, GpuEmissionGlowMap) |
        set(descriptor.emission.externallyDriven, GpuEmissionExternal);
    record.paletteFlags =
        set(descriptor.palette.colorFromGreyscale, GpuPaletteColor) |
        set(descriptor.palette.alphaFromGreyscale, GpuPaletteAlpha);

    for (std::size_t channel = 0; channel < 3; ++channel) {
        record.emissionColor[channel] = descriptor.emission.color[channel];
        record.tintColor[channel] = descriptor.tint.color[channel];
    }
    // The enable rides in w so a zero tint colour and an absent tint stay
    // distinguishable on the GPU.
    record.tintColor[3] = descriptor.tint.enabled ? 1.0f : 0.0f;

    record.subsurface[0] = descriptor.subsurface.rolloff;
    record.subsurface[1] = descriptor.subsurface.rimPower;
    record.subsurface[2] = descriptor.subsurface.backlightPower;

    record.parallax[0] = descriptor.parallax.scale;
    record.parallax[1] = descriptor.parallax.bias;
    record.parallax[2] = descriptor.parallax.uvScale[0];
    record.parallax[3] = descriptor.parallax.uvScale[1];

    for (std::size_t axis = 0; axis < 3; ++axis) {
        record.eyeCenterRadius[axis] = descriptor.eye.center[axis];
    }
    record.eyeCenterRadius[3] = descriptor.eye.radius;

    record.layerAndEye[0] = descriptor.layer.thickness;
    record.layerAndEye[1] = descriptor.layer.refraction;
    record.layerAndEye[2] = descriptor.eye.irisScale;
    record.layerAndEye[3] = descriptor.eye.reflects ? 1.0f : 0.0f;

    for (std::size_t index = 0; index < 4; ++index) {
        record.wetnessLow[index] = descriptor.wetness.controls[index];
    }
    record.wetnessHigh[0] = descriptor.wetness.controls[4];
    record.wetnessHigh[1] = descriptor.wetness.controls[5];
    // The step range is integral and has no float home in the payload, so it
    // rides in the unused wetness lanes rather than being dropped.
    record.wetnessHigh[2] =
        static_cast<float>(descriptor.parallax.minimumSteps);
    record.wetnessHigh[3] =
        static_cast<float>(descriptor.parallax.maximumSteps);
    return record;
}

FamilyRecordV1 MakeFamilyRecord(
    const FamilyDescriptor& descriptor,
    const std::uint64_t objectId) noexcept
{
    FamilyRecordV1 record{};
    record.materialId = descriptor.materialId;
    record.objectId = objectId;
    record.generation = descriptor.generation;
    record.revision = descriptor.revision;
    record.staticRevision = descriptor.staticRevision;
    record.family = static_cast<std::uint8_t>(descriptor.family);
    record.shaderClass = static_cast<std::uint8_t>(descriptor.shaderClass);
    record.provenance = static_cast<std::uint8_t>(descriptor.provenance);
    record.normalEncoding =
        static_cast<std::uint8_t>(descriptor.normalEncoding);
    record.usedFallback = descriptor.usedFallback ? 1u : 0u;
    record.capturedFeatureId = descriptor.diagnostic.capturedFeatureId;
    record.capturedFlags = descriptor.diagnostic.capturedFlags;
    record.baseTechniqueId = descriptor.diagnostic.baseTechniqueId;
    record.unclassifiedAuthoredSlots =
        descriptor.diagnostic.unclassifiedAuthoredSlots;

    // The GPU projection already agrees on the payload lanes, so building it
    // once here keeps the wire record and the shader record from drifting.
    const auto gpu = BuildFamilyGpuRecord(descriptor);
    record.featureFlags = gpu.featureFlags;
    record.emissionFlags = gpu.emissionFlags;
    record.paletteFlags = gpu.paletteFlags;
    std::memcpy(record.emissionColor, gpu.emissionColor,
        sizeof(record.emissionColor));
    std::memcpy(record.tintColor, gpu.tintColor, sizeof(record.tintColor));
    std::memcpy(record.subsurface, gpu.subsurface, sizeof(record.subsurface));
    std::memcpy(record.parallax, gpu.parallax, sizeof(record.parallax));
    std::memcpy(record.eyeCenterRadius, gpu.eyeCenterRadius,
        sizeof(record.eyeCenterRadius));
    std::memcpy(record.layerAndEye, gpu.layerAndEye,
        sizeof(record.layerAndEye));
    std::memcpy(record.wetnessLow, gpu.wetnessLow, sizeof(record.wetnessLow));
    std::memcpy(record.wetnessHigh, gpu.wetnessHigh,
        sizeof(record.wetnessHigh));

    for (std::uint32_t slot = 0; slot < kShaderTextureSlots; ++slot) {
        record.slots[slot].resourceId = descriptor.slots[slot].resourceId;
        record.slots[slot].generation = descriptor.slots[slot].generation;
        record.slots[slot].role =
            static_cast<std::uint8_t>(descriptor.slots[slot].role);
    }
    return record;
}

FamilyPacketError ValidateFamilyPacket(const FamilyPacket& packet) noexcept
{
    if (packet.records.size() > kMaximumFamilyRecords) {
        return FamilyPacketError::TooManyRecords;
    }
    for (std::size_t index = 0; index < packet.records.size(); ++index) {
        const auto& record = packet.records[index];
        if (record.materialId == 0 || record.objectId == 0) {
            return FamilyPacketError::InvalidIdentity;
        }
        if (record.shaderClass ==
                static_cast<std::uint8_t>(ShaderClass::Unknown) ||
            record.shaderClass >
                static_cast<std::uint8_t>(ShaderClass::Lod)) {
            return FamilyPacketError::UnclassifiedRecord;
        }
        if (record.family > static_cast<std::uint8_t>(
                MaterialFamily::Unknown)) {
            return FamilyPacketError::UnclassifiedRecord;
        }
        const float* const payloads[]{record.emissionColor,
            record.tintColor, record.subsurface, record.parallax,
            record.eyeCenterRadius, record.layerAndEye, record.wetnessLow,
            record.wetnessHigh};
        for (const auto* payload : payloads) {
            for (std::size_t lane = 0; lane < 4; ++lane) {
                if (!std::isfinite(payload[lane])) {
                    return FamilyPacketError::NonFiniteValue;
                }
            }
        }
        for (std::uint32_t slot = 0; slot < kShaderTextureSlots; ++slot) {
            if (record.slots[slot].role >
                static_cast<std::uint8_t>(MaterialSlotRole::SmoothSpec)) {
                return FamilyPacketError::InvalidSlotRole;
            }
            for (const auto pad : record.slots[slot].reserved0) {
                if (pad != 0) return FamilyPacketError::NonZeroPadding;
            }
        }
        for (const auto pad : record.reserved0) {
            if (pad != 0) return FamilyPacketError::NonZeroPadding;
        }
        if (record.reserved1 != 0 || record.reserved2 != 0) {
            return FamilyPacketError::NonZeroPadding;
        }
        // Two records claiming the same object cannot both own it.
        for (std::size_t other = 0; other < index; ++other) {
            if (packet.records[other].objectId == record.objectId) {
                return FamilyPacketError::DuplicateRecord;
            }
        }
    }
    return FamilyPacketError::None;
}

FamilyPacketError EncodeFamilyPacket(
    const FamilyPacket& packet,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    const auto validation = ValidateFamilyPacket(packet);
    if (validation != FamilyPacketError::None) return validation;
    try {
        FamilyPacketHeaderV1 header{};
        header.frameId = packet.header.frameId;
        header.viewId = packet.header.viewId;
        header.recordCount =
            static_cast<std::uint32_t>(packet.records.size());
        header.recordsOffset = sizeof(FamilyPacketHeaderV1);
        const auto recordBytes =
            packet.records.size() * sizeof(FamilyRecordV1);
        if (recordBytes >
            std::numeric_limits<std::uint32_t>::max() - sizeof(header)) {
            return FamilyPacketError::AllocationFailure;
        }
        header.totalSize =
            static_cast<std::uint32_t>(sizeof(header) + recordBytes);
        bytes.resize(header.totalSize);
        if (recordBytes != 0) {
            std::memcpy(bytes.data() + sizeof(header), packet.records.data(),
                recordBytes);
        }
        header.payloadCrc32 = trace::Crc32(
            std::span<const std::byte>{bytes}.subspan(sizeof(header)));
        std::memcpy(bytes.data(), &header, sizeof(header));
        return FamilyPacketError::None;
    } catch (const std::bad_alloc&) {
        bytes.clear();
        return FamilyPacketError::AllocationFailure;
    }
}

FamilyPacketError DecodeFamilyPacket(
    const std::span<const std::byte> bytes,
    FamilyPacket& packet) noexcept
{
    packet = {};
    if (bytes.size() < sizeof(FamilyPacketHeaderV1)) {
        return FamilyPacketError::TruncatedHeader;
    }
    FamilyPacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kFamilyPacketMagic) {
        return FamilyPacketError::BadMagic;
    }
    if (header.endianMarker != kFamilyPacketEndian) {
        return FamilyPacketError::WrongEndian;
    }
    if (header.versionMajor != kFamilyPacketVersionMajor ||
        header.versionMinor > kFamilyPacketVersionMinor) {
        return FamilyPacketError::UnsupportedVersion;
    }
    if (header.headerSize != sizeof(FamilyPacketHeaderV1)) {
        return FamilyPacketError::SizeMismatch;
    }
    if (header.totalSize != bytes.size()) {
        return FamilyPacketError::SizeMismatch;
    }
    if (header.recordCount > kMaximumFamilyRecords) {
        return FamilyPacketError::TooManyRecords;
    }
    if (header.recordsOffset % alignof(FamilyRecordV1) != 0) {
        return FamilyPacketError::MisalignedSection;
    }
    const auto recordBytes =
        static_cast<std::size_t>(header.recordCount) * sizeof(FamilyRecordV1);
    if (header.recordsOffset != sizeof(FamilyPacketHeaderV1) ||
        recordBytes + header.recordsOffset != bytes.size()) {
        return FamilyPacketError::SectionOutOfBounds;
    }
    if (trace::Crc32(bytes.subspan(sizeof(header))) != header.payloadCrc32) {
        return FamilyPacketError::ChecksumMismatch;
    }
    if (header.reserved0 != 0 || header.reserved1 != 0) {
        return FamilyPacketError::NonZeroPadding;
    }
    try {
        packet.header = header;
        packet.records.resize(header.recordCount);
        if (recordBytes != 0) {
            std::memcpy(packet.records.data(),
                bytes.data() + header.recordsOffset, recordBytes);
        }
    } catch (const std::bad_alloc&) {
        packet = {};
        return FamilyPacketError::AllocationFailure;
    }
    const auto validation = ValidateFamilyPacket(packet);
    if (validation != FamilyPacketError::None) {
        packet = {};
        return validation;
    }
    return FamilyPacketError::None;
}

FamilyRecordV1 ResolveFamilyRecord(
    const FamilyPacket& packet,
    const std::uint64_t objectId) noexcept
{
    for (const auto& record : packet.records) {
        if (record.objectId == objectId) return record;
    }
    FamilyRecordV1 implicit{};
    implicit.objectId = objectId;
    implicit.family = static_cast<std::uint8_t>(MaterialFamily::Default);
    implicit.shaderClass = static_cast<std::uint8_t>(ShaderClass::Standard);
    implicit.provenance =
        static_cast<std::uint8_t>(MaterialProvenance::MaterialDefault);
    implicit.normalEncoding = static_cast<std::uint8_t>(
        MaterialNormalEncoding::TangentSpaceBc5);
    implicit.capturedFeatureId = -1;
    implicit.slots[0].role =
        static_cast<std::uint8_t>(MaterialSlotRole::BaseColor);
    implicit.slots[1].role =
        static_cast<std::uint8_t>(MaterialSlotRole::Normal);
    implicit.slots[7].role =
        static_cast<std::uint8_t>(MaterialSlotRole::SmoothSpec);
    return implicit;
}

GpuFamilyRecordV1 BuildFamilyGpuRecord(
    const FamilyRecordV1& record) noexcept
{
    GpuFamilyRecordV1 gpu{};
    gpu.familyPacked = static_cast<std::uint32_t>(record.family) |
        (static_cast<std::uint32_t>(record.shaderClass) << 8) |
        (static_cast<std::uint32_t>(record.normalEncoding) << 16);
    gpu.featureFlags = record.featureFlags;
    gpu.emissionFlags = record.emissionFlags;
    gpu.paletteFlags = record.paletteFlags;
    std::memcpy(gpu.emissionColor, record.emissionColor,
        sizeof(gpu.emissionColor));
    std::memcpy(gpu.tintColor, record.tintColor, sizeof(gpu.tintColor));
    std::memcpy(gpu.subsurface, record.subsurface, sizeof(gpu.subsurface));
    std::memcpy(gpu.parallax, record.parallax, sizeof(gpu.parallax));
    std::memcpy(gpu.eyeCenterRadius, record.eyeCenterRadius,
        sizeof(gpu.eyeCenterRadius));
    std::memcpy(gpu.layerAndEye, record.layerAndEye, sizeof(gpu.layerAndEye));
    std::memcpy(gpu.wetnessLow, record.wetnessLow, sizeof(gpu.wetnessLow));
    std::memcpy(gpu.wetnessHigh, record.wetnessHigh, sizeof(gpu.wetnessHigh));
    return gpu;
}

const char* ToString(const FamilyPacketError error) noexcept
{
    switch (error) {
    case FamilyPacketError::None: return "None";
    case FamilyPacketError::TruncatedHeader: return "TruncatedHeader";
    case FamilyPacketError::BadMagic: return "BadMagic";
    case FamilyPacketError::UnsupportedVersion: return "UnsupportedVersion";
    case FamilyPacketError::WrongEndian: return "WrongEndian";
    case FamilyPacketError::SizeMismatch: return "SizeMismatch";
    case FamilyPacketError::ChecksumMismatch: return "ChecksumMismatch";
    case FamilyPacketError::SectionOutOfBounds: return "SectionOutOfBounds";
    case FamilyPacketError::MisalignedSection: return "MisalignedSection";
    case FamilyPacketError::NonZeroPadding: return "NonZeroPadding";
    case FamilyPacketError::TooManyRecords: return "TooManyRecords";
    case FamilyPacketError::InvalidIdentity: return "InvalidIdentity";
    case FamilyPacketError::DuplicateRecord: return "DuplicateRecord";
    case FamilyPacketError::UnclassifiedRecord: return "UnclassifiedRecord";
    case FamilyPacketError::NonFiniteValue: return "NonFiniteValue";
    case FamilyPacketError::InvalidSlotRole: return "InvalidSlotRole";
    case FamilyPacketError::AllocationFailure: return "AllocationFailure";
    }
    return "Unknown";
}

const char* ToString(const FamilyError error) noexcept
{
    switch (error) {
    case FamilyError::None: return "None";
    case FamilyError::InvalidIdentity: return "InvalidIdentity";
    case FamilyError::NonFiniteSource: return "NonFiniteSource";
    case FamilyError::InvalidEyeTransform: return "InvalidEyeTransform";
    case FamilyError::InvalidParallaxRange: return "InvalidParallaxRange";
    case FamilyError::InvalidLayer: return "InvalidLayer";
    case FamilyError::MissingRequiredSlot: return "MissingRequiredSlot";
    }
    return "Unknown";
}

const char* ToString(const MaterialFamily family) noexcept
{
    switch (family) {
    case MaterialFamily::Default: return "Default";
    case MaterialFamily::EnvironmentMap: return "EnvironmentMap";
    case MaterialFamily::GlowMap: return "GlowMap";
    case MaterialFamily::Parallax: return "Parallax";
    case MaterialFamily::Face: return "Face";
    case MaterialFamily::SkinTint: return "SkinTint";
    case MaterialFamily::HairTint: return "HairTint";
    case MaterialFamily::ParallaxOcclusion: return "ParallaxOcclusion";
    case MaterialFamily::Landscape: return "Landscape";
    case MaterialFamily::LodLandscape: return "LodLandscape";
    case MaterialFamily::Snow: return "Snow";
    case MaterialFamily::MultiLayerParallax: return "MultiLayerParallax";
    case MaterialFamily::TreeAnimation: return "TreeAnimation";
    case MaterialFamily::LodObjects: return "LodObjects";
    case MaterialFamily::MultiIndexSnow: return "MultiIndexSnow";
    case MaterialFamily::LodObjectsHd: return "LodObjectsHd";
    case MaterialFamily::Eye: return "Eye";
    case MaterialFamily::Cloud: return "Cloud";
    case MaterialFamily::LodLandscapeNoise: return "LodLandscapeNoise";
    case MaterialFamily::LodLandscapeBlend: return "LodLandscapeBlend";
    case MaterialFamily::Dismemberment: return "Dismemberment";
    case MaterialFamily::None: return "None";
    case MaterialFamily::Unknown: return "Unknown";
    }
    return "Unknown";
}

}
