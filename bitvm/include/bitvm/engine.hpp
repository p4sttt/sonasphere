#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

#include "bitvm/proto/graphdef.hpp"
#include "bitvm/proto/messages.hpp"

namespace bitvm
{

struct EngineConfig {
    std::uint32_t sample_rate       = 48000;
    std::uint16_t block_size        = 64;
    std::uint16_t out_channels      = 2;
    std::uint32_t audio_buses       = 128;
    std::uint32_t control_buses     = 128;
    std::uint32_t max_nodes         = 1024;
    std::uint32_t queue_capacity    = 256;
    std::uint32_t max_buffers       = 64;
    std::uint64_t buffer_bytes      = 64 * 1024 * 1024;
    std::int64_t  late_tolerance_ns = 50000000;
};

struct EngineError {
    proto::ErrorCode code = proto::ErrorCode::internal;
    std::string      message;
    std::uint16_t    index = proto::kNoCommand;
};

/// fixed-size notification crossing from audio to control; converted to protocol events there.
struct Notification {
    enum class Kind : std::uint8_t {
        n_end,
        error,
        late
    };
    Kind             kind    = Kind::n_end;
    std::uint8_t     client  = 0;
    proto::NodeId    id      = 0;
    std::uint32_t    seq     = 0;
    std::uint16_t    index   = proto::kNoCommand;
    proto::Opcode    opcode  = proto::Opcode::node_new;
    proto::ErrorCode code    = proto::ErrorCode::ok;
    std::int64_t     late_ns = 0;
    bool             played  = false;
};

/// prepares on one control thread and renders on one audio thread; no shared caller locks.
/// call(), tree() and status() require a stopped audio thread in m2. submit(), collect() and
/// pop_notification() may run concurrently with process(). construction validates config.
class Engine {
public:
    explicit Engine (EngineConfig config = {});
    ~Engine ();
    Engine (const Engine&)            = delete;
    Engine& operator= (const Engine&) = delete;

    const EngineConfig& config () const noexcept;
    std::expected<proto::CallResult, EngineError>
    call (std::uint8_t client, const proto::CallCommand& command);
    std::expected<void, EngineError>
    submit (std::uint8_t client, std::uint32_t seq, const proto::Bundle& bundle);
    /// interleaved output; size must be frames * out_channels; arbitrary callback sizes work.
    void process (std::span<float> output, std::uint32_t frames) noexcept;
    /// drains deferred deletion; only the control thread destroys prepared bundles and nodes.
    void              collect ();
    bool              pop_notification (Notification& notification) noexcept;
    std::uint64_t     frame () const noexcept;
    proto::StatusInfo status () const;
    std::expected<proto::TreeInfo, EngineError>
                           tree (proto::NodeId root = 0, std::uint16_t depth = 0) const;
    const proto::GraphDef* graph_definition (std::uint32_t def) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bitvm
