#include "bitvm/backend.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace bitvm
{
namespace
{

void
put_u16 (std::span<char> bytes, std::size_t offset, std::uint16_t value) {
    for (unsigned i = 0; i < 2; ++i) {
        bytes[offset + i] = static_cast<char> ((value >> (8 * i)) & 0xffU);
    }
}

void
put_u32 (std::span<char> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<char> ((value >> (8 * i)) & 0xffU);
    }
}

std::unexpected<BackendError>
error (std::string message) {
    return std::unexpected (BackendError{ std::move (message) });
}

}  // namespace

OfflineBackend::OfflineBackend (const EngineConfig& config) :
    config_{ config.sample_rate, config.out_channels, config.block_size } {}

OfflineBackend::OfflineBackend (const EngineConfig& config, std::filesystem::path output) :
    OfflineBackend (config) {
    output_      = std::move (output);
    file_output_ = true;
}

const AudioOutputConfig&
OfflineBackend::output_config () const noexcept {
    return config_;
}

const std::vector<float>&
OfflineBackend::samples () const noexcept {
    return samples_;
}

std::uint64_t
OfflineBackend::frames () const noexcept {
    return frames_;
}

std::expected<void, BackendError>
OfflineBackend::open_file () {
    if (file_.is_open()) {
        if (!file_) {
            return error ("wave output is in a failed state");
        }
        return {};
    }
    file_.open (output_, std::ios::binary | std::ios::trunc);
    if (!file_) {
        return error ("cannot open wave output: " + output_.string());
    }
    return write_header();
}

std::expected<void, BackendError>
OfflineBackend::write_header () {
    const auto alignment = static_cast<std::uint32_t> (config_.channels) * sizeof (float);
    const auto byte_rate = static_cast<std::uint64_t> (config_.sample_rate) * alignment;
    const auto data_size = frames_ * alignment;
    if (alignment > std::numeric_limits<std::uint16_t>::max() ||
        byte_rate > std::numeric_limits<std::uint32_t>::max() ||
        data_size > std::numeric_limits<std::uint32_t>::max() - 36U) {
        return error ("wave output exceeds riff format limits");
    }
    std::array<char, 44>   bytes{};
    const std::string_view riff = "RIFF";
    const std::string_view wave = "WAVEfmt ";
    const std::string_view data = "data";
    std::copy (riff.begin(), riff.end(), bytes.begin());
    std::copy (wave.begin(), wave.end(), bytes.begin() + 8);
    std::copy (data.begin(), data.end(), bytes.begin() + 36);
    put_u32 (bytes, 4, static_cast<std::uint32_t> (data_size) + 36);
    put_u32 (bytes, 16, 16);
    put_u16 (bytes, 20, 3);
    put_u16 (bytes, 22, config_.channels);
    put_u32 (bytes, 24, config_.sample_rate);
    put_u32 (bytes, 28, static_cast<std::uint32_t> (byte_rate));
    put_u16 (bytes, 32, static_cast<std::uint16_t> (alignment));
    put_u16 (bytes, 34, 32);
    put_u32 (bytes, 40, static_cast<std::uint32_t> (data_size));
    file_.seekp (0);
    file_.write (bytes.data(), static_cast<std::streamsize> (bytes.size()));
    if (!file_) {
        return error ("cannot write wave header: " + output_.string());
    }
    return {};
}

std::expected<void, BackendError>
OfflineBackend::write (std::span<const float> samples) {
    if (finished_) {
        return error ("audio output has already finished");
    }
    if (config_.channels == 0 || config_.sample_rate == 0 ||
        samples.size() % config_.channels != 0) {
        return error ("invalid audio output configuration or sample count");
    }
    const auto count = samples.size() / config_.channels;
    if (count > std::numeric_limits<std::uint64_t>::max() - frames_) {
        return error ("audio frame count overflow");
    }
    if (!file_output_) {
        samples_.insert (samples_.end(), samples.begin(), samples.end());
    } else {
        const auto max_samples =
            (std::numeric_limits<std::uint32_t>::max() - 36ULL) / sizeof (float);
        if (frames_ > max_samples / config_.channels ||
            count > max_samples / config_.channels - frames_) {
            return error ("wave output exceeds riff size limit");
        }
        if (auto opened = open_file(); !opened) {
            return opened;
        }
        std::array<char, 4096> bytes{};
        for (std::size_t offset = 0; offset < samples.size();) {
            const auto length = std::min (samples.size() - offset, bytes.size() / sizeof (float));
            for (std::size_t i = 0; i < length; ++i) {
                put_u32 (
                    bytes,
                    i * sizeof (float),
                    std::bit_cast<std::uint32_t> (samples[offset + i])
                );
            }
            file_.write (bytes.data(), static_cast<std::streamsize> (length * sizeof (float)));
            if (!file_) {
                return error ("cannot write wave audio: " + output_.string());
            }
            offset += length;
        }
    }
    frames_ += count;
    return {};
}

std::expected<void, BackendError>
OfflineBackend::finish () {
    if (finished_) {
        return {};
    }
    if (config_.sample_rate == 0 || config_.channels == 0) {
        return error ("invalid audio output configuration");
    }
    if (file_output_) {
        if (auto opened = open_file(); !opened) {
            return opened;
        }
        if (auto header = write_header(); !header) {
            return header;
        }
        file_.flush();
        if (!file_) {
            return error ("cannot flush wave output: " + output_.string());
        }
        file_.close();
        if (file_.fail()) {
            return error ("cannot close wave output: " + output_.string());
        }
    }
    finished_ = true;
    return {};
}

}  // namespace bitvm
