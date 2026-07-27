#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace vf::renderer::texture {

enum class ResidencyError : std::uint8_t
{
    None,
    InvalidGeneration,
    InvalidMip,
    DuplicateUpload,
    NotResident,
};

struct PublishedMipView
{
    std::uint32_t baseMip{};
    std::uint32_t levelCount{};
    std::uint64_t revision{};

    friend bool operator==(const PublishedMipView&,
        const PublishedMipView&) = default;
};

class TextureResidency
{
public:
    TextureResidency(std::uint32_t generation, std::uint32_t mipLevels);
    [[nodiscard]] ResidencyError ScheduleUpload(
        std::uint32_t generation,
        std::uint32_t mipLevel,
        std::uint64_t barrierCompletion) noexcept;
    [[nodiscard]] ResidencyError Advance(
        std::uint32_t generation,
        std::uint64_t completedValue) noexcept;
    [[nodiscard]] ResidencyError Evict(
        std::uint32_t generation,
        std::uint32_t mipLevel) noexcept;
    [[nodiscard]] std::optional<PublishedMipView> Published() const noexcept;

private:
    enum class State : std::uint8_t { Missing, Uploading, Resident };
    struct MipState { State state{State::Missing}; std::uint64_t completion{}; };
    std::uint32_t generation_{};
    std::vector<MipState> mips_;
    std::optional<PublishedMipView> published_;
    std::uint64_t revision_{};
};

class DescriptorQuarantine
{
public:
    explicit DescriptorQuarantine(std::uint32_t capacity);
    [[nodiscard]] std::optional<std::uint32_t> Acquire() noexcept;
    [[nodiscard]] bool Retire(
        std::uint32_t index,
        std::uint64_t completionValue) noexcept;
    // Returns a slot that was acquired but never published to the GPU. It
    // skips quarantine precisely because no submission can reference it.
    [[nodiscard]] bool Release(std::uint32_t index) noexcept;
    void Advance(std::uint64_t completedValue) noexcept;
    [[nodiscard]] bool IsQuarantined(std::uint32_t index) const noexcept;

private:
    struct Retired { std::uint32_t index{}; std::uint64_t completion{}; };
    std::vector<std::uint32_t> free_;
    std::vector<std::uint32_t> live_;
    std::vector<Retired> retired_;
};

}
