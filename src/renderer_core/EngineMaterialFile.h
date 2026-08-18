#pragma once

#include "renderer_core/EngineMaterialFamily.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace vf::renderer::materialfile {

// Bethesda's authored material files: BGSM for a lighting shader and BGEM for
// an effect shader. Both begin with the same header and then diverge.
//
// The layouts here were confirmed against the installed corpus rather than
// assumed: a wrong field order still parses, it just ends in the wrong place,
// so the parser requires the file to be consumed *exactly* and the sweep
// reports how many did. On `Fallout4 - Materials.ba2` that is the difference
// between a frequency table and a table of coincidences.
enum class MaterialFileError : std::uint8_t
{
    None,
    TruncatedHeader,
    // Neither BGSM nor BGEM.
    BadMagic,
    UnsupportedVersion,
    // A length-prefixed string running past the end of the file.
    TruncatedString,
    // A field running past the end of the file.
    Truncated,
    // The layout was consumed and bytes remained, which means the layout does
    // not describe this file even though every field in it read cleanly.
    TrailingBytes,
};

enum class MaterialFileKind : std::uint8_t
{
    Shader,
    Effect,
};

struct MaterialFile
{
    MaterialFileKind kind{MaterialFileKind::Shader};
    std::uint32_t version{};

    // Shared header.
    float alpha{1.0f};
    std::uint8_t alphaTestReference{};
    bool alphaBlend{};
    bool alphaTest{};
    bool zBufferWrite{};
    bool zBufferTest{};
    bool screenSpaceReflections{};
    bool decal{};
    bool twoSided{};
    bool nonOccluder{};
    bool refraction{};
    bool environmentMapping{};
    float environmentMappingMaskScale{};
    bool grayscaleToPaletteColor{};

    // BGSM. Nine texture slots in the order the format writes them: diffuse,
    // normal, smoothness/specular, greyscale, environment, glow, inner layer,
    // wrinkles, displacement.
    std::array<std::string, 9> textures;
    bool specularEnabled{};
    bool rimLighting{};
    bool subsurfaceLighting{};
    bool anisotropicLighting{};
    bool emitEnabled{};
    bool modelSpaceNormals{};
    bool externalEmittance{};
    bool backLighting{};
    bool glowmap{};
    bool environmentMappingWindow{};
    bool environmentMappingEye{};
    bool hair{};
    bool tree{};
    bool faceGen{};
    bool skinTint{};
    bool tessellate{};
    float smoothness{};

    // BGEM. Five texture slots: base, greyscale, environment, normal,
    // environment mask.
    std::array<std::string, 5> effectTextures;
    bool bloodEnabled{};
    bool effectLightingEnabled{};
    bool falloffEnabled{};
    bool falloffColorEnabled{};
    bool grayscaleToPaletteAlpha{};
    bool softEnabled{};
};

[[nodiscard]] MaterialFileError ParseMaterialFile(
    std::span<const std::byte> bytes,
    MaterialFile& material) noexcept;

// Which shader family the engine selects for a parsed material.
//
// A material declares several of these at once -- a hair material is also
// environment mapped, an eye material is also a face material -- and the
// engine picks one shader. The order below is the rule this returns, stated
// rather than implied, so a frequency table built from it can be read against
// a different reading of the same corpus.
[[nodiscard]] material::MaterialFamily ClassifyMaterialFile(
    const MaterialFile& material) noexcept;

[[nodiscard]] const char* ToString(MaterialFileError error) noexcept;

}  // namespace vf::renderer::materialfile
