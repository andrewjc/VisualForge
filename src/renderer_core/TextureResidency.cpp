#include "renderer_core/TextureResidency.h"

#include <algorithm>

namespace vf::renderer::texture {

TextureResidency::TextureResidency(
    const std::uint32_t generation, const std::uint32_t mipLevels)
    : generation_(generation), mips_(mipLevels)
{}

ResidencyError TextureResidency::ScheduleUpload(
    const std::uint32_t generation,
    const std::uint32_t mipLevel,
    const std::uint64_t barrierCompletion) noexcept
{
    if (generation != generation_) return ResidencyError::InvalidGeneration;
    if (mipLevel >= mips_.size()) return ResidencyError::InvalidMip;
    auto& mip = mips_[mipLevel];
    if (mip.state != State::Missing) return ResidencyError::DuplicateUpload;
    mip.state = State::Uploading;
    mip.completion = barrierCompletion;
    return ResidencyError::None;
}

ResidencyError TextureResidency::Advance(
    const std::uint32_t generation,
    const std::uint64_t completedValue) noexcept
{
    if (generation != generation_) return ResidencyError::InvalidGeneration;
    for (auto& mip : mips_) {
        if (mip.state == State::Uploading &&
            mip.completion <= completedValue) {
            mip.state = State::Resident;
        }
    }
    std::optional<PublishedMipView> next;
    if (!mips_.empty() && mips_.back().state == State::Resident) {
        auto base = mips_.size() - 1;
        while (base != 0 && mips_[base - 1].state == State::Resident) {
            --base;
        }
        next = PublishedMipView{
            static_cast<std::uint32_t>(base),
            static_cast<std::uint32_t>(mips_.size() - base), 0};
    }
    const auto changed = next.has_value() != published_.has_value() ||
        (next.has_value() &&
            (next->baseMip != published_->baseMip ||
             next->levelCount != published_->levelCount));
    if (changed) {
        ++revision_;
        if (next) next->revision = revision_;
        published_ = next;
    }
    return ResidencyError::None;
}

ResidencyError TextureResidency::Evict(
    const std::uint32_t generation,
    const std::uint32_t mipLevel) noexcept
{
    if (generation != generation_) return ResidencyError::InvalidGeneration;
    if (mipLevel >= mips_.size()) return ResidencyError::InvalidMip;
    if (mips_[mipLevel].state != State::Resident) {
        return ResidencyError::NotResident;
    }
    mips_[mipLevel] = {};
    std::optional<PublishedMipView> next;
    if (!mips_.empty() && mips_.back().state == State::Resident) {
        auto base = mips_.size() - 1;
        while (base != 0 && mips_[base - 1].state == State::Resident) {
            --base;
        }
        next = PublishedMipView{
            static_cast<std::uint32_t>(base),
            static_cast<std::uint32_t>(mips_.size() - base), 0};
    }
    const auto changed = next.has_value() != published_.has_value() ||
        (next.has_value() &&
            (next->baseMip != published_->baseMip ||
             next->levelCount != published_->levelCount));
    if (changed) {
        ++revision_;
        if (next) next->revision = revision_;
        published_ = next;
    }
    return ResidencyError::None;
}

std::optional<PublishedMipView> TextureResidency::Published() const noexcept
{
    return published_;
}

DescriptorQuarantine::DescriptorQuarantine(const std::uint32_t capacity)
{
    if (capacity > 1) {
        free_.reserve(capacity - 1);
        for (auto index = capacity - 1; index != 0; --index) {
            free_.push_back(index);
        }
    }
}

std::optional<std::uint32_t> DescriptorQuarantine::Acquire() noexcept
{
    if (free_.empty()) return std::nullopt;
    const auto index = free_.back();
    free_.pop_back();
    live_.push_back(index);
    return index;
}

bool DescriptorQuarantine::Retire(
    const std::uint32_t index,
    const std::uint64_t completionValue) noexcept
{
    if (index == 0) return false;
    const auto found = std::find(live_.begin(), live_.end(), index);
    if (found == live_.end()) return false;
    live_.erase(found);
    retired_.push_back({index, completionValue});
    return true;
}

bool DescriptorQuarantine::Release(const std::uint32_t index) noexcept
{
    if (index == 0) return false;
    const auto found = std::find(live_.begin(), live_.end(), index);
    if (found == live_.end()) return false;
    live_.erase(found);
    free_.push_back(index);
    std::sort(free_.begin(), free_.end(), std::greater<>{});
    return true;
}

void DescriptorQuarantine::Advance(const std::uint64_t completedValue) noexcept
{
    for (auto iterator = retired_.begin(); iterator != retired_.end();) {
        if (iterator->completion <= completedValue) {
            free_.push_back(iterator->index);
            iterator = retired_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    std::sort(free_.begin(), free_.end(), std::greater<>{});
}

bool DescriptorQuarantine::IsQuarantined(
    const std::uint32_t index) const noexcept
{
    return std::any_of(retired_.begin(), retired_.end(),
        [index](const Retired& value) { return value.index == index; });
}

}
