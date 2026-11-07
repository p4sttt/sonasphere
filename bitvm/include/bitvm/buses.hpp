#pragma once

#include <cstdint>
#include <vector>

namespace bitvm
{

/// @brief preallocated audio blocks and persistent control buses.
class Buses {
public:
    Buses (std::uint32_t audio_count, std::uint32_t control_count, std::uint16_t block_size);

    void                clear_audio () noexcept;
    [[nodiscard]] float read_audio (std::uint32_t bus, std::uint16_t offset) const noexcept;
    void                add_audio (std::uint32_t bus, std::uint16_t offset, float value) noexcept;
    [[nodiscard]] float read_control (std::uint32_t bus) const noexcept;
    void                write_control (std::uint32_t bus, float value) noexcept;

    [[nodiscard]] std::uint32_t audio_count () const noexcept;
    [[nodiscard]] std::uint32_t control_count () const noexcept;
    /// @brief advances when clear_audio starts a new block, including while a node is paused.
    [[nodiscard]] std::uint64_t block_generation () const noexcept;

private:
    std::uint32_t      audio_count_;
    std::uint16_t      block_size_;
    std::vector<float> audio_;
    std::vector<float> control_;
    std::uint64_t      block_generation_ = 0;
};

}  // namespace bitvm
