#include "renderer_core/EngineVisibility.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using namespace vf::renderer;

visibility::AlphaPropertyCapture BuildCapture(
    const bool blend,
    const bool test,
    const std::uint8_t reference)
{
    visibility::AlphaPropertyCapture capture{};
    capture.blendEnabled = blend;
    capture.testEnabled = test;
    capture.testReference = reference;
    capture.sourceBlend = 6;      // observed engine value, opaque to us
    capture.destinationBlend = 7;
    capture.fade = 1.0f;
    return capture;
}

visibility::AlphaStateV1 BuildTestedState(const float reference)
{
    visibility::AlphaStateV1 state{};
    state.source = visibility::AlphaSource::TextureAndConstant;
    state.classification = visibility::AlphaClass::Tested;
    state.reference = reference;
    state.constantAlpha = 1.0f;
    state.fade = 1.0f;
    return state;
}

}

TEST_CASE("P15_alpha_classification_is_observed_and_fails_closed",
    "[phase15][visibility]")
{
    visibility::AlphaStateV1 state{};

    // Neither bit set is genuinely opaque; the alpha channel is not consulted.
    REQUIRE(visibility::ClassifyAlphaState(BuildCapture(false, false, 128),
        visibility::AlphaSource::TextureAndConstant, state) ==
        visibility::VisibilityError::None);
    CHECK(state.classification == visibility::AlphaClass::Opaque);
    CHECK(state.source == visibility::AlphaSource::None);

    REQUIRE(visibility::ClassifyAlphaState(BuildCapture(false, true, 128),
        visibility::AlphaSource::TextureAndConstant, state) ==
        visibility::VisibilityError::None);
    CHECK(state.classification == visibility::AlphaClass::Tested);
    CHECK(state.source == visibility::AlphaSource::TextureAndConstant);
    // The engine stores the reference as a byte; 128/255 is the exact value,
    // not a rounded 0.5.
    CHECK(state.reference == Catch::Approx(128.0f / 255.0f));

    REQUIRE(visibility::ClassifyAlphaState(BuildCapture(true, false, 0),
        visibility::AlphaSource::BaseColorTexture, state) ==
        visibility::VisibilityError::None);
    CHECK(state.classification == visibility::AlphaClass::Blended);

    // Blend plus test is still sorted transparency, which this phase does not
    // render. It is classified, not silently demoted to a cutout.
    REQUIRE(visibility::ClassifyAlphaState(BuildCapture(true, true, 64),
        visibility::AlphaSource::BaseColorTexture, state) ==
        visibility::VisibilityError::None);
    CHECK(state.classification == visibility::AlphaClass::Blended);
    CHECK(state.reference == Catch::Approx(64.0f / 255.0f));

    // Alpha-to-coverage without a test is a combination we have not observed;
    // it is refused rather than guessed at.
    auto coverageWithoutTest = BuildCapture(false, false, 0);
    coverageWithoutTest.alphaToCoverage = true;
    CHECK(visibility::ClassifyAlphaState(coverageWithoutTest,
        visibility::AlphaSource::BaseColorTexture, state) ==
        visibility::VisibilityError::UnclassifiedAlpha);

    // A tested surface that consults no alpha source can never discard, so
    // the combination is a capture defect rather than an opaque surface.
    CHECK(visibility::ClassifyAlphaState(BuildCapture(false, true, 128),
        visibility::AlphaSource::None, state) ==
        visibility::VisibilityError::UnclassifiedAlpha);

    auto badFade = BuildCapture(false, true, 128);
    badFade.ditherFade = true;
    badFade.fade = 1.5f;
    CHECK(visibility::ClassifyAlphaState(badFade,
        visibility::AlphaSource::BaseColorTexture, state) ==
        visibility::VisibilityError::InvalidFade);
}

TEST_CASE("P15_alpha_test_uses_the_engine_comparison_at_the_boundary",
    "[phase15][visibility]")
{
    const auto state = BuildTestedState(128.0f / 255.0f);
    visibility::CoverageContext context{};

    // The engine keeps a fragment whose alpha is greater than or equal to the
    // reference, so the boundary sample survives.
    CHECK(visibility::EvaluateCoverage(state, 128.0f / 255.0f, context)
        .covered);
    CHECK_FALSE(visibility::EvaluateCoverage(state, 127.0f / 255.0f, context)
        .covered);
    CHECK(visibility::EvaluateCoverage(state, 129.0f / 255.0f, context)
        .covered);
    CHECK(visibility::EvaluateCoverage(state, 1.0f, context).coverage ==
        Catch::Approx(1.0f));
    CHECK(visibility::EvaluateCoverage(state, 0.0f, context).coverage ==
        Catch::Approx(0.0f));

    // The material's constant alpha scales the sampled alpha before the test,
    // so a half-transparent material raises the sample needed to survive.
    auto scaled = BuildTestedState(0.4f);
    scaled.constantAlpha = 0.5f;
    CHECK(visibility::EvaluateCoverage(scaled, 0.9f, context).covered);
    CHECK_FALSE(visibility::EvaluateCoverage(scaled, 0.7f, context).covered);
    CHECK(visibility::EvaluateCoverage(scaled, 0.8f, context).covered);

    visibility::AlphaStateV1 opaque{};
    opaque.classification = visibility::AlphaClass::Opaque;
    opaque.source = visibility::AlphaSource::None;
    opaque.constantAlpha = 1.0f;
    opaque.fade = 1.0f;
    // An opaque surface never consults alpha, even a zero one.
    CHECK(visibility::EvaluateCoverage(opaque, 0.0f, context).covered);
    CHECK(visibility::EvaluateCoverage(opaque, 0.0f, context).coverage ==
        Catch::Approx(1.0f));
}

TEST_CASE("P15_depth_and_color_passes_resolve_identical_coverage",
    "[phase15][visibility]")
{
    const auto state = BuildTestedState(0.5f);
    visibility::CoverageContext color{};
    visibility::CoverageContext depth{};
    depth.depthOnly = true;
    // A cutout silhouette that differed between the depth prepass and the
    // color pass would punch holes in the G-buffer.
    for (std::uint32_t step = 0; step <= 255; ++step) {
        const auto alpha = static_cast<float>(step) / 255.0f;
        const auto colorResult = visibility::EvaluateCoverage(
            state, alpha, color);
        const auto depthResult = visibility::EvaluateCoverage(
            state, alpha, depth);
        CHECK(colorResult.covered == depthResult.covered);
        CHECK(colorResult.coverage == Catch::Approx(depthResult.coverage));
    }
}

TEST_CASE("P15_dither_fade_is_deterministic_and_monotonic",
    "[phase15][visibility]")
{
    auto state = BuildTestedState(0.0f);
    state.flags = visibility::DitherFade;

    // A fully faded-in surface keeps every pixel; a fully faded-out one keeps
    // none, at every pixel of the dither pattern.
    for (std::uint32_t y = 0; y < visibility::kDitherExtent; ++y) {
        for (std::uint32_t x = 0; x < visibility::kDitherExtent; ++x) {
            visibility::CoverageContext context{};
            context.pixelX = x;
            context.pixelY = y;
            state.fade = 1.0f;
            CHECK(visibility::EvaluateCoverage(state, 1.0f, context).covered);
            state.fade = 0.0f;
            CHECK_FALSE(
                visibility::EvaluateCoverage(state, 1.0f, context).covered);
        }
    }

    // Coverage over the whole pattern rises monotonically with fade and hits
    // every intermediate level, which is what makes a fade look continuous.
    std::uint32_t previous = 0;
    const auto cells = visibility::kDitherExtent * visibility::kDitherExtent;
    for (std::uint32_t step = 0; step <= cells; ++step) {
        state.fade = static_cast<float>(step) / static_cast<float>(cells);
        std::uint32_t kept = 0;
        for (std::uint32_t y = 0; y < visibility::kDitherExtent; ++y) {
            for (std::uint32_t x = 0; x < visibility::kDitherExtent; ++x) {
                visibility::CoverageContext context{};
                context.pixelX = x;
                context.pixelY = y;
                if (visibility::EvaluateCoverage(state, 1.0f, context)
                        .covered) {
                    ++kept;
                }
            }
        }
        CHECK(kept == step);
        CHECK(kept >= previous);
        previous = kept;
    }

    // The pattern is a function of the pixel, so the same pixel resolves the
    // same way in the depth pass and the color pass.
    state.fade = 0.5f;
    for (std::uint32_t y = 0; y < visibility::kDitherExtent; ++y) {
        for (std::uint32_t x = 0; x < visibility::kDitherExtent; ++x) {
            visibility::CoverageContext color{};
            color.pixelX = x;
            color.pixelY = y;
            auto depth = color;
            depth.depthOnly = true;
            CHECK(visibility::EvaluateCoverage(state, 1.0f, color).covered ==
                visibility::EvaluateCoverage(state, 1.0f, depth).covered);
        }
    }
}

TEST_CASE("P15_alpha_to_coverage_quantizes_to_the_sample_count",
    "[phase15][visibility]")
{
    auto state = BuildTestedState(0.25f);
    state.flags = visibility::AlphaToCoverage;
    visibility::CoverageContext context{};
    context.sampleCount = 4;

    CHECK(visibility::EvaluateCoverage(state, 0.0f, context).coverage ==
        Catch::Approx(0.0f));
    CHECK(visibility::EvaluateCoverage(state, 1.0f, context).coverage ==
        Catch::Approx(1.0f));
    // Partial coverage lands on a representable sample fraction.
    const auto partial =
        visibility::EvaluateCoverage(state, 0.6f, context).coverage;
    CHECK(partial > 0.0f);
    CHECK(partial < 1.0f);
    CHECK(partial * 4.0f == Catch::Approx(std::round(partial * 4.0f)));

    // Single-sample rendering has no partial coverage to give, so it must
    // fall back to the same binary result as a plain alpha test.
    visibility::CoverageContext single{};
    single.sampleCount = 1;
    const auto plain = BuildTestedState(0.25f);
    for (std::uint32_t step = 0; step <= 255; ++step) {
        const auto alpha = static_cast<float>(step) / 255.0f;
        CHECK(visibility::EvaluateCoverage(state, alpha, single).covered ==
            visibility::EvaluateCoverage(plain, alpha, single).covered);
    }
}

TEST_CASE("P15_mip_scales_preserve_alpha_coverage_across_the_chain",
    "[phase15][visibility]")
{
    // A cutout that fades below the reference as it is averaged down, which
    // is the classic disappearing-foliage defect. Every level here has enough
    // texels to represent the target coverage of one quarter.
    const auto makeLevel = [](const std::uint32_t extent,
                              const float opaqueAlpha,
                              const float restAlpha) {
        visibility::AlphaMipLevel level{};
        level.width = extent;
        level.height = extent;
        const auto texels = extent * extent;
        level.alpha.assign(texels, restAlpha);
        for (std::uint32_t index = 0; index < texels / 4; ++index) {
            level.alpha[index] = opaqueAlpha;
        }
        return level;
    };
    std::vector<visibility::AlphaMipLevel> chain{
        makeLevel(8, 1.00f, 0.00f),
        // Below the reference at unit scale: this level has lost its cutout.
        makeLevel(4, 0.45f, 0.10f),
        makeLevel(2, 0.35f, 0.05f),
    };

    const auto reference = 0.5f;
    std::vector<float> scales;
    REQUIRE(visibility::ComputeAlphaCoverageScales(chain, reference, scales) ==
        visibility::VisibilityError::None);
    REQUIRE(scales.size() == chain.size());
    // Mip 0 defines the target, so it is never rescaled.
    CHECK(scales[0] == Catch::Approx(1.0f));

    const auto baseCoverage = visibility::AlphaCoverage(chain[0], reference,
        1.0f);
    CHECK(baseCoverage == Catch::Approx(0.25f));
    for (std::size_t level = 0; level < chain.size(); ++level) {
        const auto coverage = visibility::AlphaCoverage(
            chain[level], reference, scales[level]);
        CHECK(coverage == Catch::Approx(baseCoverage).margin(0.05));
        CHECK(scales[level] > 0.0f);
    }
    // Both coarser mips needed amplification to keep their coverage.
    CHECK(scales[1] > 1.0f);
    CHECK(scales[2] > 1.0f);

    std::vector<float> rejected;
    CHECK(visibility::ComputeAlphaCoverageScales({}, reference, rejected) ==
        visibility::VisibilityError::InvalidMipChain);
    auto ragged = chain;
    ragged[1].alpha.pop_back();
    CHECK(visibility::ComputeAlphaCoverageScales(ragged, reference, rejected) ==
        visibility::VisibilityError::InvalidMipChain);
    CHECK(visibility::ComputeAlphaCoverageScales(chain, 1.5f, rejected) ==
        visibility::VisibilityError::InvalidCutoff);
}

TEST_CASE("P15_two_sided_shading_frames_stay_in_the_geometric_hemisphere",
    "[phase15][visibility]")
{
    visibility::ShadingFrameInput input{};
    input.geometricNormal = {0.0f, 0.0f, 1.0f};
    input.shadingNormal = {0.0f, 0.30f, 0.95f};
    input.tangent = {1.0f, 0.0f, 0.0f};
    input.bitangent = {0.0f, 1.0f, 0.0f};

    visibility::ShadingFrame frame{};
    REQUIRE(visibility::ResolveShadingFrame(visibility::FaceMode::TwoSided,
        false, 1.0f, input, frame) == visibility::VisibilityError::None);
    CHECK(frame.geometricNormal[2] > 0.0f);
    CHECK(frame.shadingNormal[2] > 0.0f);
    CHECK(frame.flipped == false);

    // A back face of a two-sided surface flips both normals so lighting sees
    // the side it is actually looking at.
    REQUIRE(visibility::ResolveShadingFrame(visibility::FaceMode::TwoSided,
        true, 1.0f, input, frame) == visibility::VisibilityError::None);
    CHECK(frame.geometricNormal[2] < 0.0f);
    CHECK(frame.shadingNormal[2] < 0.0f);
    CHECK(frame.flipped);

    // A single-sided surface never flips, because a back face of it is culled
    // rather than shaded.
    REQUIRE(visibility::ResolveShadingFrame(visibility::FaceMode::FrontOnly,
        true, 1.0f, input, frame) == visibility::VisibilityError::None);
    CHECK(frame.geometricNormal[2] > 0.0f);
    CHECK(frame.flipped == false);

    // Whatever the face, the shading normal stays on the geometric side.
    for (const auto back : {false, true}) {
        REQUIRE(visibility::ResolveShadingFrame(
            visibility::FaceMode::TwoSided, back, 1.0f, input, frame) ==
            visibility::VisibilityError::None);
        const auto dot =
            frame.geometricNormal[0] * frame.shadingNormal[0] +
            frame.geometricNormal[1] * frame.shadingNormal[1] +
            frame.geometricNormal[2] * frame.shadingNormal[2];
        CHECK(dot > 0.0f);
    }

    auto degenerate = input;
    degenerate.geometricNormal = {0.0f, 0.0f, 0.0f};
    CHECK(visibility::ResolveShadingFrame(visibility::FaceMode::TwoSided,
        false, 1.0f, degenerate, frame) ==
        visibility::VisibilityError::InvalidNormalFrame);

    // A shading normal that has fallen below the geometric horizon is lifted
    // back onto it rather than being shaded from behind the surface.
    auto belowHorizon = input;
    belowHorizon.shadingNormal = {0.0f, 0.95f, -0.30f};
    REQUIRE(visibility::ResolveShadingFrame(visibility::FaceMode::TwoSided,
        false, 1.0f, belowHorizon, frame) ==
        visibility::VisibilityError::None);
    CHECK(frame.shadingNormal[2] >= 0.0f);
    CHECK(frame.liftedToHorizon);
}


TEST_CASE("P20_engine_winding_declaration_converts_into_the_packet_convention",
    "[phase20][visibility]")
{
    using visibility::PacketFrontFaceFromEngine;
    // D3D11 decides which side of a triangle faces the camera in *screen*
    // space -- after the viewport transform, where Y points down. The packet
    // declares winding in mathematical NDC, where Y points up, because that is
    // what the backend documents and what the offline fixtures are built
    // against. Flipping Y reverses a triangle's signed area, so the two
    // conventions differ by exactly one inversion.
    //
    // Measured live before this existed: the engine declared
    // FrontCounterClockwise for all 1,043 meshes in the frame, the flag was
    // copied into the packet unconverted, and the backend's own Y-down
    // inversion then turned it into VK_FRONT_FACE_CLOCKWISE -- the opposite of
    // the engine's rule. Every single-sided model culled the face nearest the
    // camera and drew the one behind it: a loading-screen book showed its
    // cover through its own spine, with the title reading backwards. The 416
    // two-sided meshes in the same frame survived either way, which is why
    // this looked intermittent rather than total.
    CHECK(PacketFrontFaceFromEngine(true) == raster::FrontFace::Clockwise);
    CHECK(PacketFrontFaceFromEngine(false) ==
        raster::FrontFace::CounterClockwise);
}
TEST_CASE("P15_negative_determinant_transforms_flip_winding_and_handedness",
    "[phase15][visibility]")
{
    using visibility::EffectiveFrontFace;
    CHECK(EffectiveFrontFace(raster::FrontFace::CounterClockwise, 1.0f) ==
        raster::FrontFace::CounterClockwise);
    // A mirrored instance reverses triangle winding, so the declared front
    // face has to reverse with it or the whole object culls inside out.
    CHECK(EffectiveFrontFace(raster::FrontFace::CounterClockwise, -1.0f) ==
        raster::FrontFace::Clockwise);
    CHECK(EffectiveFrontFace(raster::FrontFace::Clockwise, -1.0f) ==
        raster::FrontFace::CounterClockwise);

    visibility::ShadingFrameInput input{};
    input.geometricNormal = {0.0f, 0.0f, 1.0f};
    input.shadingNormal = {0.0f, 0.0f, 1.0f};
    input.tangent = {1.0f, 0.0f, 0.0f};
    input.bitangent = {0.0f, 1.0f, 0.0f};

    visibility::ShadingFrame frame{};
    REQUIRE(visibility::ResolveShadingFrame(visibility::FaceMode::FrontOnly,
        false, -1.0f, input, frame) == visibility::VisibilityError::None);
    // The mirrored frame keeps a right-handed tangent basis by flipping the
    // bitangent, so normal maps are not mirrored into the wrong side.
    CHECK(frame.bitangent[1] == Catch::Approx(-1.0f));
    CHECK(frame.mirrored);

    REQUIRE(visibility::ResolveShadingFrame(visibility::FaceMode::FrontOnly,
        false, 1.0f, input, frame) == visibility::VisibilityError::None);
    CHECK(frame.bitangent[1] == Catch::Approx(1.0f));
    CHECK_FALSE(frame.mirrored);

    CHECK(visibility::ResolveShadingFrame(visibility::FaceMode::FrontOnly,
        false, 0.0f, input, frame) ==
        visibility::VisibilityError::InvalidDeterminant);
}

TEST_CASE("P15_visibility_records_validate_and_reject_unsupported_classes",
    "[phase15][visibility]")
{
    visibility::VisibilityRecordV1 record{};
    record.objectId = 0x5100'0000'0000'0001ull;
    record.materialId = 0x5200'0000'0000'0001ull;
    record.alpha = BuildTestedState(0.5f);
    record.faceMode = visibility::FaceMode::TwoSided;
    record.modelDeterminant = 1.0f;
    CHECK(visibility::ValidateVisibilityRecord(record) ==
        visibility::VisibilityError::None);

    auto anonymous = record;
    anonymous.objectId = 0;
    CHECK(visibility::ValidateVisibilityRecord(anonymous) ==
        visibility::VisibilityError::InvalidIdentity);

    auto outOfRange = record;
    outOfRange.alpha.reference = 1.5f;
    CHECK(visibility::ValidateVisibilityRecord(outOfRange) ==
        visibility::VisibilityError::InvalidCutoff);

    auto unknownFlag = record;
    unknownFlag.alpha.flags = 1u << 20;
    CHECK(visibility::ValidateVisibilityRecord(unknownFlag) ==
        visibility::VisibilityError::InvalidFlags);

    auto singular = record;
    singular.modelDeterminant = 0.0f;
    CHECK(visibility::ValidateVisibilityRecord(singular) ==
        visibility::VisibilityError::InvalidDeterminant);

    auto unclassified = record;
    unclassified.alpha.classification = visibility::AlphaClass::Unclassified;
    CHECK(visibility::ValidateVisibilityRecord(unclassified) ==
        visibility::VisibilityError::UnclassifiedAlpha);

    // Sorted transparency is a declared, classified class that this phase
    // does not render. It is refused at the raster boundary rather than
    // rendered incorrectly as a cutout.
    auto blended = record;
    blended.alpha.classification = visibility::AlphaClass::Blended;
    CHECK(visibility::ValidateVisibilityRecord(blended) ==
        visibility::VisibilityError::None);
    CHECK(visibility::ValidateOpaqueRasterClass(blended) ==
        visibility::VisibilityError::BlendedNotSupported);
    CHECK(visibility::ValidateOpaqueRasterClass(record) ==
        visibility::VisibilityError::None);
}
