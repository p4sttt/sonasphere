#include "bitvm/graph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>

#include "bitvm/proto/catalog.hpp"

namespace bitvm
{
namespace
{

float
finite_sample (double value) noexcept {
    constexpr double kLimit = std::numeric_limits<float>::max();
    if (std::isnan (value)) {
        return 0.0F;
    }
    return static_cast<float> (std::clamp (value, -kLimit, kLimit));
}

double
duration_frames (float seconds, std::uint32_t sample_rate) noexcept {
    return std::round (std::max (0.0, static_cast<double> (seconds)) * sample_rate);
}

double
curved_fraction (double position, double curve) noexcept {
    if (position <= 0) {
        return 0;
    }
    if (position >= 1) {
        return 1;
    }
    if (curve == 0) {
        return position;
    }
    if (curve < 0) {
        return std::expm1 (curve * position) / std::expm1 (curve);
    }
    // factoring out exp(curve) avoids overflow for arbitrarily large positive curves.
    return std::exp (curve * (position - 1)) * (-std::expm1 (-curve * position)) /
           (-std::expm1 (-curve));
}

std::optional<std::uint32_t>
bus_index (float value, std::uint32_t channel, std::uint32_t count) noexcept {
    if (!std::isfinite (value) || value < 0) {
        return std::nullopt;
    }
    const double rounded = std::round (static_cast<double> (value));
    if (rounded >= count) {
        return std::nullopt;
    }
    const auto index = static_cast<std::uint64_t> (rounded) + channel;
    if (index >= count) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t> (index);
}

}  // namespace

std::expected<std::shared_ptr<const CompiledGraph>, proto::GraphError>
CompiledGraph::compile (const proto::GraphDef& graph) {
    if (auto result = proto::validate (graph); !result) {
        return std::unexpected (result.error());
    }
    static constexpr std::array<std::pair<std::string_view, Kind>, 9> kKinds = {
        std::pair{ "add", Kind::add },
        std::pair{ "sub", Kind::sub },
        std::pair{ "mul", Kind::mul },
        std::pair{ "div", Kind::div },
        std::pair{ "sin_osc", Kind::sin_osc },
        std::pair{ "env_perc", Kind::env_perc },
        std::pair{ "env_asr", Kind::env_asr },
        std::pair{ "out", Kind::out },
        std::pair{ "in", Kind::in }
    };
    const auto resolve = [] (std::string_view name) -> std::optional<Kind> {
        for (const auto& [candidate, kind] : kKinds) {
            if (candidate == name) {
                return kind;
            }
        }
        return std::nullopt;
    };
    for (const auto& entry : proto::catalog()) {
        if (!resolve (entry.name)) {
            return std::unexpected (proto::GraphError{ "catalog unit has no dsp implementation" });
        }
    }

    auto result    = std::shared_ptr<CompiledGraph> (new CompiledGraph);
    result->graph_ = graph;
    result->units_.reserve (graph.units.size());
    for (const auto& unit : graph.units) {
        const auto* spec = proto::find_unit (graph.types[unit.type]);
        Unit        compiled{ *resolve (spec->name),
                              unit.rate,
                              static_cast<std::uint32_t> (result->inputs_.size()),
                              result->output_count_,
                              static_cast<std::uint16_t> (unit.inputs.size()),
                              unit.n_out };
        for (std::size_t index = 0; index < unit.inputs.size(); ++index) {
            const auto& input = unit.inputs[index];
            const auto  mode =
                index < spec->inputs.size() ? spec->inputs[index].read : spec->variadic->read;
            const auto    read         = proto::effective_read (mode, unit.rate);
            std::uint32_t source_index = input.index;
            bool          audio_source = false;
            if (input.src == proto::InputSource::unit) {
                const auto& source = result->units_[input.index];
                source_index       = source.output_offset + input.output;
                audio_source       = source.rate == proto::Rate::ar;
            }
            result->inputs_.push_back ({ input.src, read, source_index, audio_source });
        }
        result->output_count_ += unit.n_out;
        result->units_.push_back (compiled);
    }
    return std::shared_ptr<const CompiledGraph> (std::move (result));
}

const proto::GraphDef&
CompiledGraph::graph () const noexcept {
    return graph_;
}

GraphInstance::GraphInstance (
    std::shared_ptr<const CompiledGraph> graph,
    std::uint32_t                        sample_rate,
    std::uint16_t                        block_size,
    std::span<const proto::Pair>         params
) :
    graph_ (std::move (graph)),
    sample_rate_ (std::max (sample_rate, 1U)),
    block_size_ (std::max<std::uint16_t> (block_size, 1)),
    params_ (graph_->graph_.params.size()),
    first_params_ (params_.size()),
    inputs_ (graph_->inputs_.size()),
    outputs_ (graph_->output_count_),
    first_outputs_ (outputs_.size()),
    states_ (graph_->units_.size()) {
    for (std::size_t index = 0; index < params_.size(); ++index) {
        params_[index].value = graph_->graph_.params[index].default_value;
    }
    for (const auto& param : params) {
        set_param (param.index, param.value, 0);
    }
    for (std::size_t index = 0; index < params_.size(); ++index) {
        first_params_[index] = finite_sample (params_[index].value);
    }
    for (std::size_t index = 0; index < graph_->units_.size(); ++index) {
        const auto& unit = graph_->units_[index];
        for (std::uint16_t item = 0; item < unit.input_count; ++item) {
            const auto  flat  = unit.input_offset + item;
            const auto& input = graph_->inputs_[flat];
            switch (input.source) {
                case proto::InputSource::constant: {
                    inputs_[flat] = graph_->graph_.consts[input.index];
                    break;
                }
                case proto::InputSource::param: {
                    inputs_[flat] = first_params_[input.index];
                    break;
                }
                case proto::InputSource::unit: {
                    inputs_[flat] = outputs_[input.index];
                    break;
                }
            }
        }
        if (unit.rate == proto::Rate::ir) {
            evaluate (index, nullptr, 0);
        }
    }
}

std::uint16_t
GraphInstance::param_count () const noexcept {
    return static_cast<std::uint16_t> (params_.size());
}

void
GraphInstance::set_param (std::uint16_t index, float value, std::uint64_t ramp_frames) noexcept {
    if (index >= params_.size() || !std::isfinite (value)) {
        return;
    }
    auto& param     = params_[index];
    param.target    = value;
    param.remaining = ramp_frames;
    if (ramp_frames == 0) {
        param.value = value;
        param.step  = 0;
    } else {
        param.step =
            (static_cast<double> (value) - param.value) / static_cast<double> (ramp_frames);
    }
}

void
GraphInstance::prepare_inputs (std::size_t index) noexcept {
    const auto& unit  = graph_->units_[index];
    const auto& state = states_[index];
    for (std::uint16_t item = 0; item < unit.input_count; ++item) {
        const auto  flat  = unit.input_offset + item;
        const auto& input = graph_->inputs_[flat];
        if (input.read == proto::Rate::ir) {
            // direct parameters were latched in the constructor; unit sources become ready
            // in topological order at the first active frame.
            if (!state.initialized && input.source == proto::InputSource::unit) {
                inputs_[flat] = outputs_[input.index];
            }
            continue;
        }
        switch (input.source) {
            case proto::InputSource::constant: {
                break;
            }
            case proto::InputSource::param: {
                inputs_[flat] = first_params_[input.index];
                break;
            }
            case proto::InputSource::unit: {
                inputs_[flat] = input.read == proto::Rate::kr && input.audio_source ?
                                    first_outputs_[input.index] :
                                    outputs_[input.index];
                break;
            }
        }
    }
}

void
GraphInstance::evaluate (std::size_t index, Buses* buses, std::uint16_t offset) noexcept {
    const auto&  unit  = graph_->units_[index];
    auto&        state = states_[index];
    const auto*  args  = inputs_.data() + unit.input_offset;
    const double delta =
        state.initialized ? static_cast<double> (active_frames_ - state.last_frame) : 0;
    using Kind = CompiledGraph::Kind;
    switch (unit.kind) {
        case Kind::add: {
            outputs_[unit.output_offset] =
                finite_sample (static_cast<double> (args[0]) + static_cast<double> (args[1]));
            break;
        }
        case Kind::sub: {
            outputs_[unit.output_offset] =
                finite_sample (static_cast<double> (args[0]) - static_cast<double> (args[1]));
            break;
        }
        case Kind::mul: {
            outputs_[unit.output_offset] =
                finite_sample (static_cast<double> (args[0]) * static_cast<double> (args[1]));
            break;
        }
        case Kind::div: {
            outputs_[unit.output_offset] =
                args[1] == 0 ?
                    0.0F :
                    finite_sample (static_cast<double> (args[0]) / static_cast<double> (args[1]));
            break;
        }
        case Kind::sin_osc: {
            if (!state.initialized) {
                state.phase = args[1];
            } else {
                state.phase += std::fmod (state.frequency / sample_rate_ * delta, 1.0);
            }
            state.phase -= std::floor (state.phase);
            outputs_[unit.output_offset] =
                finite_sample (std::sin (2 * std::numbers::pi * state.phase));
            state.frequency = args[0];
            break;
        }
        case Kind::env_perc: {
            const double attack  = duration_frames (args[0], sample_rate_);
            const double release = duration_frames (args[1], sample_rate_);
            const double frame   = static_cast<double> (active_frames_);
            double       value   = 0;
            if (frame < attack) {
                value = static_cast<double> (args[2]) * curved_fraction (frame / attack, args[3]);
            } else if (frame < attack + release) {
                value = static_cast<double> (args[2]) *
                        (1 - curved_fraction ((frame - attack) / release, args[3]));
            } else {
                state.done = args[4] == 1.0F;
            }
            outputs_[unit.output_offset] = finite_sample (value);
            break;
        }
        case Kind::env_asr: {
            const double attack  = duration_frames (args[1], sample_rate_);
            const double release = duration_frames (args[3], sample_rate_);
            const bool   gate    = args[0] > 0;
            if (!state.initialized) {
                state.gate  = gate;
                state.stage = gate ? Stage::attack : Stage::finished;
            }
            state.segment_elapsed += delta;
            if (state.stage == Stage::attack) {
                if (attack == 0 || state.segment_elapsed >= attack) {
                    state.value = args[2];
                    state.stage = Stage::sustain;
                } else {
                    state.value = state.segment_start +
                                  (static_cast<double> (args[2]) - state.segment_start) *
                                      state.segment_elapsed / attack;
                }
            } else if (state.stage == Stage::release) {
                if (release == 0 || state.segment_elapsed >= release) {
                    state.value = 0;
                    state.stage = Stage::finished;
                } else {
                    state.value = state.segment_start * (1 - state.segment_elapsed / release);
                }
            }
            if (gate != state.gate) {
                state.gate            = gate;
                state.stage           = gate ? Stage::attack : Stage::release;
                state.segment_start   = state.value;
                state.segment_elapsed = 0;
                if (gate && attack == 0) {
                    state.value = args[2];
                    state.stage = Stage::sustain;
                } else if (!gate && release == 0) {
                    state.value = 0;
                    state.stage = Stage::finished;
                }
            }
            state.done                   = state.stage == Stage::finished && args[4] == 1.0F;
            outputs_[unit.output_offset] = finite_sample (state.value);
            break;
        }
        case Kind::out: {
            const bool audio = unit.rate == proto::Rate::ar;
            const auto count = audio ? buses->audio_count() : buses->control_count();
            for (std::uint16_t channel = 0; channel < unit.input_count - 1; ++channel) {
                if (auto bus = bus_index (args[0], channel, count)) {
                    if (audio) {
                        buses->add_audio (*bus, offset, args[channel + 1]);
                    } else {
                        buses->write_control (*bus, args[channel + 1]);
                    }
                }
            }
            break;
        }
        case Kind::in: {
            const bool audio = unit.rate == proto::Rate::ar;
            const auto count = audio ? buses->audio_count() : buses->control_count();
            for (std::uint8_t channel = 0; channel < unit.output_count; ++channel) {
                const auto bus                         = bus_index (args[0], channel, count);
                outputs_[unit.output_offset + channel] = !bus  ? 0.0F :
                                                         audio ? buses->read_audio (*bus, offset) :
                                                                 buses->read_control (*bus);
            }
            break;
        }
    }
    state.initialized = true;
    state.last_frame  = active_frames_;
}

bool
GraphInstance::process (Buses& buses, std::uint16_t offset) noexcept {
    if (offset >= block_size_) {
        return false;
    }
    for (auto& param : params_) {
        if (param.remaining != 0) {
            --param.remaining;
            param.value = param.remaining == 0 ? param.target : param.value + param.step;
        }
    }
    const bool first =
        active_frames_ == 0 || offset == 0 || block_generation_ != buses.block_generation();
    block_generation_ = buses.block_generation();
    if (first) {
        for (std::size_t index = 0; index < params_.size(); ++index) {
            first_params_[index] = finite_sample (params_[index].value);
        }
    }
    bool done = false;
    for (std::size_t index = 0; index < graph_->units_.size(); ++index) {
        const auto& unit = graph_->units_[index];
        if (unit.rate == proto::Rate::ar || (unit.rate == proto::Rate::kr && first)) {
            prepare_inputs (index);
            evaluate (index, &buses, offset);
        }
        if (first) {
            for (std::uint8_t output = 0; output < unit.output_count; ++output) {
                first_outputs_[unit.output_offset + output] = outputs_[unit.output_offset + output];
            }
        }
        done = done || states_[index].done;
    }
    ++active_frames_;
    return done;
}

}  // namespace bitvm
