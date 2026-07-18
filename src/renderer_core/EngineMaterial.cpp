#include "renderer_core/EngineMaterial.h"

#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>

namespace vf::renderer::material {

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool IsValidProvenance(const MaterialProvenance value) noexcept
{
    return value >= MaterialProvenance::Missing &&
        value <= MaterialProvenance::ShaderProperty;
}

bool IsValidRole(const MaterialTextureRole value) noexcept
{
    return value >= MaterialTextureRole::BaseColor &&
        value <= MaterialTextureRole::SmoothSpec;
}

template <std::size_t Size>
bool Finite(const std::array<float, Size>& value) noexcept
{
    return std::all_of(value.begin(), value.end(),
        [](const float component) { return std::isfinite(component); });
}

MaterialError ResolveScalar(
    const std::vector<ScalarSource>& sources,
    const float fallback,
    const float minimum,
    const float maximum,
    ResolvedScalar& resolved) noexcept
{
    bool found{};
    ScalarSource selected{};
    for (const auto& source : sources) {
        if (!IsValidProvenance(source.provenance)) {
            return MaterialError::InvalidProvenance;
        }
        if (source.provenance == MaterialProvenance::Missing) continue;
        if (!std::isfinite(source.value)) {
            return MaterialError::NonFiniteSource;
        }
        if (!found || source.provenance > selected.provenance) {
            selected = source;
            found = true;
        } else if (source.provenance == selected.provenance &&
            source.value != selected.value) {
            return MaterialError::AmbiguousSource;
        }
    }
    if (!found) {
        selected = {fallback, MaterialProvenance::MaterialDefault};
    }
    resolved.raw = selected.value;
    resolved.value = std::clamp(selected.value, minimum, maximum);
    resolved.provenance = selected.provenance;
    resolved.clamped = resolved.raw != resolved.value;
    return MaterialError::None;
}

template <std::size_t Size, class Source, class Resolved>
MaterialError ResolveVector(
    const std::vector<Source>& sources,
    const std::array<float, Size>& fallback,
    const float minimum,
    const float maximum,
    Resolved& resolved) noexcept
{
    bool found{};
    Source selected{};
    for (const auto& source : sources) {
        if (!IsValidProvenance(source.provenance)) {
            return MaterialError::InvalidProvenance;
        }
        if (source.provenance == MaterialProvenance::Missing) continue;
        if (!Finite(source.value)) return MaterialError::NonFiniteSource;
        if (!found || source.provenance > selected.provenance) {
            selected = source;
            found = true;
        } else if (source.provenance == selected.provenance &&
            source.value != selected.value) {
            return MaterialError::AmbiguousSource;
        }
    }
    if (!found) {
        selected.value = fallback;
        selected.provenance = MaterialProvenance::MaterialDefault;
    }
    resolved.raw = selected.value;
    resolved.value = selected.value;
    for (auto& component : resolved.value) {
        component = std::clamp(component, minimum, maximum);
    }
    resolved.provenance = selected.provenance;
    resolved.clamped = resolved.raw != resolved.value;
    return MaterialError::None;
}

texture::FallbackTextureRole FallbackRole(
    const MaterialTextureRole role) noexcept
{
    switch (role) {
    case MaterialTextureRole::BaseColor:
        return texture::FallbackTextureRole::White;
    case MaterialTextureRole::Normal:
        return texture::FallbackTextureRole::FlatNormal;
    case MaterialTextureRole::SmoothSpec:
        return texture::FallbackTextureRole::NeutralMask;
    }
    return texture::FallbackTextureRole::White;
}

MaterialColorSpace RequiredColorSpace(
    const MaterialTextureRole role) noexcept
{
    return role == MaterialTextureRole::BaseColor
        ? MaterialColorSpace::Srgb : MaterialColorSpace::Linear;
}

bool SameTextureSource(
    const TextureSource& first,
    const TextureSource& second) noexcept
{
    return first.role == second.role &&
        first.resourceId == second.resourceId &&
        first.generation == second.generation &&
        first.viewFormat == second.viewFormat;
}

MaterialError ResolveTexture(
    const MaterialCapture& capture,
    const MaterialTextureRole role,
    ResolvedTexture& resolved)
{
    bool found{};
    TextureSource selected{};
    for (const auto& source : capture.textures) {
        if (!IsValidRole(source.role)) {
            return MaterialError::InvalidTextureRole;
        }
        if (!IsValidProvenance(source.provenance)) {
            return MaterialError::InvalidProvenance;
        }
        if (source.role != role ||
            source.provenance == MaterialProvenance::Missing) continue;
        if (source.resourceId == 0 || source.generation == 0) {
            return MaterialError::InvalidTexture;
        }
        texture::TextureFormatInfo info{};
        if (texture::ResolveTextureFormat(
                source.viewFormat, source.viewFormat, info) !=
            texture::TexturePacketError::None) {
            return MaterialError::InvalidTexture;
        }
        const auto wantsSrgb = role == MaterialTextureRole::BaseColor;
        if (info.srgb != wantsSrgb) {
            return MaterialError::InvalidColorSpace;
        }
        if (!found || source.provenance > selected.provenance) {
            selected = source;
            found = true;
        } else if (source.provenance == selected.provenance &&
            !SameTextureSource(source, selected)) {
            return MaterialError::AmbiguousSource;
        }
    }
    resolved.role = role;
    resolved.colorSpace = RequiredColorSpace(role);
    if (found) {
        resolved.resourceId = selected.resourceId;
        resolved.generation = selected.generation;
        resolved.viewFormat = selected.viewFormat;
        resolved.provenance = selected.provenance;
        resolved.authored = true;
        return MaterialError::None;
    }
    const auto fallback = texture::MakeFallbackTexture(FallbackRole(role));
    resolved.resourceId = fallback.resourceId;
    resolved.generation = fallback.generation;
    resolved.viewFormat = fallback.viewFormat;
    resolved.provenance = MaterialProvenance::CanonicalFallback;
    resolved.authored = false;
    return MaterialError::None;
}

float Unit(const float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

float Dot(
    const std::array<float, 3>& first,
    const std::array<float, 3>& second) noexcept
{
    return first[0] * second[0] + first[1] * second[1] +
        first[2] * second[2];
}

std::array<float, 3> Normalize(std::array<float, 3> value) noexcept
{
    const auto lengthSquared = Dot(value, value);
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20f) {
        return {0.0f, 0.0f, 1.0f};
    }
    const auto inverse = 1.0f / std::sqrt(lengthSquared);
    for (auto& component : value) component *= inverse;
    return value;
}

float LookupTransfer(
    const std::array<float, kMaterialTransferSampleCount>& values,
    const float input) noexcept
{
    const auto coordinate = Unit(input) *
        static_cast<float>(kMaterialTransferSampleCount - 1);
    const auto first = static_cast<std::size_t>(std::floor(coordinate));
    const auto second = std::min(
        first + 1, static_cast<std::size_t>(kMaterialTransferSampleCount - 1));
    const auto fraction = coordinate - std::floor(coordinate);
    return values[first] + (values[second] - values[first]) * fraction;
}

bool FiniteSurfaceInput(const MaterialSurfaceInput& input) noexcept
{
    const std::array colors{
        input.baseColor.r, input.baseColor.g, input.baseColor.b,
        input.baseColor.a, input.normal.r, input.normal.g, input.normal.b,
        input.normal.a, input.smoothSpec.r, input.smoothSpec.g,
        input.smoothSpec.b, input.smoothSpec.a};
    return Finite(colors) && Finite(input.vertexColor) &&
        Finite(input.geometricNormal) && Finite(input.tangent) &&
        Finite(input.bitangent);
}

float SmithG1(const float cosine, const float alphaSquared) noexcept
{
    if (cosine <= 0.0f) return 0.0f;
    return 2.0f * cosine /
        (cosine + std::sqrt(alphaSquared +
            (1.0f - alphaSquared) * cosine * cosine));
}

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

ResolvedScalarPacketV1 Pack(const ResolvedScalar& value) noexcept
{
    return {value.raw, value.value,
        static_cast<std::uint32_t>(value.provenance),
        value.clamped ? 1u : 0u};
}

ResolvedVector2PacketV1 Pack(const ResolvedVector2& value) noexcept
{
    ResolvedVector2PacketV1 result{};
    std::copy(value.raw.begin(), value.raw.end(), result.raw);
    std::copy(value.value.begin(), value.value.end(), result.value);
    result.provenance = static_cast<std::uint32_t>(value.provenance);
    result.flags = value.clamped ? 1u : 0u;
    return result;
}

ResolvedVector3PacketV1 Pack(const ResolvedVector3& value) noexcept
{
    ResolvedVector3PacketV1 result{};
    std::copy(value.raw.begin(), value.raw.end(), result.raw);
    std::copy(value.value.begin(), value.value.end(), result.value);
    result.provenance = static_cast<std::uint32_t>(value.provenance);
    result.flags = value.clamped ? 1u : 0u;
    return result;
}

MaterialRecordV1 Pack(const CanonicalMaterial& material) noexcept
{
    MaterialRecordV1 record{};
    record.materialId = material.materialId;
    record.generation = material.generation;
    record.revision = material.revision;
    record.staticRevision = material.staticRevision;
    record.alphaSemantic = static_cast<std::uint32_t>(material.alphaSemantic);
    record.normalEncoding = static_cast<std::uint32_t>(material.normalEncoding);
    for (std::size_t index = 0; index < material.textures.size(); ++index) {
        const auto& source = material.textures[index];
        auto& destination = record.textures[index];
        destination.resourceId = source.resourceId;
        destination.generation = source.generation;
        destination.viewFormat = source.viewFormat;
        destination.role = static_cast<std::uint32_t>(source.role);
        destination.colorSpace = static_cast<std::uint32_t>(source.colorSpace);
        destination.provenance =
            static_cast<std::uint32_t>(source.provenance);
        destination.authored = source.authored ? 1u : 0u;
    }
    record.scalars[0] = Pack(material.alpha);
    record.scalars[1] = Pack(material.alphaCutoff);
    record.scalars[2] = Pack(material.smoothness);
    record.scalars[3] = Pack(material.specularScale);
    record.scalars[4] = Pack(material.fresnelPower);
    record.uvOffset = Pack(material.uvOffset);
    record.uvScale = Pack(material.uvScale);
    record.specularColor = Pack(material.specularColor);
    return record;
}

bool Unpack(
    const ResolvedScalarPacketV1& source,
    const float minimum,
    const float maximum,
    ResolvedScalar& destination) noexcept
{
    const auto provenance = static_cast<MaterialProvenance>(source.provenance);
    if (!IsValidProvenance(provenance) ||
        provenance == MaterialProvenance::Missing || source.flags > 1 ||
        !std::isfinite(source.raw) || !std::isfinite(source.value) ||
        source.value < minimum || source.value > maximum ||
        ((source.flags != 0) != (source.raw != source.value))) {
        return false;
    }
    destination = {source.raw, source.value, provenance, source.flags != 0};
    return true;
}

template <std::size_t Size, class Source, class Destination>
bool UnpackVector(
    const Source& source,
    const float minimum,
    const float maximum,
    Destination& destination) noexcept
{
    const auto provenance = static_cast<MaterialProvenance>(source.provenance);
    if (!IsValidProvenance(provenance) ||
        provenance == MaterialProvenance::Missing || source.flags > 1) {
        return false;
    }
    std::copy_n(source.raw, Size, destination.raw.begin());
    std::copy_n(source.value, Size, destination.value.begin());
    if (!Finite(destination.raw) || !Finite(destination.value) ||
        std::any_of(destination.value.begin(), destination.value.end(),
            [minimum, maximum](const float component) {
                return component < minimum || component > maximum;
            }) ||
        ((source.flags != 0) != (destination.raw != destination.value))) {
        return false;
    }
    destination.provenance = provenance;
    destination.clamped = source.flags != 0;
    return true;
}

bool Unpack(
    const MaterialRecordV1& record,
    CanonicalMaterial& material) noexcept
{
    material = {};
    if (record.materialId == 0 || record.generation == 0 ||
        record.revision == 0 || record.staticRevision == 0 ||
        record.reserved0 != 0) return false;
    const auto alpha = static_cast<MaterialAlphaSemantic>(
        record.alphaSemantic);
    const auto normal = static_cast<MaterialNormalEncoding>(
        record.normalEncoding);
    if (alpha < MaterialAlphaSemantic::Opaque ||
        alpha > MaterialAlphaSemantic::Coverage ||
        normal < MaterialNormalEncoding::TangentSpaceBc5 ||
        normal > MaterialNormalEncoding::ModelSpaceRgb) return false;
    CanonicalMaterial candidate{};
    candidate.materialId = record.materialId;
    candidate.generation = record.generation;
    candidate.revision = record.revision;
    candidate.staticRevision = record.staticRevision;
    candidate.alphaSemantic = alpha;
    candidate.normalEncoding = normal;
    for (std::size_t index = 0; index < candidate.textures.size(); ++index) {
        const auto& source = record.textures[index];
        const auto role = static_cast<MaterialTextureRole>(source.role);
        const auto colorSpace = static_cast<MaterialColorSpace>(
            source.colorSpace);
        const auto provenance = static_cast<MaterialProvenance>(
            source.provenance);
        const auto expectedRole = static_cast<MaterialTextureRole>(index);
        const auto expectedColor = RequiredColorSpace(expectedRole);
        if (source.resourceId == 0 || source.generation == 0 ||
            role != expectedRole || colorSpace != expectedColor ||
            !IsValidProvenance(provenance) ||
            provenance == MaterialProvenance::Missing ||
            source.authored > 1 ||
            (source.authored == 0 && provenance !=
                MaterialProvenance::CanonicalFallback)) return false;
        texture::TextureFormatInfo info{};
        if (texture::ResolveTextureFormat(source.viewFormat,
                source.viewFormat, info) != texture::TexturePacketError::None ||
            (source.authored != 0 && info.srgb !=
                (expectedColor == MaterialColorSpace::Srgb))) return false;
        candidate.textures[index] = {
            role, source.resourceId, source.generation, source.viewFormat,
            colorSpace, provenance, source.authored != 0};
    }
    if (!Unpack(record.scalars[0], 0.0f, 1.0f, candidate.alpha) ||
        !Unpack(record.scalars[1], 0.0f, 1.0f, candidate.alphaCutoff) ||
        !Unpack(record.scalars[2], 0.0f, 1.0f, candidate.smoothness) ||
        !Unpack(record.scalars[3], 0.0f, 4.0f, candidate.specularScale) ||
        !Unpack(record.scalars[4], 1.0f, 16.0f, candidate.fresnelPower) ||
        !UnpackVector<2>(record.uvOffset, -65536.0f, 65536.0f,
            candidate.uvOffset) ||
        !UnpackVector<2>(record.uvScale, -1024.0f, 1024.0f,
            candidate.uvScale) ||
        !UnpackVector<3>(record.specularColor, 0.0f, 1.0f,
            candidate.specularColor)) return false;
    material = candidate;
    return true;
}

bool TextureMatches(
    const ResolvedTexture& binding,
    const texture::CapturedTexture& texture) noexcept
{
    return texture.resourceId == binding.resourceId &&
        texture.generation == binding.generation &&
        texture.viewFormat == binding.viewFormat &&
        texture.dimension == texture::TextureDimension::Texture2D;
}

}

MaterialError TranslateMaterial(
    const MaterialCapture& capture,
    CanonicalMaterial& material) noexcept
{
    material = {};
    if (capture.materialId == 0 || capture.generation == 0) {
        return MaterialError::InvalidIdentity;
    }
    if (capture.revision == 0 || capture.staticRevision == 0) {
        return MaterialError::InvalidRevision;
    }
    if (capture.alphaSemantic < MaterialAlphaSemantic::Opaque ||
        capture.alphaSemantic > MaterialAlphaSemantic::Coverage) {
        return MaterialError::InvalidAlphaSemantic;
    }
    if (capture.normalEncoding < MaterialNormalEncoding::TangentSpaceBc5 ||
        capture.normalEncoding > MaterialNormalEncoding::ModelSpaceRgb) {
        return MaterialError::InvalidNormalEncoding;
    }
    try {
        CanonicalMaterial candidate{};
        candidate.materialId = capture.materialId;
        candidate.generation = capture.generation;
        candidate.revision = capture.revision;
        candidate.staticRevision = capture.staticRevision;
        candidate.alphaSemantic = capture.alphaSemantic;
        candidate.normalEncoding = capture.normalEncoding;
        for (std::size_t index = 0; index < candidate.textures.size(); ++index) {
            const auto result = ResolveTexture(capture,
                static_cast<MaterialTextureRole>(index),
                candidate.textures[index]);
            if (result != MaterialError::None) return result;
        }
        auto result = ResolveScalar(capture.alpha,
            1.0f, 0.0f, 1.0f, candidate.alpha);
        if (result != MaterialError::None) return result;
        result = ResolveScalar(capture.alphaCutoff,
            0.5f, 0.0f, 1.0f, candidate.alphaCutoff);
        if (result != MaterialError::None) return result;
        result = ResolveScalar(capture.smoothness,
            0.5f, 0.0f, 1.0f, candidate.smoothness);
        if (result != MaterialError::None) return result;
        result = ResolveScalar(capture.specularScale,
            1.0f, 0.0f, 4.0f, candidate.specularScale);
        if (result != MaterialError::None) return result;
        result = ResolveScalar(capture.fresnelPower,
            5.0f, 1.0f, 16.0f, candidate.fresnelPower);
        if (result != MaterialError::None) return result;
        result = ResolveVector<2>(capture.uvOffset,
            {0.0f, 0.0f}, -65536.0f, 65536.0f, candidate.uvOffset);
        if (result != MaterialError::None) return result;
        result = ResolveVector<2>(capture.uvScale,
            {1.0f, 1.0f}, -1024.0f, 1024.0f, candidate.uvScale);
        if (result != MaterialError::None) return result;
        result = ResolveVector<3>(capture.specularColor,
            {0.04f, 0.04f, 0.04f}, 0.0f, 1.0f,
            candidate.specularColor);
        if (result != MaterialError::None) return result;
        material = candidate;
        return MaterialError::None;
    } catch (...) {
        material = {};
        return MaterialError::AllocationFailure;
    }
}

std::array<float, 2> TransformMaterialUv(
    const CanonicalMaterial& material,
    const std::array<float, 2> uv) noexcept
{
    return {
        uv[0] * material.uvScale.value[0] + material.uvOffset.value[0],
        uv[1] * material.uvScale.value[1] + material.uvOffset.value[1],
    };
}

SmoothSpecSample DecodeSmoothSpec(
    const texture::SampledColor& sample) noexcept
{
    return {Unit(sample.r), Unit(sample.g)};
}

MaterialTransferLut MakeDefaultMaterialTransferLut() noexcept
{
    MaterialTransferLut result{};
    for (std::size_t index = 0; index < kMaterialTransferSampleCount; ++index) {
        const auto value = static_cast<float>(index) /
            static_cast<float>(kMaterialTransferSampleCount - 1);
        result.smoothnessToPerceptualRoughness[index] =
            std::max(0.045f, 1.0f - value);
        result.specularWeight[index] = value;
    }
    return result;
}

MaterialError ValidateMaterialTransferLut(
    const MaterialTransferLut& lut) noexcept
{
    if (lut.version != kMaterialTransferVersion) {
        return MaterialError::InvalidTransferLut;
    }
    for (std::size_t index = 0; index < kMaterialTransferSampleCount; ++index) {
        const auto roughness = lut.smoothnessToPerceptualRoughness[index];
        const auto specular = lut.specularWeight[index];
        if (!std::isfinite(roughness) || !std::isfinite(specular) ||
            roughness < 0.0f || roughness > 1.0f ||
            specular < 0.0f || specular > 1.0f ||
            (index != 0 && roughness >
                lut.smoothnessToPerceptualRoughness[index - 1]) ||
            (index != 0 && specular < lut.specularWeight[index - 1])) {
            return MaterialError::InvalidTransferLut;
        }
    }
    return MaterialError::None;
}

MaterialError EvaluateMaterialSurface(
    const CanonicalMaterial& material,
    const MaterialSurfaceInput& input,
    const MaterialTransferLut& transfer,
    MaterialSurface& surface) noexcept
{
    surface = {};
    const auto transferResult = ValidateMaterialTransferLut(transfer);
    if (transferResult != MaterialError::None) return transferResult;
    if (!FiniteSurfaceInput(input)) return MaterialError::NonFiniteSource;

    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float sampled[] = {
            input.baseColor.r, input.baseColor.g, input.baseColor.b};
        surface.baseColor[channel] = std::clamp(
            sampled[channel] * input.vertexColor[channel], 0.0f, 16.0f);
    }
    surface.opacity = Unit(input.baseColor.a * material.alpha.value);
    surface.discarded = material.alphaSemantic ==
        MaterialAlphaSemantic::Coverage &&
        surface.opacity < material.alphaCutoff.value;

    const auto& normalTexture = material.textures[
        static_cast<std::size_t>(MaterialTextureRole::Normal)];
    if (!normalTexture.authored) {
        surface.normal = Normalize(input.geometricNormal);
    } else if (material.normalEncoding ==
        MaterialNormalEncoding::TangentSpaceBc5) {
        const auto x = Unit(input.normal.r) * 2.0f - 1.0f;
        const auto y = Unit(input.normal.g) * 2.0f - 1.0f;
        const auto z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
        surface.normal = Normalize({
            input.tangent[0] * x + input.bitangent[0] * y +
                input.geometricNormal[0] * z,
            input.tangent[1] * x + input.bitangent[1] * y +
                input.geometricNormal[1] * z,
            input.tangent[2] * x + input.bitangent[2] * y +
                input.geometricNormal[2] * z,
        });
    } else {
        surface.normal = Normalize({
            Unit(input.normal.r) * 2.0f - 1.0f,
            Unit(input.normal.g) * 2.0f - 1.0f,
            Unit(input.normal.b) * 2.0f - 1.0f,
        });
    }

    const auto smoothSpec = DecodeSmoothSpec(input.smoothSpec);
    surface.smoothness = Unit(
        material.smoothness.value * smoothSpec.smoothnessWeight);
    surface.perceptualRoughness = LookupTransfer(
        transfer.smoothnessToPerceptualRoughness, surface.smoothness);
    surface.alphaRoughness = std::max(
        0.002025f,
        surface.perceptualRoughness * surface.perceptualRoughness);
    const auto specularWeight = LookupTransfer(
        transfer.specularWeight, smoothSpec.specularWeight);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        surface.specularF0[channel] = std::clamp(
            material.specularColor.value[channel] *
                material.specularScale.value * specularWeight,
            0.0f, 0.99f);
    }
    surface.fresnelPower = material.fresnelPower.value;
    return MaterialError::None;
}

GgxLighting EvaluateGgxDirect(
    const MaterialSurface& surface,
    std::array<float, 3> viewDirection,
    std::array<float, 3> lightDirection,
    const std::array<float, 3> radiance) noexcept
{
    GgxLighting result{};
    if (surface.discarded || !Finite(viewDirection) ||
        !Finite(lightDirection) || !Finite(radiance)) return result;
    const auto normal = Normalize(surface.normal);
    viewDirection = Normalize(viewDirection);
    lightDirection = Normalize(lightDirection);
    const auto halfVector = Normalize({
        viewDirection[0] + lightDirection[0],
        viewDirection[1] + lightDirection[1],
        viewDirection[2] + lightDirection[2]});
    const auto nDotV = std::max(0.0f, Dot(normal, viewDirection));
    const auto nDotL = std::max(0.0f, Dot(normal, lightDirection));
    const auto nDotH = std::max(0.0f, Dot(normal, halfVector));
    const auto vDotH = std::max(0.0f, Dot(viewDirection, halfVector));
    if (nDotV <= 0.0f || nDotL <= 0.0f) return result;

    const auto alphaSquared = surface.alphaRoughness *
        surface.alphaRoughness;
    const auto denominator = nDotH * nDotH *
        (alphaSquared - 1.0f) + 1.0f;
    const auto distribution = alphaSquared /
        std::max(kPi * denominator * denominator, 1.0e-8f);
    const auto geometry = SmithG1(nDotV, alphaSquared) *
        SmithG1(nDotL, alphaSquared);
    const auto fresnelAmount = std::pow(
        std::max(0.0f, 1.0f - vDotH), surface.fresnelPower);
    const auto brdfDenominator = std::max(
        4.0f * nDotV * nDotL, 1.0e-6f);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto fresnel = surface.specularF0[channel] +
            (1.0f - surface.specularF0[channel]) * fresnelAmount;
        const auto specularBrdf = distribution * geometry * fresnel /
            brdfDenominator;
        const auto diffuseBrdf = surface.baseColor[channel] *
            (1.0f - fresnel) / kPi;
        result.specular[channel] = specularBrdf * radiance[channel] * nDotL;
        result.diffuse[channel] = diffuseBrdf * radiance[channel] * nDotL;
        result.combined[channel] =
            result.diffuse[channel] + result.specular[channel];
    }
    return result;
}

bool BindlessTextureTable::Key::operator<(const Key& other) const noexcept
{
    return std::tie(resourceId, generation) <
        std::tie(other.resourceId, other.generation);
}

DescriptorError BindlessTextureTable::Register(
    const std::uint64_t resourceId,
    const std::uint32_t generation,
    const std::uint32_t descriptorIndex)
{
    if (resourceId == 0 || generation == 0) {
        return DescriptorError::InvalidResource;
    }
    const Key key{resourceId, generation};
    const auto found = descriptors_.find(key);
    if (found != descriptors_.end()) {
        return found->second == descriptorIndex
            ? DescriptorError::None : DescriptorError::DuplicateResource;
    }
    descriptors_.emplace(key, descriptorIndex);
    return DescriptorError::None;
}

std::optional<std::uint32_t> BindlessTextureTable::Resolve(
    const std::uint64_t resourceId,
    const std::uint32_t generation) const noexcept
{
    const auto found = descriptors_.find(Key{resourceId, generation});
    return found == descriptors_.end()
        ? std::nullopt : std::optional{found->second};
}

MaterialError BuildMaterialGpuRecords(
    const CanonicalMaterial& material,
    const BindlessTextureTable& textures,
    const std::uint32_t transferVersion,
    MaterialGpuRecords& records) noexcept
{
    records = {};
    if (material.materialId == 0 || material.generation == 0) {
        return MaterialError::InvalidIdentity;
    }
    if (material.revision == 0 || material.staticRevision == 0) {
        return MaterialError::InvalidRevision;
    }
    if (transferVersion != kMaterialTransferVersion) {
        return MaterialError::InvalidTransferLut;
    }
    records.staticRecord.materialId = material.materialId;
    records.staticRecord.staticRevision = material.staticRevision;
    for (std::size_t index = 0; index < material.textures.size(); ++index) {
        const auto descriptor = textures.Resolve(
            material.textures[index].resourceId,
            material.textures[index].generation);
        if (!descriptor) {
            records = {};
            return MaterialError::MissingDescriptor;
        }
        records.staticRecord.textureIndices[index] = *descriptor;
    }
    if (material.textures[0].authored) {
        records.staticRecord.flags |= kGpuMaterialHasBaseColor;
    }
    if (material.textures[1].authored) {
        records.staticRecord.flags |= kGpuMaterialHasNormal;
    }
    if (material.textures[2].authored) {
        records.staticRecord.flags |= kGpuMaterialHasSmoothSpec;
    }
    if (material.normalEncoding == MaterialNormalEncoding::ModelSpaceRgb) {
        records.staticRecord.flags |= kGpuMaterialModelSpaceNormal;
    }
    if (material.alphaSemantic == MaterialAlphaSemantic::Coverage) {
        records.staticRecord.flags |= kGpuMaterialAlphaCoverage;
    }
    std::copy(material.uvScale.value.begin(), material.uvScale.value.end(),
        records.staticRecord.uvScale);
    std::copy(material.uvOffset.value.begin(), material.uvOffset.value.end(),
        records.staticRecord.uvOffset);
    std::copy(material.specularColor.value.begin(),
        material.specularColor.value.end(),
        records.staticRecord.specularColor);

    records.dynamicRecord.materialId = material.materialId;
    records.dynamicRecord.materialRevision = material.revision;
    records.dynamicRecord.staticRevision = material.staticRevision;
    records.dynamicRecord.alpha = material.alpha.value;
    records.dynamicRecord.smoothness = material.smoothness.value;
    records.dynamicRecord.specularScale = material.specularScale.value;
    records.dynamicRecord.fresnelPower = material.fresnelPower.value;
    records.dynamicRecord.alphaCutoff = material.alphaCutoff.value;
    records.dynamicRecord.transferVersion = transferVersion;
    return MaterialError::None;
}

MaterialUpdatePlan MaterialRevisionTracker::PlanAndCommit(
    const MaterialGpuRecords& records) noexcept
{
    MaterialUpdatePlan plan{};
    if (!currentValid_) {
        plan = {true, true, true};
    } else {
        plan.writeStatic = !(current_.staticRecord == records.staticRecord);
        plan.writeDynamic = !(current_.dynamicRecord == records.dynamicRecord);
        plan.writeDescriptors = !std::equal(
            std::begin(current_.staticRecord.textureIndices),
            std::end(current_.staticRecord.textureIndices),
            std::begin(records.staticRecord.textureIndices));
    }
    current_ = records;
    currentValid_ = true;
    return plan;
}

MaterialPacketError EncodeMaterialReplayBundle(
    const MaterialReplayBundle& bundle,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    if (bundle.transferVersion != kMaterialTransferVersion) {
        return MaterialPacketError::UnsupportedTransferVersion;
    }
    const auto record = Pack(bundle.material);
    CanonicalMaterial verified{};
    if (!Unpack(record, verified)) {
        return MaterialPacketError::InvalidMaterial;
    }
    try {
        std::array<std::vector<std::byte>, 3> textureBytes;
        for (std::size_t index = 0; index < textureBytes.size(); ++index) {
            if (!TextureMatches(bundle.material.textures[index],
                    bundle.textures[index])) {
                return MaterialPacketError::TextureMismatch;
            }
            if (texture::EncodeCapturedTexture(
                    bundle.textures[index], textureBytes[index]) !=
                texture::TexturePacketError::None) {
                return MaterialPacketError::TexturePacketFailed;
            }
        }
        MaterialPacketHeaderV1 header{};
        header.recordOffset = sizeof(header);
        header.recordSize = sizeof(record);
        header.transferVersion = bundle.transferVersion;
        bytes.resize(sizeof(header));
        const auto* recordBegin = reinterpret_cast<const std::byte*>(&record);
        bytes.insert(bytes.end(), recordBegin, recordBegin + sizeof(record));
        for (std::size_t index = 0; index < textureBytes.size(); ++index) {
            const auto offset = AlignUp(bytes.size(), 8);
            if (offset == std::numeric_limits<std::size_t>::max() ||
                offset > std::numeric_limits<std::uint32_t>::max() ||
                textureBytes[index].size() >
                    std::numeric_limits<std::uint32_t>::max() ||
                textureBytes[index].size() >
                    std::numeric_limits<std::uint32_t>::max() - offset) {
                bytes.clear();
                return MaterialPacketError::AllocationFailure;
            }
            bytes.resize(offset, std::byte{0});
            header.textureOffsets[index] =
                static_cast<std::uint32_t>(offset);
            header.textureSizes[index] =
                static_cast<std::uint32_t>(textureBytes[index].size());
            bytes.insert(bytes.end(), textureBytes[index].begin(),
                textureBytes[index].end());
        }
        if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            bytes.clear();
            return MaterialPacketError::AllocationFailure;
        }
        header.totalSize = static_cast<std::uint32_t>(bytes.size());
        header.payloadCrc32 = vf::renderer::trace::Crc32(
            std::span<const std::byte>{bytes}.subspan(header.recordOffset));
        std::memcpy(bytes.data(), &header, sizeof(header));
        return MaterialPacketError::None;
    } catch (...) {
        bytes.clear();
        return MaterialPacketError::AllocationFailure;
    }
}

MaterialPacketError DecodeMaterialReplayBundle(
    const std::span<const std::byte> bytes,
    MaterialReplayBundle& bundle) noexcept
{
    bundle = {};
    if (bytes.size() < sizeof(MaterialPacketHeaderV1)) {
        return MaterialPacketError::TruncatedHeader;
    }
    MaterialPacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kMaterialPacketMagic) {
        return MaterialPacketError::BadMagic;
    }
    if (header.versionMajor != kMaterialPacketVersionMajor ||
        header.versionMinor > kMaterialPacketVersionMinor) {
        return MaterialPacketError::UnsupportedVersion;
    }
    if (header.headerSize != sizeof(header) ||
        header.recordOffset != sizeof(header) ||
        header.recordSize != sizeof(MaterialRecordV1)) {
        return MaterialPacketError::SectionOutOfBounds;
    }
    if (header.totalSize != bytes.size()) {
        return MaterialPacketError::SizeMismatch;
    }
    if (header.transferVersion != kMaterialTransferVersion) {
        return MaterialPacketError::UnsupportedTransferVersion;
    }
    if (header.recordOffset > bytes.size() ||
        header.recordSize > bytes.size() - header.recordOffset) {
        return MaterialPacketError::InvalidMaterial;
    }
    std::size_t cursor = header.recordOffset + header.recordSize;
    for (std::size_t index = 0; index < 3; ++index) {
        const auto expectedOffset = AlignUp(cursor, 8);
        if (expectedOffset == std::numeric_limits<std::size_t>::max() ||
            header.textureOffsets[index] != expectedOffset) {
            return header.textureOffsets[index] % 8 != 0
                ? MaterialPacketError::MisalignedSection
                : MaterialPacketError::SectionOutOfBounds;
        }
        if (!std::all_of(bytes.begin() + cursor,
                bytes.begin() + expectedOffset,
                [](const std::byte value) { return value == std::byte{0}; })) {
            return MaterialPacketError::SectionOutOfBounds;
        }
        const auto offset = static_cast<std::size_t>(
            header.textureOffsets[index]);
        const auto size = static_cast<std::size_t>(header.textureSizes[index]);
        if (size == 0 || offset > bytes.size() ||
            size > bytes.size() - offset) {
            return MaterialPacketError::SectionOutOfBounds;
        }
        cursor = offset + size;
    }
    if (cursor != bytes.size()) {
        return MaterialPacketError::SectionOutOfBounds;
    }
    if (vf::renderer::trace::Crc32(bytes.subspan(header.recordOffset)) !=
        header.payloadCrc32) {
        return MaterialPacketError::ChecksumMismatch;
    }
    MaterialRecordV1 record{};
    std::memcpy(&record, bytes.data() + header.recordOffset, sizeof(record));
    CanonicalMaterial material{};
    if (!Unpack(record, material)) {
        return MaterialPacketError::InvalidMaterial;
    }
    try {
        MaterialReplayBundle candidate{};
        candidate.material = material;
        candidate.transferVersion = header.transferVersion;
        for (std::size_t index = 0; index < candidate.textures.size(); ++index) {
            const auto textureBytes = bytes.subspan(
                header.textureOffsets[index], header.textureSizes[index]);
            if (texture::DecodeCapturedTexture(
                    textureBytes, candidate.textures[index]) !=
                texture::TexturePacketError::None) {
                return MaterialPacketError::TexturePacketFailed;
            }
            if (!TextureMatches(candidate.material.textures[index],
                    candidate.textures[index])) {
                return MaterialPacketError::TextureMismatch;
            }
        }
        bundle = std::move(candidate);
        return MaterialPacketError::None;
    } catch (...) {
        bundle = {};
        return MaterialPacketError::AllocationFailure;
    }
}

const char* ToString(const MaterialPacketError error) noexcept
{
    switch (error) {
    case MaterialPacketError::None: return "none";
    case MaterialPacketError::TruncatedHeader: return "truncated-header";
    case MaterialPacketError::BadMagic: return "bad-magic";
    case MaterialPacketError::UnsupportedVersion: return "unsupported-version";
    case MaterialPacketError::SizeMismatch: return "size-mismatch";
    case MaterialPacketError::MisalignedSection: return "misaligned-section";
    case MaterialPacketError::SectionOutOfBounds: return "section-out-of-bounds";
    case MaterialPacketError::ChecksumMismatch: return "checksum-mismatch";
    case MaterialPacketError::InvalidMaterial: return "invalid-material";
    case MaterialPacketError::TexturePacketFailed: return "texture-packet-failed";
    case MaterialPacketError::TextureMismatch: return "texture-mismatch";
    case MaterialPacketError::AllocationFailure: return "allocation-failure";
    case MaterialPacketError::UnsupportedTransferVersion:
        return "unsupported-transfer-version";
    }
    return "unknown";
}

const char* ToString(const MaterialError error) noexcept
{
    switch (error) {
    case MaterialError::None: return "none";
    case MaterialError::InvalidIdentity: return "invalid-identity";
    case MaterialError::InvalidRevision: return "invalid-revision";
    case MaterialError::InvalidProvenance: return "invalid-provenance";
    case MaterialError::AmbiguousSource: return "ambiguous-source";
    case MaterialError::NonFiniteSource: return "non-finite-source";
    case MaterialError::InvalidTexture: return "invalid-texture";
    case MaterialError::InvalidTextureRole: return "invalid-texture-role";
    case MaterialError::InvalidColorSpace: return "invalid-color-space";
    case MaterialError::InvalidAlphaSemantic: return "invalid-alpha-semantic";
    case MaterialError::InvalidNormalEncoding: return "invalid-normal-encoding";
    case MaterialError::InvalidTransferLut: return "invalid-transfer-lut";
    case MaterialError::MissingDescriptor: return "missing-descriptor";
    case MaterialError::DuplicateDescriptor: return "duplicate-descriptor";
    case MaterialError::AllocationFailure: return "allocation-failure";
    }
    return "unknown";
}

}
