#include "bitvm/engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef BITVM_HAVE_SNDFILE
    #include <sndfile.h>
#endif

#include "bitvm/buses.hpp"
#include "bitvm/graph.hpp"
#include "bitvm/proto/codec.hpp"
#include "bitvm/proto/validate.hpp"
#include "bitvm/spsc.hpp"
#include "bitvm/time_map.hpp"
#include "node_tree.hpp"

namespace bitvm
{
namespace
{
using detail::Node;
using detail::NodeTree;
using proto::ErrorCode;
using proto::NodeId;

std::unexpected<EngineError>
fail (ErrorCode code, std::string message, std::uint16_t index = proto::kNoCommand) {
    return std::unexpected (EngineError{ code, std::move (message), index });
}

EngineConfig
checked_config (EngineConfig config) {
    if (config.sample_rate == 0 || config.sample_rate > 768000 || config.block_size == 0 ||
        config.out_channels == 0 || config.out_channels > 64 ||
        config.audio_buses < config.out_channels || config.audio_buses >= (1U << 24) ||
        config.control_buses >= (1U << 24) || config.max_nodes == 0 || config.max_nodes > 65536 ||
        config.queue_capacity == 0 || config.queue_capacity > 65536 || config.max_buffers > 65536 ||
        config.late_tolerance_ns < 0) {
        throw std::invalid_argument ("invalid engine configuration");
    }
    return config;
}

struct PreparedCommand {
    proto::RtCommand      command;
    std::unique_ptr<Node> node;
};
struct PreparedBundle {
    std::uint8_t                 client         = 0;
    std::uint32_t                seq            = 0;
    std::int64_t                 time           = 0;
    std::uint64_t                target         = 0;
    std::int64_t                 effective_time = 0;
    std::uint64_t                order          = 0;
    std::vector<PreparedCommand> commands;
    std::vector<NodeId>          created;
};
struct Later {
    bool
    operator() (const PreparedBundle* lhs, const PreparedBundle* rhs) const noexcept {
        if (lhs->target != rhs->target) {
            return lhs->target > rhs->target;
        }
        if (lhs->effective_time != rhs->effective_time) {
            return lhs->effective_time > rhs->effective_time;
        }
        return lhs->order > rhs->order;
    }
};
struct Definition {
    std::shared_ptr<const CompiledGraph> graph;
    std::uint8_t                         owner = 0;
};
struct Allocation {
    std::uint32_t count = 0;
    std::uint8_t  owner = 0;
};
struct Buffer {
    std::vector<float> samples;
    std::uint8_t       owner = 0;
};
}  // namespace

struct Engine::Impl {
    explicit Impl (EngineConfig value) :
        config (checked_config (value)),
        buses (config.audio_buses, config.control_buses, config.block_size),
        clock (config.sample_rate),
        node_tree (config.max_nodes),
        incoming (config.queue_capacity),
        completed (config.queue_capacity),
        retired (config.max_nodes),
        notifications (config.max_nodes + config.queue_capacity * 2),
        audio_alloc (config.audio_buses),
        control_alloc (config.control_buses),
        buffers (config.max_buffers) {
        scheduled.reserve (config.queue_capacity);
        for (std::uint32_t i = 0; i < config.out_channels; ++i) {
            audio_alloc[i] = { 1, 255 };
        }
    }

    ~Impl () {
        node_tree.clear (&node_tree.root(), { nullptr, [] (void*, Node* node) noexcept {
                                                 delete node;
                                             } });
        Node*           node   = nullptr;
        PreparedBundle* bundle = nullptr;
        while (incoming.try_pop (bundle)) {
            delete bundle;
        }
        for (auto* pending : scheduled) {
            delete pending;
        }
        while (completed.try_pop (bundle)) {
            delete bundle;
        }
        while (retired.try_pop (node)) {
            delete node;
        }
    }

    EngineConfig                                  config;
    Buses                                         buses;
    TimeMap                                       clock;
    NodeTree                                      node_tree;
    SpscQueue<PreparedBundle*>                    incoming;
    SpscQueue<PreparedBundle*>                    completed;
    SpscQueue<Node*>                              retired;
    SpscQueue<Notification>                       notifications;
    std::vector<PreparedBundle*>                  scheduled;
    std::unordered_map<std::uint32_t, Definition> definitions;
    std::vector<Allocation>                       audio_alloc;
    std::vector<Allocation>                       control_alloc;
    std::vector<Buffer>                           buffers;
    std::uint64_t                                 buffer_used    = 0;
    std::uint32_t                                 next_def       = 1;
    std::uint64_t                                 order          = 0;
    std::size_t                                   owned_nodes    = 0;
    std::size_t                                   owned_bundles  = 0;
    std::uint64_t                                 frame          = 0;
    std::uint64_t                                 skipped        = 0;
    std::uint64_t                                 late_played    = 0;
    std::uint64_t                                 late_dropped   = 0;
    std::uint64_t                                 events_dropped = 0;

    Node*
    find (NodeId id) const noexcept {
        return node_tree.find (id);
    }

    void
    notify (const Notification& event) noexcept {
        if (!notifications.try_push (event)) {
            ++events_dropped;
        }
    }

    static void
    retire_node (void* context, Node* node) noexcept {
        auto&        state = *static_cast<Impl*> (context);
        Notification event;
        event.kind   = Notification::Kind::n_end;
        event.client = static_cast<std::uint8_t> (node->id >> proto::kClientIdShift);
        event.id     = node->id;
        state.notify (event);
        // every allocated node has one reserved slot in the reverse queue.
        if (!state.retired.try_push (node)) {
            std::terminate();
        }
    }

    NodeTree::RemovalSink
    removal_sink () noexcept {
        return { this, &Impl::retire_node };
    }

    void
    remove (Node* node) noexcept {
        node_tree.erase_subtree (node, removal_sink());
    }

    ErrorCode
    execute (PreparedCommand& prepared) noexcept {
        return std::visit (
            [&] (const auto& command) -> ErrorCode {
                using T = std::decay_t<decltype (command)>;
                if constexpr (std::is_same_v<T, proto::NodeNew> ||
                              std::is_same_v<T, proto::GroupNew>) {
                    const auto error = node_tree.insert (
                        prepared.node.get(),
                        command.add,
                        command.target,
                        removal_sink()
                    );
                    if (error != ErrorCode::ok) {
                        return error;
                    }
                    static_cast<void> (prepared.node.release());
                } else if constexpr (std::is_same_v<T, proto::NodeFree>) {
                    for (auto id : command.ids) {
                        if (auto* node = find (id)) {
                            remove (node);
                        } else {
                            ++skipped;
                        }
                    }
                } else if constexpr (std::is_same_v<T, proto::CbusSet>) {
                    for (std::size_t i = 0; i < command.values.size(); ++i) {
                        buses.write_control (
                            command.index + static_cast<std::uint32_t> (i),
                            command.values[i]
                        );
                    }
                } else {
                    auto* node = find (command.id);
                    if (!node) {
                        ++skipped;
                        return ErrorCode::ok;
                    }
                    if constexpr (std::is_same_v<T, proto::NodeSet>) {
                        if (!node->synth) {
                            ++skipped;
                            return ErrorCode::ok;
                        }
                        for (const auto& pair : command.params) {
                            if (pair.index >= node->synth->param_count()) {
                                return ErrorCode::invalid_arg;
                            }
                        }
                        const auto duration = static_cast<std::uint64_t> (
                            std::ceil (static_cast<double> (command.ramp) * config.sample_rate)
                        );
                        for (const auto& pair : command.params) {
                            node->synth->set_param (pair.index, pair.value, duration);
                        }
                    } else if constexpr (std::is_same_v<T, proto::NodeRun>) {
                        node->running = command.run;
                    } else if constexpr (std::is_same_v<T, proto::GroupClear>) {
                        if (node->synth) {
                            ++skipped;
                            return ErrorCode::ok;
                        }
                        node_tree.clear (node, removal_sink());
                    } else if constexpr (std::is_same_v<T, proto::NodeMove>) {
                        return node_tree.move (node, command.add, command.target);
                    }
                }
                return ErrorCode::ok;
            },
            prepared.command
        );
    }

    void
    receive () noexcept {
        PreparedBundle* bundle = nullptr;
        for (std::size_t count = 0; count < config.queue_capacity && incoming.try_pop (bundle);
             ++count) {
            bundle->target =
                bundle->time == 0 ?
                    frame + (config.block_size - frame % config.block_size) % config.block_size :
                    clock.frame_at (bundle->time);
            bundle->effective_time =
                bundle->time == 0 ? clock.ns_at (bundle->target) : bundle->time;
            scheduled.push_back (bundle);
            std::push_heap (scheduled.begin(), scheduled.end(), Later{});
        }
    }

    void
    apply_due () noexcept {
        while (!scheduled.empty() && scheduled.front()->target <= frame) {
            std::pop_heap (scheduled.begin(), scheduled.end(), Later{});
            auto* bundle = scheduled.back();
            scheduled.pop_back();
            bool dropped = false;
            if (bundle->time != 0 && bundle->target < frame) {
                const auto late = clock.ns_at (frame) - bundle->time;
                dropped         = late > config.late_tolerance_ns;
                if (dropped) {
                    ++late_dropped;
                } else {
                    ++late_played;
                }
                Notification event;
                event.kind    = Notification::Kind::late;
                event.client  = bundle->client;
                event.seq     = bundle->seq;
                event.late_ns = late;
                event.played  = !dropped;
                notify (event);
            }
            if (!dropped) {
                for (std::size_t i = 0; i < bundle->commands.size(); ++i) {
                    auto&      command = bundle->commands[i];
                    const bool creates = command.node != nullptr;
                    const auto id      = creates ? command.node->id : 0;
                    auto       error   = execute (command);
                    if (error != ErrorCode::ok) {
                        for (auto it = bundle->created.rbegin(); it != bundle->created.rend();
                             ++it) {
                            if (auto* node = find (*it)) {
                                remove (node);
                            }
                        }
                        Notification event;
                        event.kind   = Notification::Kind::error;
                        event.client = bundle->client;
                        event.seq    = bundle->seq;
                        event.index  = static_cast<std::uint16_t> (i);
                        event.opcode = proto::opcode_of (command.command);
                        event.code   = error;
                        notify (event);
                        break;
                    }
                    if (creates) {
                        bundle->created.push_back (id);
                    }
                }
            }
            if (!completed.try_push (bundle)) {
                std::terminate();
            }
        }
    }
};

Engine::Engine (EngineConfig config) : impl_ (std::make_unique<Impl> (config)) {}
Engine::~Engine () = default;
const EngineConfig&
Engine::config () const noexcept {
    return impl_->config;
}
std::uint64_t
Engine::frame () const noexcept {
    return impl_->frame;
}

void
Engine::collect () {
    Node* node = nullptr;
    while (impl_->retired.try_pop (node)) {
        delete node;
        --impl_->owned_nodes;
    }
    PreparedBundle* bundle = nullptr;
    while (impl_->completed.try_pop (bundle)) {
        for (const auto& command : bundle->commands) {
            if (command.node) {
                --impl_->owned_nodes;
            }
        }
        delete bundle;
        --impl_->owned_bundles;
    }
}

bool
Engine::pop_notification (Notification& notification) noexcept {
    return impl_->notifications.try_pop (notification);
}

const proto::GraphDef*
Engine::graph_definition (std::uint32_t def) const noexcept {
    auto it = impl_->definitions.find (def);
    return it == impl_->definitions.end() ? nullptr : &it->second.graph->graph();
}

std::expected<void, EngineError>
Engine::submit (std::uint8_t client, std::uint32_t seq, const proto::Bundle& bundle) {
    collect();
    auto& state = *impl_;
    if (client == 0) {
        return fail (ErrorCode::id_range, "client zero is reserved");
    }
    if (auto check = proto::validate (bundle); !check) {
        return fail (check.error().code, std::string (check.error().detail), check.error().index);
    }
    if (auto check = proto::encode (proto::Message{ seq, bundle }); !check) {
        return fail (ErrorCode::invalid_arg, std::string (check.error().detail));
    }
    if (state.owned_bundles >= state.config.queue_capacity) {
        return fail (ErrorCode::limit, "rt queue is full");
    }
    auto pending    = std::make_unique<PreparedBundle>();
    pending->client = client;
    pending->seq    = seq;
    pending->time   = bundle.time;
    pending->order  = state.order++;
    pending->commands.reserve (bundle.commands.size());
    pending->created.reserve (bundle.commands.size());
    std::size_t created = 0;
    for (std::size_t i = 0; i < bundle.commands.size(); ++i) {
        PreparedCommand prepared{ bundle.commands[i], nullptr };
        auto            checked = std::visit (
            [&] (const auto& command) -> std::expected<void, EngineError> {
                using T = std::decay_t<decltype (command)>;
                if constexpr (std::is_same_v<T, proto::NodeNew> ||
                              std::is_same_v<T, proto::GroupNew>) {
                    if (command.id >> proto::kClientIdShift != client) {
                        return fail (ErrorCode::id_range, "node id is outside client range");
                    }
                    if (state.owned_nodes + created >= state.config.max_nodes) {
                        return fail (ErrorCode::limit, "node memory limit");
                    }
                    prepared.node     = std::make_unique<Node>();
                    prepared.node->id = command.id;
                    if constexpr (std::is_same_v<T, proto::NodeNew>) {
                        auto found = state.definitions.find (command.def);
                        if (found == state.definitions.end()) {
                            return fail (ErrorCode::not_found, "graph definition not found");
                        }
                        for (const auto& param : command.params) {
                            if (param.index >= found->second.graph->graph().params.size()) {
                                return fail (
                                    ErrorCode::invalid_arg,
                                    "parameter index out of range"
                                );
                            }
                        }
                        prepared.node->def     = command.def;
                        prepared.node->running = !command.paused;
                        prepared.node->synth   = std::make_unique<GraphInstance> (
                            found->second.graph,
                            state.config.sample_rate,
                            state.config.block_size,
                            command.params
                        );
                    }
                    ++created;
                } else if constexpr (std::is_same_v<T, proto::CbusSet>) {
                    if (command.index > state.control_alloc.size() ||
                        command.values.size() > state.control_alloc.size() - command.index) {
                        return fail (ErrorCode::not_found, "control bus range not found");
                    }
                    for (std::size_t j = 0; j < command.values.size(); ++j) {
                        if (state.control_alloc[command.index + j].owner == 0) {
                            return fail (ErrorCode::not_found, "control bus is not allocated");
                        }
                    }
                }
                return {};
            },
            prepared.command
        );
        if (!checked) {
            auto error  = checked.error();
            error.index = static_cast<std::uint16_t> (i);
            return std::unexpected (std::move (error));
        }
        pending->commands.push_back (std::move (prepared));
    }
    if (!state.incoming.try_push (pending.get())) {
        return fail (ErrorCode::limit, "rt queue is full");
    }
    state.owned_nodes += created;
    ++state.owned_bundles;
    static_cast<void> (pending.release());
    return {};
}

void
Engine::process (std::span<float> output, std::uint32_t frames) noexcept {
    auto&      state  = *impl_;
    const auto needed = static_cast<std::size_t> (frames) * state.config.out_channels;
    if (output.size() != needed) {
        std::fill (output.begin(), output.end(), 0.F);
        return;
    }
    state.receive();
    state.apply_due();
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto offset = static_cast<std::uint16_t> (state.frame % state.config.block_size);
        if (offset == 0) {
            state.buses.clear_audio();
            state.receive();
        }
        state.apply_due();
        auto* node = state.node_tree.root().running ? state.node_tree.root().first : nullptr;
        while (node) {
            if (!node->running) {
                node = NodeTree::after_subtree (node);
                continue;
            }
            if (!node->synth && node->first) {
                node = node->first;
                continue;
            }
            auto* next = NodeTree::after_subtree (node);
            if (node->synth && node->synth->process (state.buses, offset)) {
                state.remove (node);
            }
            node = next;
        }
        for (std::uint32_t channel = 0; channel < state.config.out_channels; ++channel) {
            output[static_cast<std::size_t> (i) * state.config.out_channels + channel] =
                state.buses.read_audio (channel, offset);
        }
        ++state.frame;
    }
}

proto::StatusInfo
Engine::status () const {
    const auto&       state = *impl_;
    proto::StatusInfo info;
    info.now_ns      = state.clock.ns_at (state.frame);
    info.sample_rate = state.config.sample_rate;
    info.block_size  = state.config.block_size;
    info.nodes       = state.node_tree.node_count();
    info.groups      = state.node_tree.group_count();
    info.defs        = static_cast<std::uint32_t> (state.definitions.size());
    info.buffers     = static_cast<std::uint32_t> (
        std::ranges::count_if (state.buffers, [] (const Buffer& buffer) {
            return buffer.owner != 0;
        })
    );
    // completed work remains reserved until the control thread collects it.
    info.rt_queue_used     = static_cast<std::uint32_t> (state.owned_bundles);
    info.rt_queue_capacity = state.config.queue_capacity;
    info.late_played       = state.late_played;
    info.late_dropped      = state.late_dropped;
    info.skipped           = state.skipped;
    info.events_dropped    = state.events_dropped;
    return info;
}

std::expected<proto::TreeInfo, EngineError>
Engine::tree (NodeId id, std::uint16_t depth) const {
    auto* root = impl_->find (id);
    if (!root) {
        return fail (ErrorCode::not_found, "tree root not found");
    }
    proto::TreeInfo       info;
    auto*                 current     = root;
    std::uint32_t         level       = 0;
    constexpr std::size_t kMaxEntries = (proto::kMaxMessage - 21) / 14;
    while (current) {
        if (info.entries.size() == kMaxEntries) {
            info.truncated = true;
            break;
        }
        info.entries.push_back (
            { current->id,
              current == root ? proto::kNoParent : current->parent->id,
              current->synth ? proto::NodeKind::synth : proto::NodeKind::group,
              current->def,
              current->running }
        );
        if (current->first && (depth == 0 || level < depth)) {
            current = current->first;
            ++level;
            continue;
        }
        while (current != root && !current->next) {
            current = current->parent;
            --level;
        }
        if (current == root) {
            break;
        }
        current = current->next;
    }
    return info;
}

std::expected<proto::CallResult, EngineError>
Engine::call (std::uint8_t client, const proto::CallCommand& command) {
    collect();
    if (client == 0) {
        return fail (ErrorCode::id_range, "client zero is reserved");
    }
    if (auto check = proto::encode (proto::Message{ 0, proto::Call{ command } }); !check) {
        return fail (ErrorCode::invalid_arg, std::string (check.error().detail));
    }
    auto& state = *impl_;
    return std::visit (
        [&] (const auto& value) -> std::expected<proto::CallResult, EngineError> {
            using T = std::decay_t<decltype (value)>;
            if constexpr (std::is_same_v<T, proto::DefLoad>) {
                auto graph = proto::load_graph (value.graph);
                if (!graph) {
                    return fail (ErrorCode::graph, graph.error().message);
                }
                auto compiled = CompiledGraph::compile (*graph);
                if (!compiled) {
                    return fail (ErrorCode::graph, compiled.error().message);
                }
                if (state.next_def == 0) {
                    return fail (ErrorCode::limit, "graph numbers exhausted");
                }
                const auto id = state.next_def++;
                state.definitions.emplace (id, Definition{ *compiled, client });
                return proto::DefLoaded{ id };
            } else if constexpr (std::is_same_v<T, proto::DefFree>) {
                if (state.definitions.erase (value.def) == 0) {
                    return fail (ErrorCode::not_found, "graph not found");
                }
                return proto::Empty{};
            } else if constexpr (std::is_same_v<T, proto::Status>) {
                return status();
            } else if constexpr (std::is_same_v<T, proto::Tree>) {
                auto result = tree (value.root, value.depth);
                if (!result) {
                    return std::unexpected (result.error());
                }
                return *result;
            } else if constexpr (std::is_same_v<T, proto::Sync>) {
                // offline control is quiescent: apply prior immediate work without a sample.
                process ({}, 0);
                collect();
                for (const auto* pending : state.scheduled) {
                    if (pending->client == client && pending->time == 0) {
                        return fail (
                            ErrorCode::context,
                            "sync is pending: render through the next block boundary"
                        );
                    }
                }
                return proto::Empty{};
            } else if constexpr (std::is_same_v<T, proto::BusAlloc>) {
                if (value.count == 0) {
                    return fail (ErrorCode::invalid_arg, "empty bus allocation");
                }
                auto& allocations =
                    value.kind == proto::BusKind::audio ? state.audio_alloc : state.control_alloc;
                std::size_t run = 0;
                for (std::size_t i = 0; i < allocations.size(); ++i) {
                    run = allocations[i].owner == 0 ? run + 1 : 0;
                    if (run == value.count) {
                        const auto base = i + 1 - run;
                        for (std::size_t j = base; j <= i; ++j) {
                            allocations[j] = { 0, client };
                        }
                        allocations[base].count = value.count;
                        return proto::BusAllocated{ static_cast<std::uint32_t> (base) };
                    }
                }
                return fail (ErrorCode::limit, "no contiguous bus range");
            } else if constexpr (std::is_same_v<T, proto::BusFree>) {
                auto& allocations =
                    value.kind == proto::BusKind::audio ? state.audio_alloc : state.control_alloc;
                if (value.base >= allocations.size() || allocations[value.base].count == 0 ||
                    (value.kind == proto::BusKind::audio &&
                     value.base < state.config.out_channels)) {
                    return fail (ErrorCode::not_found, "bus allocation not found");
                }
                const auto count = allocations[value.base].count;
                for (std::size_t i = value.base; i < value.base + count; ++i) {
                    allocations[i] = {};
                    if (value.kind == proto::BusKind::control) {
                        state.buses.write_control (static_cast<std::uint32_t> (i), 0.F);
                    }
                }
                return proto::Empty{};
            } else if constexpr (std::is_same_v<T, proto::BufAlloc> ||
                                 std::is_same_v<T, proto::BufLoad>) {
                std::size_t id = 0;
                while (id < state.buffers.size() && state.buffers[id].owner != 0) {
                    ++id;
                }
                if (id == state.buffers.size()) {
                    return fail (ErrorCode::limit, "buffer table is full");
                }
                std::uint32_t frames      = 0;
                std::uint32_t sample_rate = state.config.sample_rate;
                std::uint16_t channels    = 0;
#ifdef BITVM_HAVE_SNDFILE
                SF_INFO info{};
                using SoundFile = std::unique_ptr<SNDFILE, decltype (&sf_close)>;
                SoundFile file (nullptr, &sf_close);
#endif
                if constexpr (std::is_same_v<T, proto::BufAlloc>) {
                    frames   = value.frames;
                    channels = value.channels;
                    if (frames == 0 || channels == 0 || channels > 16) {
                        return fail (ErrorCode::invalid_arg, "invalid buffer shape");
                    }
                } else {
#ifdef BITVM_HAVE_SNDFILE
                    file.reset (sf_open (value.path.c_str(), SFM_READ, &info));
                    if (!file) {
                        const auto reason = sf_error (nullptr);
                        const bool format = reason == SF_ERR_UNRECOGNISED_FORMAT ||
                                            reason == SF_ERR_MALFORMED_FILE ||
                                            reason == SF_ERR_UNSUPPORTED_ENCODING;
                        return fail (
                            format ? ErrorCode::format : ErrorCode::io,
                            format ? "unsupported or malformed sample format" :
                                     "cannot open sample file"
                        );
                    }
                    if (info.channels < 1 || info.channels > 16 || info.samplerate < 1 ||
                        info.frames < 0) {
                        return fail (ErrorCode::format, "invalid sample format");
                    }
                    if (value.start >= static_cast<std::uint64_t> (info.frames)) {
                        return fail (ErrorCode::invalid_arg, "sample start outside file");
                    }
                    const auto available = static_cast<std::uint64_t> (info.frames) - value.start;
                    const auto requested = value.frames == 0 ?
                                               available :
                                               std::min<std::uint64_t> (available, value.frames);
                    if (requested > std::numeric_limits<std::uint32_t>::max()) {
                        return fail (ErrorCode::limit, "sample is too long");
                    }
                    frames      = static_cast<std::uint32_t> (requested);
                    channels    = static_cast<std::uint16_t> (info.channels);
                    sample_rate = static_cast<std::uint32_t> (info.samplerate);
#else
                    return fail (ErrorCode::unsupported, "sample file loading is disabled");
#endif
                }
                const auto bytes = static_cast<std::uint64_t> (frames) * channels * sizeof (float);
                if (bytes > state.config.buffer_bytes - state.buffer_used) {
                    return fail (ErrorCode::limit, "buffer memory limit");
                }
                std::vector<float> samples (static_cast<std::size_t> (frames) * channels);
                if constexpr (std::is_same_v<T, proto::BufLoad>) {
#ifdef BITVM_HAVE_SNDFILE
                    if (sf_seek (file.get(), value.start, SEEK_SET) < 0 ||
                        sf_readf_float (file.get(), samples.data(), frames) != frames) {
                        return fail (ErrorCode::io, "cannot read sample file");
                    }
#endif
                }
                state.buffers[id] = { std::move (samples), client };
                state.buffer_used += bytes;
                if constexpr (std::is_same_v<T, proto::BufLoad>) {
                    return proto::BufLoaded{ static_cast<std::uint32_t> (id),
                                             frames,
                                             channels,
                                             sample_rate };
                } else {
                    return proto::BufAllocated{ static_cast<std::uint32_t> (id) };
                }
            } else if constexpr (std::is_same_v<T, proto::BufFree>) {
                if (value.buf >= state.buffers.size() || state.buffers[value.buf].owner == 0) {
                    return fail (ErrorCode::not_found, "buffer not found");
                }
                state.buffer_used -= state.buffers[value.buf].samples.size() * sizeof (float);
                state.buffers[value.buf] = {};
                return proto::Empty{};
            } else if constexpr (std::is_same_v<T, proto::ClientReset>) {
                process ({}, 0);
                // reset cancels this client's queued notes as well as its current resources.
                std::erase_if (state.scheduled, [&] (PreparedBundle* pending) {
                    if (pending->client == client) {
                        if (!state.completed.try_push (pending)) {
                            std::terminate();
                        }
                        return true;
                    }
                    return false;
                });
                std::make_heap (state.scheduled.begin(), state.scheduled.end(), Later{});
                for (auto* node = state.node_tree.root().first; node;) {
                    auto* next = NodeTree::after_subtree (node);
                    if (node->id >> proto::kClientIdShift == client) {
                        state.remove (node);
                    } else if (node->first) {
                        next = node->first;
                    }
                    node = next;
                }
                std::erase_if (state.definitions, [&] (const auto& item) {
                    return item.second.owner == client;
                });
                for (std::size_t i = 0; i < state.buffers.size(); ++i) {
                    if (state.buffers[i].owner == client) {
                        state.buffer_used -= state.buffers[i].samples.size() * sizeof (float);
                        state.buffers[i] = {};
                    }
                }
                for (auto kind : { proto::BusKind::audio, proto::BusKind::control }) {
                    auto& allocations =
                        kind == proto::BusKind::audio ? state.audio_alloc : state.control_alloc;
                    for (std::size_t i = 0; i < allocations.size(); ++i) {
                        if (kind == proto::BusKind::audio && i < state.config.out_channels) {
                            continue;
                        }
                        if (allocations[i].owner == client) {
                            allocations[i] = {};
                            if (kind == proto::BusKind::control) {
                                state.buses.write_control (static_cast<std::uint32_t> (i), 0.F);
                            }
                        }
                    }
                }
                collect();
                return proto::Empty{};
            }
        },
        command
    );
}

}  // namespace bitvm
