#include "renderer_core/RasterGolden.h"
#include "renderer_core/EngineMaterial.h"
#include "renderer_core/EngineTexture.h"
#include "renderer_core/EngineView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace vf::renderer::raster {

ReferenceRasterError ProjectPacketForView(
    const DecodedPacket& packet,
    const view::ViewRecordV1& view,
    DecodedPacket& projected) noexcept
{
    projected = {};
    if (view::ValidateView(view) != view::ViewError::None ||
        view.outputWidth != packet.header.width ||
        view.outputHeight != packet.header.height ||
        view.viewport.x != packet.header.viewportX ||
        view.viewport.y != packet.header.viewportY ||
        view.viewport.width != packet.header.viewportWidth ||
        view.viewport.height != packet.header.viewportHeight ||
        view.viewport.minimumDepth != packet.header.viewportMinDepth ||
        view.viewport.maximumDepth != packet.header.viewportMaxDepth ||
        view.scissor.x != packet.header.scissorX ||
        view.scissor.y != packet.header.scissorY ||
        view.scissor.width != packet.header.scissorWidth ||
        view.scissor.height != packet.header.scissorHeight) {
        return ReferenceRasterError::InvalidPacket;
    }
    try {
        projected = packet;
        for (auto& vertex : projected.vertices) {
            const double input[]{vertex.position[0], vertex.position[1],
                vertex.position[2], 1.0};
            double clip[4]{};
            for (std::size_t row = 0; row < 4; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    clip[row] += static_cast<double>(
                        view.viewProjection.elements[row * 4 + column]) *
                        input[column];
                }
            }
            if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) ||
                !std::isfinite(clip[2]) || !std::isfinite(clip[3]) ||
                clip[3] <= 1.0e-12) {
                projected = {};
                return ReferenceRasterError::UnsupportedState;
            }
            for (std::size_t component = 0; component < 3; ++component) {
                vertex.position[component] = static_cast<float>(
                    clip[component] / clip[3]);
                if (!std::isfinite(vertex.position[component])) {
                    projected = {};
                    return ReferenceRasterError::UnsupportedState;
                }
            }
        }
        return ReferenceRasterError::None;
    } catch (...) {
        projected = {};
        return ReferenceRasterError::InvalidPacket;
    }
}

const Rgba8& RasterImage::At(
    const std::uint32_t x,
    const std::uint32_t y) const
{
    if (x >= width || y >= height) {
        throw std::out_of_range("raster pixel");
    }
    return pixels[static_cast<std::size_t>(y) * width + x];
}

bool RasterComparison::Within(
    const std::uint32_t maximumError,
    const double maximumMeanError,
    const std::uint64_t maximumDifferingPixels) const noexcept
{
    return maximumChannelError <= maximumError &&
        meanAbsoluteError <= maximumMeanError &&
        differingPixels <= maximumDifferingPixels;
}

ReferenceRasterError RenderReferenceImpl(
    const DecodedPacket& packet,
    const texture::CapturedTexture* sampledTexture,
    const material::MaterialReplayBundle* sampledMaterial,
    RasterImage& image) noexcept
{
    image = {};
    if (packet.header.width == 0 || packet.header.height == 0 ||
        packet.vertices.empty() || packet.indices.empty() ||
        packet.draws.empty() || packet.materials.empty()) {
        return ReferenceRasterError::InvalidPacket;
    }
    struct LinearPixel
    {
        float color[4];
        float depth;
    };
    try {
        const auto pixelCount = static_cast<std::size_t>(packet.header.width) *
            packet.header.height;
        std::vector<LinearPixel> linear(pixelCount);
        for (auto& pixel : linear) {
            pixel.color[0] = 0.01f;
            pixel.color[1] = 0.021f;
            pixel.color[2] = 0.04f;
            pixel.color[3] = 1.0f;
            pixel.depth = 1.0f;
        }

        const auto screen = [&packet](const auto& vertex) {
            return std::array<float, 3>{
                packet.header.viewportX +
                    (vertex.position[0] * 0.5f + 0.5f) *
                        packet.header.viewportWidth,
                packet.header.viewportY +
                    (vertex.position[1] * 0.5f + 0.5f) *
                        packet.header.viewportHeight,
                packet.header.viewportMinDepth + vertex.position[2] *
                    (packet.header.viewportMaxDepth -
                        packet.header.viewportMinDepth),
            };
        };
        const auto edge = [](const std::array<float, 3>& a,
                             const std::array<float, 3>& b,
                             const float x,
                             const float y) {
            return (b[0] - a[0]) * (y - a[1]) -
                (b[1] - a[1]) * (x - a[0]);
        };

        const MaterialRegistry registry{packet.materials};
        if (registry.HasDuplicateIds()) {
            return ReferenceRasterError::InvalidPacket;
        }
        for (const auto& draw : packet.draws) {
            const auto* rasterMaterial = registry.Resolve(draw.materialId);
            if (rasterMaterial == nullptr) {
                return ReferenceRasterError::InvalidPacket;
            }
            for (std::uint32_t local = 0; local < draw.indexCount; local += 3) {
                const auto indexA = static_cast<std::size_t>(
                    static_cast<std::int64_t>(
                        packet.indices[draw.firstIndex + local]) +
                    draw.vertexOffset);
                const auto indexB = static_cast<std::size_t>(
                    static_cast<std::int64_t>(
                        packet.indices[draw.firstIndex + local + 1]) +
                    draw.vertexOffset);
                const auto indexC = static_cast<std::size_t>(
                    static_cast<std::int64_t>(
                        packet.indices[draw.firstIndex + local + 2]) +
                    draw.vertexOffset);
                if (indexA >= packet.vertices.size() ||
                    indexB >= packet.vertices.size() ||
                    indexC >= packet.vertices.size()) {
                    return ReferenceRasterError::InvalidPacket;
                }
                const auto& vertexA = packet.vertices[indexA];
                const auto& vertexB = packet.vertices[indexB];
                const auto& vertexC = packet.vertices[indexC];
                const auto winding = ClassifyTriangle(
                    vertexA, vertexB, vertexC);
                const auto expectedWinding =
                    draw.frontFace == FrontFace::CounterClockwise
                    ? TriangleWinding::CounterClockwise
                    : TriangleWinding::Clockwise;
                if (winding != expectedWinding) {
                    continue;
                }
                const auto a = screen(vertexA);
                const auto b = screen(vertexB);
                const auto c = screen(vertexC);
                const auto area = edge(a, b, c[0], c[1]);
                if (std::abs(area) <= std::numeric_limits<float>::epsilon()) {
                    continue;
                }
                const auto minX = std::max<std::int32_t>(
                    packet.header.scissorX,
                    static_cast<std::int32_t>(std::floor(
                        std::min({a[0], b[0], c[0]}))));
                const auto minY = std::max<std::int32_t>(
                    packet.header.scissorY,
                    static_cast<std::int32_t>(std::floor(
                        std::min({a[1], b[1], c[1]}))));
                const auto maxX = std::min<std::int32_t>(
                    packet.header.scissorX +
                        static_cast<std::int32_t>(packet.header.scissorWidth) - 1,
                    static_cast<std::int32_t>(std::ceil(
                        std::max({a[0], b[0], c[0]}))));
                const auto maxY = std::min<std::int32_t>(
                    packet.header.scissorY +
                        static_cast<std::int32_t>(packet.header.scissorHeight) - 1,
                    static_cast<std::int32_t>(std::ceil(
                        std::max({a[1], b[1], c[1]}))));
                for (auto y = minY; y <= maxY; ++y) {
                    for (auto x = minX; x <= maxX; ++x) {
                        const auto sampleX = static_cast<float>(x) + 0.5f;
                        const auto sampleY = static_cast<float>(y) + 0.5f;
                        const auto weightA = edge(b, c, sampleX, sampleY) / area;
                        const auto weightB = edge(c, a, sampleX, sampleY) / area;
                        const auto weightC = edge(a, b, sampleX, sampleY) / area;
                        constexpr float edgeTolerance = -1.0e-6f;
                        if (weightA < edgeTolerance ||
                            weightB < edgeTolerance ||
                            weightC < edgeTolerance) {
                            continue;
                        }
                        const auto depth = weightA * a[2] +
                            weightB * b[2] + weightC * c[2];
                        auto& destination = linear[
                            static_cast<std::size_t>(y) * packet.header.width +
                            static_cast<std::size_t>(x)];
                        const auto depthPass =
                            draw.depthCompare == DepthCompare::Always ||
                            (draw.depthCompare == DepthCompare::Less &&
                                depth < destination.depth) ||
                            (draw.depthCompare == DepthCompare::LessOrEqual &&
                                depth <= destination.depth);
                        if (!depthPass) {
                            continue;
                        }
                        const auto u = weightA * vertexA.texCoord[0] +
                            weightB * vertexB.texCoord[0] +
                            weightC * vertexC.texCoord[0];
                        const auto v = weightA * vertexA.texCoord[1] +
                            weightB * vertexB.texCoord[1] +
                            weightC * vertexC.texCoord[1];
                        if (sampledMaterial != nullptr) {
                            if (sampledMaterial->transferVersion !=
                                material::kMaterialTransferVersion) {
                                return ReferenceRasterError::UnsupportedState;
                            }
                            const auto uv = material::TransformMaterialUv(
                                sampledMaterial->material, {u, v});
                            material::MaterialSurfaceInput input{};
                            if (texture::SampleTexture2D(
                                    sampledMaterial->textures[0],
                                    uv[0], uv[1], 0.0f, input.baseColor) !=
                                    texture::TexturePacketError::None ||
                                texture::SampleTexture2D(
                                    sampledMaterial->textures[1],
                                    uv[0], uv[1], 0.0f, input.normal) !=
                                    texture::TexturePacketError::None ||
                                texture::SampleTexture2D(
                                    sampledMaterial->textures[2],
                                    uv[0], uv[1], 0.0f,
                                    input.smoothSpec) !=
                                    texture::TexturePacketError::None) {
                                return ReferenceRasterError::UnsupportedState;
                            }
                            for (std::size_t channel = 0;
                                 channel < 3; ++channel) {
                                input.vertexColor[channel] =
                                    (weightA * vertexA.color[channel] +
                                     weightB * vertexB.color[channel] +
                                     weightC * vertexC.color[channel]) *
                                    rasterMaterial->baseColor[channel];
                            }
                            material::MaterialSurface surface{};
                            const auto transfer =
                                material::MakeDefaultMaterialTransferLut();
                            if (material::EvaluateMaterialSurface(
                                    sampledMaterial->material, input,
                                    transfer, surface) !=
                                material::MaterialError::None) {
                                return ReferenceRasterError::UnsupportedState;
                            }
                            if (surface.discarded) continue;
                            const auto lighting = material::EvaluateGgxDirect(
                                surface, {0.0f, 0.0f, 1.0f},
                                {-0.45f, 0.55f, 1.0f},
                                {4.0f, 4.0f, 4.0f});
                            destination.depth = depth;
                            for (std::size_t channel = 0;
                                 channel < 3; ++channel) {
                                destination.color[channel] =
                                    lighting.combined[channel] +
                                    surface.baseColor[channel] * 0.035f;
                            }
                            destination.color[3] = surface.opacity *
                                rasterMaterial->baseColor[3];
                            continue;
                        }
                        texture::SampledColor sampled{
                            1.0f, 1.0f, 1.0f, 1.0f};
                        if (sampledTexture != nullptr) {
                            if (texture::SampleTexture2D(
                                    *sampledTexture, u, v, 0.0f, sampled) !=
                                texture::TexturePacketError::None) {
                                return ReferenceRasterError::UnsupportedState;
                            }
                        }
                        destination.depth = depth;
                        const float textureChannels[]{
                            sampled.r, sampled.g, sampled.b};
                        for (std::size_t channel = 0; channel < 3; ++channel) {
                            destination.color[channel] =
                                (weightA * vertexA.color[channel] +
                                    weightB * vertexB.color[channel] +
                                    weightC * vertexC.color[channel]) *
                                rasterMaterial->baseColor[channel] *
                                textureChannels[channel];
                        }
                        destination.color[3] =
                            rasterMaterial->baseColor[3] * sampled.a;
                    }
                }
            }
        }

        image.width = packet.header.width;
        image.height = packet.header.height;
        image.pixels.resize(pixelCount);
        for (std::size_t index = 0; index < pixelCount; ++index) {
            image.pixels[index] = Rgba8{
                ToneMapToSrgb8(linear[index].color[0]),
                ToneMapToSrgb8(linear[index].color[1]),
                ToneMapToSrgb8(linear[index].color[2]),
                static_cast<std::uint8_t>(std::clamp(
                    std::lround(linear[index].color[3] * 255.0f),
                    0l, 255l)),
            };
        }
        return ReferenceRasterError::None;
    } catch (...) {
        image = {};
        return ReferenceRasterError::InvalidPacket;
    }
}

ReferenceRasterError RenderReference(
    const DecodedPacket& packet,
    RasterImage& image) noexcept
{
    return RenderReferenceImpl(packet, nullptr, nullptr, image);
}

ReferenceRasterError RenderReferenceTextured(
    const DecodedPacket& packet,
    const texture::CapturedTexture& texture,
    RasterImage& image) noexcept
{
    return RenderReferenceImpl(packet, &texture, nullptr, image);
}

ReferenceRasterError RenderReferenceMaterial(
    const DecodedPacket& packet,
    const material::MaterialReplayBundle& material,
    RasterImage& image) noexcept
{
    return RenderReferenceImpl(packet, nullptr, &material, image);
}

RasterComparison CompareRaster(
    const std::span<const Rgba8> expected,
    const std::span<const Rgba8> actual) noexcept
{
    RasterComparison result;
    result.comparedPixels = std::max(expected.size(), actual.size());
    std::uint64_t totalError{};
    const auto common = std::min(expected.size(), actual.size());
    for (std::size_t index = 0; index < common; ++index) {
        const std::uint8_t expectedChannels[]{
            expected[index].r, expected[index].g,
            expected[index].b, expected[index].a};
        const std::uint8_t actualChannels[]{
            actual[index].r, actual[index].g,
            actual[index].b, actual[index].a};
        bool differs{};
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const auto error = static_cast<std::uint32_t>(std::abs(
                static_cast<int>(expectedChannels[channel]) -
                static_cast<int>(actualChannels[channel])));
            totalError += error;
            result.maximumChannelError = std::max(
                result.maximumChannelError, error);
            differs = differs || error != 0;
        }
        result.differingPixels += differs ? 1u : 0u;
    }
    if (expected.size() != actual.size()) {
        const auto missing = result.comparedPixels - common;
        result.differingPixels += missing;
        result.maximumChannelError = 255;
        totalError += missing * 4 * 255;
    }
    if (result.comparedPixels != 0) {
        result.meanAbsoluteError = static_cast<double>(totalError) /
            static_cast<double>(result.comparedPixels * 4);
    }
    return result;
}

std::uint8_t ToneMapToSrgb8(const float linear) noexcept
{
    const auto nonNegative = std::max(linear, 0.0f);
    const auto mapped = nonNegative / (1.0f + nonNegative);
    const auto srgb = mapped <= 0.0031308f
        ? mapped * 12.92f
        : 1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::clamp(
        std::lround(srgb * 255.0f), 0l, 255l));
}

}
