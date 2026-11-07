#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitvm/backend.hpp"
#include "bitvm/engine.hpp"
#include "bitvm/proto/messages.hpp"

namespace bitvm
{

struct Score {
    std::vector<proto::Message> messages;
    std::filesystem::path       directory;
};

struct ScoreError {
    std::string message;
    std::size_t message_index = 0;
};

struct RenderOptions {
    std::optional<double> duration_seconds;
    double                tail_seconds = 1.0;
};

struct RenderResult {
    std::uint64_t frames = 0;
};

inline constexpr double kMaxRenderSeconds = 86400.0;

/// source_path supplies the directory for graph files and buffer loads.
[[nodiscard]] std::expected<Score, ScoreError>
parse_score (std::string_view text, const std::filesystem::path& source_path = {});
[[nodiscard]] std::expected<Score, ScoreError> decode_score (std::span<const std::byte> bytes);
[[nodiscard]] std::expected<std::vector<std::byte>, ScoreError> encode_score (const Score& score);
[[nodiscard]] std::expected<Score, ScoreError> read_score (const std::filesystem::path& path);

/// runs on a quiescent engine starting at frame zero; audio processing never performs i/o.
/// duration must include every bundle; omitted duration adds tail_seconds after the last one.
/// seconds round to the nearest nanosecond, then up to the first frame at or after that time.
[[nodiscard]] std::expected<RenderResult, ScoreError> render_score (
    Engine&       engine,
    const Score&  score,
    AudioBackend& backend,
    RenderOptions options = {}
);

}  // namespace bitvm
