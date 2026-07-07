#pragma once

#include <array>
#include <cstdint>

namespace vf::renderer {

constexpr std::uint32_t kMaxBridgeImages = 3;

enum class BridgeImageOwner : std::uint8_t
{
    Host,
    PreparedForVulkan,
    VulkanQueued,
    Faulted
};

enum class BridgeExchangeError : std::uint8_t
{
    None,
    NotConfigured,
    InvalidImageCount,
    InvalidImageIndex,
    InvalidEpoch,
    StaleEpoch,
    ImageBusy,
    WrongState,
    TicketMismatch,
    TicketOverflow,
    ResizeWhileInFlight,
    Timeout,
    DeviceRemoved,
    BackendFailure,
    Faulted
};

struct BridgeTicket
{
    std::uint64_t epoch{};
    std::uint64_t releaseValue{};
    std::uint64_t readyValue{};
    std::uint32_t imageIndex{};
    std::uint32_t reserved{};

    friend bool operator==(const BridgeTicket&, const BridgeTicket&) = default;
};

struct BridgeBeginResult
{
    BridgeExchangeError error{BridgeExchangeError::None};
    BridgeTicket ticket{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == BridgeExchangeError::None;
    }
};

class BridgeExchangeTracker
{
public:
    [[nodiscard]] BridgeExchangeError Configure(
        std::uint64_t epoch,
        std::uint32_t imageCount,
        std::uint64_t completedValue = 0) noexcept;
    [[nodiscard]] BridgeBeginResult Begin(
        std::uint64_t epoch,
        std::uint32_t imageIndex) noexcept;
    [[nodiscard]] BridgeExchangeError MarkVulkanQueued(
        const BridgeTicket& ticket) noexcept;
    [[nodiscard]] BridgeExchangeError CompleteHostAcquire(
        const BridgeTicket& ticket) noexcept;
    [[nodiscard]] BridgeExchangeError Resize(
        std::uint64_t newEpoch,
        std::uint32_t imageCount) noexcept;
    void ReportTimeout(const BridgeTicket& ticket) noexcept;
    void ReportDeviceRemoved() noexcept;
    void ReportBackendFailure() noexcept;

    [[nodiscard]] BridgeImageOwner Owner(std::uint32_t imageIndex) const noexcept;
    [[nodiscard]] BridgeExchangeError Fault() const noexcept;
    [[nodiscard]] std::uint64_t Epoch() const noexcept;
    [[nodiscard]] std::uint64_t LastIssuedValue() const noexcept;
    [[nodiscard]] bool CanDestroy() const noexcept;

private:
    struct Slot
    {
        BridgeImageOwner owner{BridgeImageOwner::Host};
        BridgeTicket ticket{};
    };

    std::array<Slot, kMaxBridgeImages> slots_{};
    std::uint64_t epoch_{};
    std::uint64_t nextValue_{1};
    std::uint32_t imageCount_{};
    BridgeExchangeError fault_{BridgeExchangeError::None};
};

}
