#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "bitvm/engine.hpp"

namespace bitvm
{

struct AudioOutputConfig {
    std::uint32_t sample_rate = 48000;
    std::uint16_t channels    = 2;
    std::uint16_t block_size  = 64;
};

struct BackendError {
    std::string message;
};

/// output pulled by a non-rt render driver; write and finish may allocate and perform i/o.
class AudioBackend {
public:
    virtual ~AudioBackend ()                                                         = default;
    virtual const AudioOutputConfig&          output_config () const noexcept        = 0;
    virtual std::expected<void, BackendError> write (std::span<const float> samples) = 0;
    virtual std::expected<void, BackendError> finish ()                              = 0;
};

/// stores interleaved samples in memory, or streams ieee float32 riff/wave to a file.
class OfflineBackend final : public AudioBackend {
public:
    explicit OfflineBackend (const EngineConfig& config);
    OfflineBackend (const EngineConfig& config, std::filesystem::path output);
    const AudioOutputConfig&          output_config () const noexcept override;
    std::expected<void, BackendError> write (std::span<const float> samples) override;
    std::expected<void, BackendError> finish () override;
    const std::vector<float>&         samples () const noexcept;
    std::uint64_t                     frames () const noexcept;

private:
    std::expected<void, BackendError> open_file ();
    std::expected<void, BackendError> write_header ();
    AudioOutputConfig                 config_;
    std::filesystem::path             output_;
    std::ofstream                     file_;
    std::vector<float>                samples_;
    std::uint64_t                     frames_      = 0;
    bool                              file_output_ = false;
    bool                              finished_    = false;
};

}  // namespace bitvm
