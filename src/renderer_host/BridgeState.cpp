#include "renderer_host/BridgeState.h"

#include <algorithm>
#include <limits>

namespace vf::renderer {

BridgeExchangeError BridgeExchangeTracker::Configure(
    const std::uint64_t epoch,
    const std::uint32_t imageCount,
    const std::uint64_t completedValue) noexcept
{
    if (epoch == 0) {
        return BridgeExchangeError::InvalidEpoch;
    }
    if (imageCount == 0 || imageCount > kMaxBridgeImages) {
        return BridgeExchangeError::InvalidImageCount;
    }
    epoch_ = epoch;
    imageCount_ = imageCount;
    nextValue_ = completedValue ==
            std::numeric_limits<std::uint64_t>::max()
        ? completedValue
        : completedValue + 1;
    fault_ = BridgeExchangeError::None;
    slots_ = {};
    return BridgeExchangeError::None;
}

BridgeBeginResult BridgeExchangeTracker::Begin(
    const std::uint64_t epoch,
    const std::uint32_t imageIndex) noexcept
{
    if (imageCount_ == 0) {
        return {BridgeExchangeError::NotConfigured};
    }
    if (fault_ != BridgeExchangeError::None) {
        return {BridgeExchangeError::Faulted};
    }
    if (epoch != epoch_) {
        return {epoch < epoch_
            ? BridgeExchangeError::StaleEpoch
            : BridgeExchangeError::InvalidEpoch};
    }
    if (imageIndex >= imageCount_) {
        return {BridgeExchangeError::InvalidImageIndex};
    }
    auto& slot = slots_[imageIndex];
    if (slot.owner != BridgeImageOwner::Host) {
        return {BridgeExchangeError::ImageBusy};
    }
    if (nextValue_ >= std::numeric_limits<std::uint64_t>::max()) {
        fault_ = BridgeExchangeError::TicketOverflow;
        for (auto& current : slots_) {
            current.owner = BridgeImageOwner::Faulted;
        }
        return {BridgeExchangeError::TicketOverflow};
    }

    slot.ticket = BridgeTicket{
        epoch_, nextValue_, nextValue_ + 1, imageIndex, 0};
    nextValue_ += 2;
    slot.owner = BridgeImageOwner::PreparedForVulkan;
    return {BridgeExchangeError::None, slot.ticket};
}

BridgeExchangeError BridgeExchangeTracker::MarkVulkanQueued(
    const BridgeTicket& ticket) noexcept
{
    if (imageCount_ == 0) {
        return BridgeExchangeError::NotConfigured;
    }
    if (fault_ != BridgeExchangeError::None) {
        return BridgeExchangeError::Faulted;
    }
    if (ticket.epoch != epoch_) {
        return BridgeExchangeError::StaleEpoch;
    }
    if (ticket.imageIndex >= imageCount_) {
        return BridgeExchangeError::InvalidImageIndex;
    }
    auto& slot = slots_[ticket.imageIndex];
    if (slot.owner != BridgeImageOwner::PreparedForVulkan) {
        return BridgeExchangeError::WrongState;
    }
    if (slot.ticket != ticket) {
        return BridgeExchangeError::TicketMismatch;
    }
    slot.owner = BridgeImageOwner::VulkanQueued;
    return BridgeExchangeError::None;
}

BridgeExchangeError BridgeExchangeTracker::CompleteHostAcquire(
    const BridgeTicket& ticket) noexcept
{
    if (imageCount_ == 0) {
        return BridgeExchangeError::NotConfigured;
    }
    if (fault_ != BridgeExchangeError::None) {
        return BridgeExchangeError::Faulted;
    }
    if (ticket.epoch != epoch_) {
        return BridgeExchangeError::StaleEpoch;
    }
    if (ticket.imageIndex >= imageCount_) {
        return BridgeExchangeError::InvalidImageIndex;
    }
    auto& slot = slots_[ticket.imageIndex];
    if (slot.owner != BridgeImageOwner::VulkanQueued) {
        return BridgeExchangeError::WrongState;
    }
    if (slot.ticket != ticket) {
        return BridgeExchangeError::TicketMismatch;
    }
    slot.owner = BridgeImageOwner::Host;
    slot.ticket = {};
    return BridgeExchangeError::None;
}

BridgeExchangeError BridgeExchangeTracker::Resize(
    const std::uint64_t newEpoch,
    const std::uint32_t imageCount) noexcept
{
    if (imageCount_ == 0) {
        return BridgeExchangeError::NotConfigured;
    }
    if (fault_ != BridgeExchangeError::None) {
        return BridgeExchangeError::Faulted;
    }
    if (newEpoch <= epoch_) {
        return newEpoch < epoch_
            ? BridgeExchangeError::StaleEpoch
            : BridgeExchangeError::InvalidEpoch;
    }
    if (imageCount == 0 || imageCount > kMaxBridgeImages) {
        return BridgeExchangeError::InvalidImageCount;
    }
    const auto inFlight = std::any_of(
        slots_.begin(),
        slots_.begin() + imageCount_,
        [](const Slot& slot) {
            return slot.owner != BridgeImageOwner::Host;
        });
    if (inFlight) {
        return BridgeExchangeError::ResizeWhileInFlight;
    }
    epoch_ = newEpoch;
    imageCount_ = imageCount;
    slots_ = {};
    return BridgeExchangeError::None;
}

void BridgeExchangeTracker::ReportTimeout(const BridgeTicket& ticket) noexcept
{
    if (fault_ != BridgeExchangeError::None ||
        ticket.epoch != epoch_ ||
        ticket.imageIndex >= imageCount_ ||
        slots_[ticket.imageIndex].ticket != ticket) {
        return;
    }
    fault_ = BridgeExchangeError::Timeout;
    for (auto& slot : slots_) {
        slot.owner = BridgeImageOwner::Faulted;
    }
}

void BridgeExchangeTracker::ReportDeviceRemoved() noexcept
{
    if (fault_ == BridgeExchangeError::None) {
        fault_ = BridgeExchangeError::DeviceRemoved;
        for (auto& slot : slots_) {
            slot.owner = BridgeImageOwner::Faulted;
        }
    }
}

void BridgeExchangeTracker::ReportBackendFailure() noexcept
{
    if (fault_ == BridgeExchangeError::None) {
        fault_ = BridgeExchangeError::BackendFailure;
        for (auto& slot : slots_) {
            slot.owner = BridgeImageOwner::Faulted;
        }
    }
}

BridgeImageOwner BridgeExchangeTracker::Owner(
    const std::uint32_t imageIndex) const noexcept
{
    if (imageIndex >= imageCount_) {
        return BridgeImageOwner::Faulted;
    }
    return slots_[imageIndex].owner;
}

BridgeExchangeError BridgeExchangeTracker::Fault() const noexcept
{
    return fault_;
}

std::uint64_t BridgeExchangeTracker::Epoch() const noexcept
{
    return epoch_;
}

std::uint64_t BridgeExchangeTracker::LastIssuedValue() const noexcept
{
    return nextValue_ == 0 ? 0 : nextValue_ - 1;
}

bool BridgeExchangeTracker::CanDestroy() const noexcept
{
    if (fault_ != BridgeExchangeError::None || imageCount_ == 0) {
        return true;
    }
    return std::all_of(
        slots_.begin(),
        slots_.begin() + imageCount_,
        [](const Slot& slot) {
            return slot.owner == BridgeImageOwner::Host;
        });
}

}
