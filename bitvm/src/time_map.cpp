#include "bitvm/time_map.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace bitvm
{
namespace
{
constexpr std::uint64_t kSecond  = 1000000000;
constexpr auto          kMaxTime = std::numeric_limits<std::int64_t>::max();
}  // namespace

TimeMap::TimeMap (std::uint32_t sample_rate, std::int64_t origin_ns) :
    sample_rate_ (sample_rate), origin_ns_ (origin_ns) {
    if (sample_rate == 0 || origin_ns < 0) {
        throw std::invalid_argument ("invalid frame clock");
    }
}

std::uint64_t
TimeMap::frame_at (std::int64_t time_ns) const noexcept {
    if (time_ns <= origin_ns_) {
        return 0;
    }
    const auto     delta    = static_cast<std::uint64_t> (time_ns - origin_ns_);
    const auto     seconds  = delta / kSecond;
    const auto     fraction = ((delta % kSecond) * sample_rate_ + kSecond - 1) / kSecond;
    constexpr auto maximum  = std::numeric_limits<std::uint64_t>::max();
    if (seconds > (maximum - fraction) / sample_rate_) {
        return maximum;
    }
    return seconds * sample_rate_ + fraction;
}

std::int64_t
TimeMap::ns_at (std::uint64_t frame) const noexcept {
    const auto seconds   = frame / sample_rate_;
    const auto fraction  = (frame % sample_rate_) * kSecond / sample_rate_;
    const auto available = static_cast<std::uint64_t> (kMaxTime - origin_ns_);
    if (fraction > available || seconds > (available - fraction) / kSecond) {
        return kMaxTime;
    }
    return origin_ns_ + static_cast<std::int64_t> (seconds * kSecond + fraction);
}

void
TimeMap::observe (std::uint64_t frame, std::int64_t time_ns) noexcept {
    if (time_ns < 0) {
        return;
    }
    const auto predicted  = ns_at (frame);
    const auto correction = (time_ns - predicted) / 16;
    if (correction > 0) {
        origin_ns_ += std::min (correction, kMaxTime - origin_ns_);
    } else {
        origin_ns_ = std::max<std::int64_t> (0, origin_ns_ + correction);
    }
}

}  // namespace bitvm
