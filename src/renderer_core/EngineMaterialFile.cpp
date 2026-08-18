#include "renderer_core/EngineMaterialFile.h"

#include <cstring>

namespace vf::renderer::materialfile {
namespace {

// A cursor that refuses rather than reads past the end, so every field below
// can be written as a straight read and the first overrun stops the parse.
class Cursor
{
public:
    explicit Cursor(const std::span<const std::byte> bytes) noexcept
        : bytes_{bytes}
    {
    }

    [[nodiscard]] bool Ok() const noexcept { return ok_; }

    [[nodiscard]] std::size_t Position() const noexcept { return position_; }

    [[nodiscard]] std::uint8_t U8() noexcept
    {
        if (!Require(1)) return 0;
        return static_cast<std::uint8_t>(bytes_[position_++]);
    }

    [[nodiscard]] bool Bool() noexcept { return U8() != 0; }

    [[nodiscard]] std::uint32_t U32() noexcept
    {
        if (!Require(4)) return 0;
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            value |= static_cast<std::uint32_t>(bytes_[position_ + index]) <<
                (index * 8);
        }
        position_ += 4;
        return value;
    }

    [[nodiscard]] float F32() noexcept
    {
        const auto bits = U32();
        // Bit pattern, not a numeric conversion: these are IEEE floats on
        // disk and reinterpreting is the whole point.
        float value = 0.0f;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // Length-prefixed and NUL-terminated: the stored length counts the
    // terminator, so a one-byte string is the empty one.
    [[nodiscard]] std::string String() noexcept
    {
        const auto length = U32();
        if (!ok_) return {};
        if (!Require(length)) {
            failure_ = MaterialFileError::TruncatedString;
            return {};
        }
        std::string value;
        try {
            value.reserve(length);
            for (std::uint32_t index = 0; index < length; ++index) {
                const auto character =
                    static_cast<char>(bytes_[position_ + index]);
                if (character == '\0') break;
                value.push_back(character);
            }
        } catch (...) {
            ok_ = false;
            failure_ = MaterialFileError::Truncated;
            return {};
        }
        position_ += length;
        return value;
    }

    void Skip(const std::size_t count) noexcept
    {
        if (Require(count)) position_ += count;
    }

    [[nodiscard]] MaterialFileError Failure() const noexcept
    {
        return failure_;
    }

private:
    [[nodiscard]] bool Require(const std::size_t count) noexcept
    {
        if (!ok_) return false;
        if (bytes_.size() - position_ < count) {
            ok_ = false;
            failure_ = MaterialFileError::Truncated;
            return false;
        }
        return true;
    }

    std::span<const std::byte> bytes_;
    std::size_t position_{};
    bool ok_{true};
    MaterialFileError failure_{MaterialFileError::None};
};

void ParseShaderTail(Cursor& cursor, MaterialFile& material)
{
    for (auto& texture : material.textures) {
        texture = cursor.String();
    }
    static_cast<void>(cursor.U8());  // enableEditorAlphaRef
    material.rimLighting = cursor.Bool();
    static_cast<void>(cursor.F32());  // rimPower
    static_cast<void>(cursor.F32());  // backlightPower
    material.subsurfaceLighting = cursor.Bool();
    static_cast<void>(cursor.F32());  // subsurfaceLightingRolloff
    material.specularEnabled = cursor.Bool();
    cursor.Skip(12);                  // specularColor
    static_cast<void>(cursor.F32());  // specularMult
    material.smoothness = cursor.F32();
    static_cast<void>(cursor.F32());  // fresnelPower
    cursor.Skip(24);                  // six wetness control scalars
    static_cast<void>(cursor.String());  // rootMaterialPath
    material.anisotropicLighting = cursor.Bool();
    material.emitEnabled = cursor.Bool();
    // The emissive colour is present only when emission is declared, which is
    // the one place this format is not a flat record.
    if (material.emitEnabled) cursor.Skip(12);
    static_cast<void>(cursor.F32());  // emittanceMult
    material.modelSpaceNormals = cursor.Bool();
    material.externalEmittance = cursor.Bool();
    material.backLighting = cursor.Bool();
    static_cast<void>(cursor.U8());  // receiveShadows
    static_cast<void>(cursor.U8());  // hideSecret
    static_cast<void>(cursor.U8());  // castShadows
    static_cast<void>(cursor.U8());  // dissolveFade
    static_cast<void>(cursor.U8());  // assumeShadowmask
    material.glowmap = cursor.Bool();
    material.environmentMappingWindow = cursor.Bool();
    material.environmentMappingEye = cursor.Bool();
    material.hair = cursor.Bool();
    cursor.Skip(12);  // hairTintColor
    material.tree = cursor.Bool();
    material.faceGen = cursor.Bool();
    material.skinTint = cursor.Bool();
    material.tessellate = cursor.Bool();
    if (material.version < 3) {
        // Displacement and tessellation scalars, dropped from the format at
        // version 3 rather than merely unused.
        cursor.Skip(20);
    }
    static_cast<void>(cursor.F32());  // grayscaleToPaletteScale
    if (material.version >= 1) {
        static_cast<void>(cursor.U8());  // skewSpecularAlpha
    }
}

void ParseEffectTail(Cursor& cursor, MaterialFile& material)
{
    for (auto& texture : material.effectTextures) {
        texture = cursor.String();
    }
    material.bloodEnabled = cursor.Bool();
    material.effectLightingEnabled = cursor.Bool();
    material.falloffEnabled = cursor.Bool();
    material.falloffColorEnabled = cursor.Bool();
    material.grayscaleToPaletteAlpha = cursor.Bool();
    material.softEnabled = cursor.Bool();
    cursor.Skip(12);                  // baseColor
    static_cast<void>(cursor.F32());  // baseColorScale
    cursor.Skip(16);                  // four falloff scalars
    static_cast<void>(cursor.F32());  // lightingInfluence
    static_cast<void>(cursor.U8());   // envmapMinLOD
    static_cast<void>(cursor.F32());  // softDepth
}

}  // namespace

MaterialFileError ParseMaterialFile(
    const std::span<const std::byte> bytes,
    MaterialFile& material) noexcept
{
    material = {};
    if (bytes.size() < 8) return MaterialFileError::TruncatedHeader;
    const auto magic = [&bytes](const char (&tag)[5]) {
        for (std::size_t index = 0; index < 4; ++index) {
            if (static_cast<char>(bytes[index]) != tag[index]) return false;
        }
        return true;
    };
    if (magic("BGSM")) {
        material.kind = MaterialFileKind::Shader;
    } else if (magic("BGEM")) {
        material.kind = MaterialFileKind::Effect;
    } else {
        return MaterialFileError::BadMagic;
    }

    Cursor cursor{bytes};
    cursor.Skip(4);
    material.version = cursor.U32();
    // Version 2 is what Fallout 4 ships. Later versions moved and added
    // fields, and reading one with this layout would consume the file to a
    // different place -- which the trailing-byte check would catch, but the
    // version is the honest reason to refuse.
    if (material.version > 2) return MaterialFileError::UnsupportedVersion;

    static_cast<void>(cursor.U32());  // tileFlags
    cursor.Skip(16);                  // uOffset, vOffset, uScale, vScale
    material.alpha = cursor.F32();
    material.alphaBlend = cursor.Bool();
    static_cast<void>(cursor.U32());  // alphaSrc
    static_cast<void>(cursor.U32());  // alphaDst
    material.alphaTestReference = cursor.U8();
    material.alphaTest = cursor.Bool();
    material.zBufferWrite = cursor.Bool();
    material.zBufferTest = cursor.Bool();
    material.screenSpaceReflections = cursor.Bool();
    static_cast<void>(cursor.U8());  // wetnessControlScreenSpaceReflections
    material.decal = cursor.Bool();
    material.twoSided = cursor.Bool();
    static_cast<void>(cursor.U8());  // decalNoFade
    material.nonOccluder = cursor.Bool();
    material.refraction = cursor.Bool();
    static_cast<void>(cursor.U8());  // refractionFalloff
    static_cast<void>(cursor.F32());  // refractionPower
    material.environmentMapping = cursor.Bool();
    material.environmentMappingMaskScale = cursor.F32();
    material.grayscaleToPaletteColor = cursor.Bool();

    if (material.kind == MaterialFileKind::Shader) {
        ParseShaderTail(cursor, material);
    } else {
        ParseEffectTail(cursor, material);
    }

    if (!cursor.Ok()) return cursor.Failure();
    // The layout has to land exactly on the end. A field order that is wrong
    // but self-consistent reads every field without complaint and stops
    // somewhere else, and this is what separates the two.
    if (cursor.Position() != bytes.size()) {
        return MaterialFileError::TrailingBytes;
    }
    return MaterialFileError::None;
}

material::MaterialFamily ClassifyMaterialFile(
    const MaterialFile& material) noexcept
{
    // Most specific first. A hair material also declares environment mapping
    // and an eye material also declares face generation, so an order that put
    // the general cases first would report almost everything as one family.
    if (material.kind == MaterialFileKind::Effect) {
        // An effect material is not a lighting shader at all. It is reported
        // as None rather than folded into Default, which would claim the
        // lighting path handles it.
        return material::MaterialFamily::None;
    }
    if (material.environmentMappingEye) return material::MaterialFamily::Eye;
    if (material.hair) return material::MaterialFamily::HairTint;
    if (material.skinTint) return material::MaterialFamily::SkinTint;
    if (material.faceGen) return material::MaterialFamily::Face;
    if (material.tree) return material::MaterialFamily::TreeAnimation;
    if (material.glowmap) return material::MaterialFamily::GlowMap;
    if (material.environmentMapping) {
        return material::MaterialFamily::EnvironmentMap;
    }
    return material::MaterialFamily::Default;
}

const char* ToString(const MaterialFileError error) noexcept
{
    switch (error) {
    case MaterialFileError::None: return "none";
    case MaterialFileError::TruncatedHeader: return "truncated header";
    case MaterialFileError::BadMagic: return "bad magic";
    case MaterialFileError::UnsupportedVersion: return "unsupported version";
    case MaterialFileError::TruncatedString: return "truncated string";
    case MaterialFileError::Truncated: return "truncated";
    case MaterialFileError::TrailingBytes: return "trailing bytes";
    }
    return "unknown";
}

}  // namespace vf::renderer::materialfile
