#include "renderer_host/BridgeState.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

using namespace vf::renderer;

TEST_CASE("P05_bridge_tickets_are_monotonic_and_slot_owned", "[unit][phase05]")
{
    BridgeExchangeTracker tracker;
    REQUIRE(tracker.Configure(7, 3) == BridgeExchangeError::None);
    CHECK(tracker.Epoch() == 7);
    CHECK(tracker.CanDestroy());

    const auto first = tracker.Begin(7, 0);
    REQUIRE(first);
    CHECK(first.ticket == BridgeTicket{7, 1, 2, 0, 0});
    CHECK(tracker.Owner(0) == BridgeImageOwner::PreparedForVulkan);
    CHECK_FALSE(tracker.CanDestroy());
    REQUIRE(tracker.MarkVulkanQueued(first.ticket) ==
            BridgeExchangeError::None);
    CHECK(tracker.Owner(0) == BridgeImageOwner::VulkanQueued);
    REQUIRE(tracker.CompleteHostAcquire(first.ticket) ==
            BridgeExchangeError::None);
    CHECK(tracker.Owner(0) == BridgeImageOwner::Host);

    const auto second = tracker.Begin(7, 2);
    REQUIRE(second);
    CHECK(second.ticket.releaseValue == 3);
    CHECK(second.ticket.readyValue == 4);
    CHECK(tracker.LastIssuedValue() == 4);
}

TEST_CASE("P05_bridge_rejects_double_acquire_stale_and_wrong_tickets", "[unit][phase05]")
{
    BridgeExchangeTracker tracker;
    CHECK(tracker.Begin(1, 0).error ==
          BridgeExchangeError::NotConfigured);
    CHECK(tracker.Configure(0, 1) == BridgeExchangeError::InvalidEpoch);
    CHECK(tracker.Configure(1, 0) ==
          BridgeExchangeError::InvalidImageCount);
    REQUIRE(tracker.Configure(4, 2) == BridgeExchangeError::None);

    CHECK(tracker.Begin(3, 0).error == BridgeExchangeError::StaleEpoch);
    CHECK(tracker.Begin(4, 2).error ==
          BridgeExchangeError::InvalidImageIndex);
    const auto ticket = tracker.Begin(4, 1);
    REQUIRE(ticket);
    CHECK(tracker.Begin(4, 1).error == BridgeExchangeError::ImageBusy);
    CHECK(tracker.CompleteHostAcquire(ticket.ticket) ==
          BridgeExchangeError::WrongState);

    auto wrong = ticket.ticket;
    ++wrong.readyValue;
    CHECK(tracker.MarkVulkanQueued(wrong) ==
          BridgeExchangeError::TicketMismatch);
    REQUIRE(tracker.MarkVulkanQueued(ticket.ticket) ==
            BridgeExchangeError::None);
    CHECK(tracker.MarkVulkanQueued(ticket.ticket) ==
          BridgeExchangeError::WrongState);
    CHECK(tracker.CompleteHostAcquire(wrong) ==
          BridgeExchangeError::TicketMismatch);
}

TEST_CASE("P05_resize_requires_quiescence_and_invalidates_old_epoch", "[unit][phase05]")
{
    BridgeExchangeTracker tracker;
    REQUIRE(tracker.Configure(10, 3, 20) == BridgeExchangeError::None);
    const auto active = tracker.Begin(10, 0);
    REQUIRE(active);
    CHECK(active.ticket.releaseValue == 21);
    CHECK(tracker.Resize(11, 2) ==
          BridgeExchangeError::ResizeWhileInFlight);
    REQUIRE(tracker.MarkVulkanQueued(active.ticket) ==
            BridgeExchangeError::None);
    REQUIRE(tracker.CompleteHostAcquire(active.ticket) ==
            BridgeExchangeError::None);
    REQUIRE(tracker.Resize(11, 2) == BridgeExchangeError::None);
    CHECK(tracker.Epoch() == 11);
    CHECK(tracker.MarkVulkanQueued(active.ticket) ==
          BridgeExchangeError::StaleEpoch);
    const auto resized = tracker.Begin(11, 1);
    REQUIRE(resized);
    CHECK(resized.ticket.releaseValue == 23);
}

TEST_CASE("P05_timeout_and_device_removal_are_terminal", "[unit][phase05]")
{
    BridgeExchangeTracker tracker;
    REQUIRE(tracker.Configure(1, 1) == BridgeExchangeError::None);
    const auto active = tracker.Begin(1, 0);
    REQUIRE(active);
    tracker.ReportTimeout(active.ticket);
    CHECK(tracker.Fault() == BridgeExchangeError::Timeout);
    CHECK(tracker.Owner(0) == BridgeImageOwner::Faulted);
    CHECK(tracker.Begin(1, 0).error == BridgeExchangeError::Faulted);
    CHECK(tracker.CanDestroy());

    BridgeExchangeTracker removed;
    REQUIRE(removed.Configure(2, 3) == BridgeExchangeError::None);
    removed.ReportDeviceRemoved();
    CHECK(removed.Fault() == BridgeExchangeError::DeviceRemoved);
    CHECK(removed.Owner(2) == BridgeImageOwner::Faulted);
    CHECK(removed.Resize(3, 3) == BridgeExchangeError::Faulted);
    CHECK(removed.CanDestroy());
}

TEST_CASE("P05_ticket_overflow_faults_before_wrap", "[unit][phase05]")
{
    BridgeExchangeTracker tracker;
    REQUIRE(tracker.Configure(
                1,
                1,
                std::numeric_limits<std::uint64_t>::max() - 1) ==
            BridgeExchangeError::None);
    CHECK(tracker.Begin(1, 0).error ==
          BridgeExchangeError::TicketOverflow);
    CHECK(tracker.Fault() == BridgeExchangeError::TicketOverflow);
}
