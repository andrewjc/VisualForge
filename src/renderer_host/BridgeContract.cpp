#include "renderer_host/BridgeContract.h"

#include <array>

namespace vf::renderer {

std::optional<BridgeFormatContract> FindBridgeFormat(
    const BridgeFormat format) noexcept
{
    constexpr std::array contracts{
        BridgeFormatContract{
            BridgeFormat::R8G8B8A8Unorm,
            4,
            BridgeTransferSource |
                BridgeTransferDestination |
                BridgeSampled |
                BridgeRenderTarget,
            true,
        },
    };
    for (const auto& contract : contracts) {
        if (contract.format == format) {
            return contract;
        }
    }
    return std::nullopt;
}

BridgeDescriptionError ValidateBridgeDescription(
    const BridgeImageDescription& description) noexcept
{
    if (description.width == 0 || description.height == 0) {
        return BridgeDescriptionError::EmptyExtent;
    }
    constexpr std::uint32_t kMaximumExtent = 16384;
    if (description.width > kMaximumExtent ||
        description.height > kMaximumExtent) {
        return BridgeDescriptionError::ExtentTooLarge;
    }
    if (description.sampleCount != 1) {
        return BridgeDescriptionError::InvalidSampleCount;
    }
    const auto contract = FindBridgeFormat(description.format);
    if (!contract.has_value() || !contract->ntHandleShareable) {
        return BridgeDescriptionError::UnsupportedFormat;
    }
    constexpr auto requiredUsage =
        BridgeTransferDestination | BridgeSampled;
    if ((description.usage & requiredUsage) != requiredUsage ||
        (description.usage & ~contract->allowedUsage) != 0) {
        return BridgeDescriptionError::UnsupportedUsage;
    }
    return BridgeDescriptionError::None;
}

}
