#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include "bitvm/buses.hpp"
#include "bitvm/proto/graphdef.hpp"
#include "bitvm/proto/messages.hpp"

namespace bitvm
{

/// @brief validated graph with resolved instructions and flat storage offsets.
class CompiledGraph {
public:
    [[nodiscard]] static std::expected<std::shared_ptr<const CompiledGraph>, proto::GraphError>
    compile (const proto::GraphDef& graph);

    [[nodiscard]] const proto::GraphDef& graph () const noexcept;

private:
    friend class GraphInstance;

    enum class Kind : std::uint8_t {
        add,
        sub,
        mul,
        div,
        sin_osc,
        env_perc,
        env_asr,
        out,
        in
    };

    struct Input {
        proto::InputSource source;
        proto::Rate        read;
        std::uint32_t      index;
        bool               audio_source;
    };

    struct Unit {
        Kind          kind;
        proto::Rate   rate;
        std::uint32_t input_offset;
        std::uint32_t output_offset;
        std::uint16_t input_count;
        std::uint8_t  output_count;
    };

    proto::GraphDef    graph_;
    std::vector<Input> inputs_;
    std::vector<Unit>  units_;
    std::uint32_t      output_count_ = 0;
};

/// @brief all mutable graph storage, allocated on the control thread.
/// @details process one active frame at a time; omitting calls freezes a paused node.
class GraphInstance {
public:
    GraphInstance (
        std::shared_ptr<const CompiledGraph> graph,
        std::uint32_t                        sample_rate,
        std::uint16_t                        block_size,
        std::span<const proto::Pair>         params = {}
    );

    [[nodiscard]] std::uint16_t param_count () const noexcept;
    void set_param (std::uint16_t index, float value, std::uint64_t ramp_frames) noexcept;

    /// @brief renders one frame at its global block offset; true requests node removal.
    [[nodiscard]] bool process (Buses& buses, std::uint16_t offset) noexcept;

private:
    struct Parameter {
        double        value     = 0;
        double        target    = 0;
        double        step      = 0;
        std::uint64_t remaining = 0;
    };

    enum class Stage : std::uint8_t {
        attack,
        sustain,
        release,
        finished
    };

    struct State {
        double        phase           = 0;
        double        frequency       = 0;
        double        value           = 0;
        double        segment_start   = 0;
        double        segment_elapsed = 0;
        std::uint64_t last_frame      = 0;
        Stage         stage           = Stage::attack;
        bool          initialized     = false;
        bool          gate            = false;
        bool          done            = false;
    };

    void prepare_inputs (std::size_t index) noexcept;
    void evaluate (std::size_t index, Buses* buses, std::uint16_t offset) noexcept;

    std::shared_ptr<const CompiledGraph> graph_;
    std::uint32_t                        sample_rate_;
    std::uint16_t                        block_size_;
    std::vector<Parameter>               params_;
    std::vector<float>                   first_params_;
    std::vector<float>                   inputs_;
    std::vector<float>                   outputs_;
    std::vector<float>                   first_outputs_;
    std::vector<State>                   states_;
    std::uint64_t                        active_frames_    = 0;
    std::uint64_t                        block_generation_ = 0;
};

}  // namespace bitvm
