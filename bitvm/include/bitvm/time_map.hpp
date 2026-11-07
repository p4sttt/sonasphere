#pragma once

#include <cstdint>

namespace bitvm
{

/// rational frame clock. events round up so a note never precedes its requested time.
class TimeMap {
public:
    explicit TimeMap (std::uint32_t sample_rate, std::int64_t origin_ns = 0);
    std::uint64_t frame_at (std::int64_t time_ns) const noexcept;
    std::int64_t  ns_at (std::uint64_t frame) const noexcept;
    /// smooth a backend observation by 1/16 of its phase error (offline never calls this).
    void observe (std::uint64_t frame, std::int64_t time_ns) noexcept;

private:
    std::uint32_t sample_rate_;
    std::int64_t  origin_ns_;
};

}  // namespace bitvm
