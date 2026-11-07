#include "bitvm/buses.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bitvm
{

Buses::Buses (std::uint32_t audio_count, std::uint32_t control_count, std::uint16_t block_size) :
    audio_count_ (audio_count),
    block_size_ (block_size),
    audio_ (static_cast<std::size_t> (audio_count) * block_size),
    control_ (control_count) {}

void
Buses::clear_audio () noexcept {
    std::fill (audio_.begin(), audio_.end(), 0.0F);
    ++block_generation_;
}

float
Buses::read_audio (std::uint32_t bus, std::uint16_t offset) const noexcept {
    if (bus >= audio_count_ || offset >= block_size_) {
        return 0.0F;
    }
    return audio_[static_cast<std::size_t> (bus) * block_size_ + offset];
}

void
Buses::add_audio (std::uint32_t bus, std::uint16_t offset, float value) noexcept {
    if (bus >= audio_count_ || offset >= block_size_ || !std::isfinite (value)) {
        return;
    }
    auto&            sample = audio_[static_cast<std::size_t> (bus) * block_size_ + offset];
    constexpr double kLimit = std::numeric_limits<float>::max();
    sample                  = static_cast<float> (
        std::clamp (static_cast<double> (sample) + static_cast<double> (value), -kLimit, kLimit)
    );
}

float
Buses::read_control (std::uint32_t bus) const noexcept {
    return bus < control_.size() ? control_[bus] : 0.0F;
}

void
Buses::write_control (std::uint32_t bus, float value) noexcept {
    if (bus < control_.size() && std::isfinite (value)) {
        control_[bus] = value;
    }
}

std::uint32_t
Buses::audio_count () const noexcept {
    return audio_count_;
}

std::uint32_t
Buses::control_count () const noexcept {
    return static_cast<std::uint32_t> (control_.size());
}

std::uint64_t
Buses::block_generation () const noexcept {
    return block_generation_;
}

}  // namespace bitvm
