#pragma once

#include <cstdint>
#include <optional>

namespace vf::renderer {

enum class BridgeFormat : std::uint32_t
{
    Unknown,
    R8G8B8A8Unorm,
    B8G8R8A8Unorm,
    R16G16B16A16Float
};

enum BridgeUsage : std::uint32_t
{
    BridgeTransferSource = 1u << 0,
    BridgeTransferDestination = 1u << 1,
    BridgeSampled = 1u << 2,
    BridgeRenderTarget = 1u << 3
};

struct BridgeFormatContract
{
    BridgeFormat format{BridgeFormat::Unknown};
    std::uint32_t bytesPerPixel{};
    std::uint32_t allowedUsage{};
    bool ntHandleShareable{};

    friend bool operator==(const BridgeFormatContract&,
                           const BridgeFormatContract&) = default;
};

struct BridgeImageDescription
{
    std::uint32_t width{};
    std::uint32_t height{};
    BridgeFormat format{BridgeFormat::Unknown};
    std::uint32_t usage{};
    std::uint32_t sampleCount{1};
};

enum class BridgeDescriptionError : std::uint8_t
{
    None,
    EmptyExtent,
    ExtentTooLarge,
    UnsupportedFormat,
    UnsupportedUsage,
    InvalidSampleCount
};

[[nodiscard]] std::optional<BridgeFormatContract> FindBridgeFormat(
    BridgeFormat format) noexcept;
[[nodiscard]] BridgeDescriptionError ValidateBridgeDescription(
    const BridgeImageDescription& description) noexcept;

}
