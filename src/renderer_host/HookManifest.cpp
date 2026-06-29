#include "renderer_host/HookManifest.h"

#include <array>
#include <cstring>
#include <limits>

namespace vf::renderer {

namespace {

constexpr std::array kResizeBytes{
    std::byte{0x40}, std::byte{0x53}, std::byte{0x57}, std::byte{0x41},
    std::byte{0x54}, std::byte{0x41}, std::byte{0x55}, std::byte{0x48},
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x78}, std::byte{0x44},
};

constexpr std::array kBeginBytes{
    std::byte{0x40}, std::byte{0x55}, std::byte{0x41}, std::byte{0x54},
    std::byte{0x41}, std::byte{0x57}, std::byte{0x48}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x60}, std::byte{0x48}, std::byte{0x89},
};

constexpr std::array kEndBytes{
    std::byte{0x40}, std::byte{0x53}, std::byte{0x48}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x20}, std::byte{0x48}, std::byte{0x8B},
    std::byte{0xD9}, std::byte{0xB9}, std::byte{0x01}, std::byte{0x00},
};

constexpr std::array kDrawWorldBytes{
    std::byte{0x48}, std::byte{0x89}, std::byte{0x5C}, std::byte{0x24},
    std::byte{0x08}, std::byte{0x57}, std::byte{0x48}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x30}, std::byte{0x8B}, std::byte{0x0D},
};

constexpr std::array kCreateTriShapeBytes{
    std::byte{0x48}, std::byte{0x89}, std::byte{0x5C}, std::byte{0x24},
    std::byte{0x08}, std::byte{0x48}, std::byte{0x89}, std::byte{0x6C},
    std::byte{0x24}, std::byte{0x10}, std::byte{0x48}, std::byte{0x89},
};

constexpr std::array kExactMask{
    std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
    std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
    std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
};

const std::array kTargetManifest{
    HookSiteDescriptor{
        1, "Renderer::WindowSizeChanged", HookSiteKind::MaskedCode,
        0x018174F0u, kResizeBytes, kExactMask, 0},
    HookSiteDescriptor{
        2, "Renderer::Begin", HookSiteKind::MaskedCode,
        0x01817E30u, kBeginBytes, kExactMask, 0},
    HookSiteDescriptor{
        3, "Renderer::End", HookSiteKind::MaskedCode,
        0x01818080u, kEndBytes, kExactMask, 0},
    HookSiteDescriptor{
        4, "DrawWorld::Forward", HookSiteKind::MaskedCode,
        0x021F16D0u, kDrawWorldBytes, kExactMask, 0},
    HookSiteDescriptor{
        5, "NiCamera::vtable[0]", HookSiteKind::RelocatedPointer,
        0x0267DD60u, {}, {}, 0x016D2710u},
    HookSiteDescriptor{
        6, "Renderer::CreateTriShape(CPU)", HookSiteKind::MaskedCode,
        0x01818760u, kCreateTriShapeBytes, kExactMask, 0},
};

bool HasRange(
    const std::span<const std::byte> image,
    const std::size_t offset,
    const std::size_t length) noexcept
{
    return offset <= image.size() && length <= image.size() - offset;
}

HookSiteValidation Failure(
    const HookSiteError error,
    const HookSiteDescriptor& descriptor,
    const std::uint32_t mismatchOffset = 0) noexcept
{
    return HookSiteValidation{error, descriptor.id, mismatchOffset};
}

}

HookSiteValidation ValidateHookSite(
    const HookSiteDescriptor& descriptor,
    const std::span<const std::byte> image,
    const std::uintptr_t imageBase) noexcept
{
    if (descriptor.id == 0 || descriptor.name.empty()) {
        return Failure(HookSiteError::InvalidDescriptor, descriptor);
    }

    const auto offset = static_cast<std::size_t>(descriptor.rva);
    switch (descriptor.kind) {
    case HookSiteKind::MaskedCode:
        if (descriptor.expected.empty() ||
            descriptor.expected.size() != descriptor.mask.size() ||
            descriptor.expected.size() >
                static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
            descriptor.expectedTargetRva != 0) {
            return Failure(HookSiteError::InvalidDescriptor, descriptor);
        }
        if (!HasRange(image, offset, descriptor.expected.size())) {
            return Failure(HookSiteError::OutOfBounds, descriptor);
        }
        for (std::size_t index = 0; index < descriptor.expected.size(); ++index) {
            if ((image[offset + index] & descriptor.mask[index]) !=
                (descriptor.expected[index] & descriptor.mask[index])) {
                return Failure(
                    HookSiteError::ByteMismatch,
                    descriptor,
                    static_cast<std::uint32_t>(index));
            }
        }
        return HookSiteValidation{};

    case HookSiteKind::RelocatedPointer:
        if (!descriptor.expected.empty() ||
            !descriptor.mask.empty() ||
            descriptor.expectedTargetRva == 0 ||
            descriptor.expectedTargetRva >
                std::numeric_limits<std::uintptr_t>::max() - imageBase) {
            return Failure(HookSiteError::InvalidDescriptor, descriptor);
        }
        if (!HasRange(image, offset, sizeof(std::uintptr_t))) {
            return Failure(HookSiteError::OutOfBounds, descriptor);
        }
        std::uintptr_t observed{};
        std::memcpy(&observed, image.data() + offset, sizeof(observed));
        if (observed != imageBase + descriptor.expectedTargetRva) {
            return Failure(HookSiteError::PointerMismatch, descriptor);
        }
        return HookSiteValidation{};
    }

    return Failure(HookSiteError::InvalidDescriptor, descriptor);
}

HookManifestReport ValidateHookManifest(
    const std::span<const HookSiteDescriptor> manifest,
    const std::span<const std::byte> image,
    const std::uintptr_t imageBase) noexcept
{
    if (manifest.empty()) {
        return HookManifestReport{
            HookSiteValidation{HookSiteError::EmptyManifest, 0, 0}, 0};
    }

    for (std::size_t index = 0; index < manifest.size(); ++index) {
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (manifest[index].id == manifest[prior].id) {
                return HookManifestReport{
                    Failure(HookSiteError::DuplicateId, manifest[index]), 0};
            }
        }
    }

    std::size_t validated = 0;
    for (const auto& descriptor : manifest) {
        const auto result = ValidateHookSite(descriptor, image, imageBase);
        if (!result) {
            return HookManifestReport{result, validated};
        }
        ++validated;
    }
    return HookManifestReport{{}, validated};
}

std::span<const HookSiteDescriptor> TargetHookManifest_1_11_221() noexcept
{
    return kTargetManifest;
}

const char* ToString(const HookSiteError error) noexcept
{
    switch (error) {
    case HookSiteError::None:
        return "none";
    case HookSiteError::EmptyManifest:
        return "empty-manifest";
    case HookSiteError::InvalidDescriptor:
        return "invalid-descriptor";
    case HookSiteError::DuplicateId:
        return "duplicate-id";
    case HookSiteError::OutOfBounds:
        return "out-of-bounds";
    case HookSiteError::ByteMismatch:
        return "byte-mismatch";
    case HookSiteError::PointerMismatch:
        return "pointer-mismatch";
    }
    return "unknown";
}

}
