#pragma once

namespace vf::capture_requests {

// Reads VISUALFORGE_CAPTURE_REQUEST. Without it the poller stays inert and
// captures keep their load-time environment behaviour.
[[nodiscard]] bool Configure() noexcept;
[[nodiscard]] bool Enabled() noexcept;

// Called once per presented frame. Polling is rate limited because it walks
// the file system on the render thread.
void Poll() noexcept;

}
