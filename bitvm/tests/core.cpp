#include <gtest/gtest.h>

#include <unistd.h>
#include <array>
#include <thread>
#include <vector>

#include "bitvm/engine.hpp"
#include "bitvm/proto/text.hpp"
#include "bitvm/spsc.hpp"
#include "bitvm/time_map.hpp"

namespace
{
constexpr auto kNode = 0x01000001U;

std::uint32_t
load_constant (bitvm::Engine& engine) {
    auto graph = bitvm::proto::assemble_graph ("graph constant\nparam amp 1\nout.ar 0 $amp $amp\n");
    EXPECT_TRUE (graph.has_value());
    auto bytes = bitvm::proto::encode_graph (*graph);
    EXPECT_TRUE (bytes.has_value());
    auto result = engine.call (1, bitvm::proto::DefLoad{ *bytes });
    EXPECT_TRUE (result.has_value());
    return std::get<bitvm::proto::DefLoaded> (*result).def;
}

TEST (Spsc, CapacityWrapAndConcurrentOrder) {
    bitvm::SpscQueue<std::uint32_t> queue (8);
    for (std::uint32_t i = 0; i < 8; ++i) {
        ASSERT_TRUE (queue.try_push (i));
    }
    EXPECT_FALSE (queue.try_push (9));
    std::uint32_t value = 0;
    for (std::uint32_t i = 0; i < 8; ++i) {
        ASSERT_TRUE (queue.try_pop (value));
        ASSERT_EQ (value, i);
    }
    EXPECT_FALSE (queue.try_pop (value));
    std::thread producer ([&] {
        for (std::uint32_t i = 0; i < 100000; ++i) {
            while (!queue.try_push (i)) {}
        }
    });
    for (std::uint32_t i = 0; i < 100000; ++i) {
        while (!queue.try_pop (value)) {}
        ASSERT_EQ (value, i);
    }
    producer.join();
}

TEST (TimeMap, NeverSchedulesBeforeTimestamp) {
    bitvm::TimeMap map (48000);
    EXPECT_EQ (map.frame_at (1), 1U);
    EXPECT_EQ (map.frame_at (1000000), 48U);
    EXPECT_EQ (map.frame_at (1000001), 49U);
    EXPECT_EQ (map.ns_at (48000), 1000000000);
    EXPECT_EQ (map.frame_at (map.ns_at (96000)), 96000U);
    bitvm::TimeMap absolute (44100, 8000000000000000LL);
    EXPECT_EQ (absolute.frame_at (8000001000000000LL), 44100U);
    EXPECT_EQ (absolute.frame_at (1), 0U);
}

TEST (Engine, ExactOnsetStopAndCallbackSize) {
    bitvm::Engine engine;
    auto          def = load_constant (engine);
    ASSERT_TRUE (engine.submit (
        1,
        1,
        { 1000000,
          { bitvm::proto::NodeNew{ kNode, def, bitvm::proto::AddAction::head, 0, false, {} } } }
    ));
    ASSERT_TRUE (engine.submit (1, 2, { 2000000, { bitvm::proto::NodeFree{ { kNode } } } }));
    std::vector<float> output (256);
    engine.process (std::span<float> (output).first (74), 37);
    engine.process (std::span<float> (output).subspan (74), 91);
    for (std::size_t frame = 0; frame < 128; ++frame) {
        EXPECT_EQ (output[frame * 2], frame >= 48 && frame < 96 ? 1.F : 0.F) << frame;
        EXPECT_EQ (output[frame * 2 + 1], output[frame * 2]);
    }
    engine.collect();
    EXPECT_EQ (engine.status().nodes, 0U);
}

TEST (Engine, RollbackAndSoftSkips) {
    bitvm::Engine engine;
    auto          def = load_constant (engine);
    ASSERT_TRUE (engine.submit (
        1,
        7,
        { 0,
          { bitvm::proto::GroupNew{ kNode },
            bitvm::proto::
                NodeNew{ kNode + 1, def, bitvm::proto::AddAction::head, kNode, false, {} },
            bitvm::proto::GroupNew{ kNode } } }
    ));
    std::array<float, 2> output{};
    engine.process (output, 1);
    EXPECT_EQ (output[0], 0.F);
    EXPECT_EQ (engine.status().groups, 1U);
    EXPECT_EQ (engine.status().nodes, 0U);
    bitvm::Notification notification;
    unsigned            ended = 0;
    bool                error = false;
    while (engine.pop_notification (notification)) {
        if (notification.kind == bitvm::Notification::Kind::n_end) {
            ++ended;
        } else if (notification.kind == bitvm::Notification::Kind::error) {
            error = true;
            EXPECT_EQ (notification.seq, 7U);
            EXPECT_EQ (notification.index, 2U);
            EXPECT_EQ (notification.code, bitvm::proto::ErrorCode::exists);
        }
    }
    EXPECT_EQ (ended, 2U);
    EXPECT_TRUE (error);
    ASSERT_TRUE (engine.submit (1, 8, { 0, { bitvm::proto::NodeRun{ kNode, false } } }));
    engine.process (output, 1);
    EXPECT_EQ (engine.status().skipped, 0U);
    std::array<float, 124> rest{};
    engine.process (rest, 62);
    engine.process (output, 1);
    EXPECT_EQ (engine.status().skipped, 1U);
}

TEST (Engine, LevelOneAndResourceLimits) {
    bitvm::Engine engine;
    auto          def = load_constant (engine);
    EXPECT_FALSE (engine.submit (
        2,
        1,
        { 0, { bitvm::proto::NodeNew{ kNode, def, bitvm::proto::AddAction::head, 0, false, {} } } }
    ));
    EXPECT_FALSE (engine.submit (
        1,
        1,
        { 0, { bitvm::proto::NodeNew{ kNode, 999, bitvm::proto::AddAction::head, 0, false, {} } } }
    ));
    EXPECT_FALSE (engine.submit (
        1,
        1,
        { 0,
          { bitvm::proto::
                NodeNew{ kNode, def, bitvm::proto::AddAction::head, 0, false, { { 1, 1.F } } } } }
    ));
    bitvm::EngineConfig config;
    config.queue_capacity = 1;
    bitvm::Engine small (config);
    ASSERT_TRUE (small.submit (1, 1, { 1000000, { bitvm::proto::GroupNew{ kNode } } }));
    auto full = small.submit (1, 2, { 0, { bitvm::proto::GroupNew{ kNode + 1 } } });
    ASSERT_FALSE (full);
    EXPECT_EQ (full.error().code, bitvm::proto::ErrorCode::limit);
}

TEST (Engine, StableOrderPauseRampAndDefinitionLifetime) {
    bitvm::Engine engine;
    auto          def = load_constant (engine);
    ASSERT_TRUE (engine.submit (
        1,
        1,
        { 0, { bitvm::proto::NodeNew{ kNode, def, bitvm::proto::AddAction::head, 0, false, {} } } }
    ));
    ASSERT_TRUE (engine.submit (
        1,
        2,
        { 0, { bitvm::proto::NodeSet{ kNode, 0.F, bitvm::proto::Curve::lin, { { 0, 0.25F } } } } }
    ));
    ASSERT_TRUE (engine.call (1, bitvm::proto::DefFree{ def }));
    std::array<float, 128> output{};
    engine.process (output, 64);
    EXPECT_EQ (output[0], 0.25F);
    EXPECT_FALSE (engine.submit (
        1,
        3,
        { 0,
          { bitvm::proto::NodeNew{ kNode + 1, def, bitvm::proto::AddAction::head, 0, false, {} } } }
    ));
    ASSERT_TRUE (engine.submit (1, 4, { 0, { bitvm::proto::NodeRun{ kNode, false } } }));
    engine.process (output, 64);
    EXPECT_EQ (output[0], 0.F);
    ASSERT_TRUE (engine.submit (1, 5, { 0, { bitvm::proto::NodeRun{ kNode, true } } }));
    engine.process (output, 64);
    EXPECT_EQ (output[0], 0.25F);
}

TEST (Engine, LatePolicy) {
    bitvm::Engine      engine;
    std::vector<float> output (9600);
    engine.process (output, 4800);
    ASSERT_TRUE (engine.submit (1, 1, { 1000000, { bitvm::proto::GroupNew{ kNode } } }));
    ASSERT_TRUE (engine.submit (1, 2, { 90000000, { bitvm::proto::GroupNew{ kNode + 1 } } }));
    engine.process (std::span<float> (output).first (128), 64);
    EXPECT_EQ (engine.status().late_dropped, 1U);
    EXPECT_EQ (engine.status().late_played, 1U);
    auto tree = engine.tree();
    ASSERT_TRUE (tree);
    ASSERT_EQ (tree->entries.size(), 2U);
    EXPECT_EQ (tree->entries[1].id, kNode + 1);
}

TEST (Engine, DistinctTimestampsWithinOneSampleKeepTimeOrder) {
    bitvm::Engine engine;
    ASSERT_TRUE (engine.submit (
        1,
        1,
        { 2, { bitvm::proto::GroupNew{ kNode + 1, bitvm::proto::AddAction::head, kNode } } }
    ));
    ASSERT_TRUE (engine.submit (1, 2, { 1, { bitvm::proto::GroupNew{ kNode } } }));
    std::array<float, 4> output;
    engine.process (output, 2);
    auto tree = engine.tree();
    ASSERT_TRUE (tree);
    ASSERT_EQ (tree->entries.size(), 3U);
    EXPECT_EQ (tree->entries[1].id, kNode);
    EXPECT_EQ (tree->entries[2].id, kNode + 1);
}

TEST (Engine, ResetReleasesFutureReservationsImmediately) {
    bitvm::EngineConfig config;
    config.max_nodes      = 1;
    config.queue_capacity = 1;
    bitvm::Engine engine (config);
    ASSERT_TRUE (engine.submit (1, 1, { 1000000000, { bitvm::proto::GroupNew{ kNode } } }));
    ASSERT_TRUE (engine.call (1, bitvm::proto::ClientReset{}));
    EXPECT_EQ (engine.status().rt_queue_used, 0U);
    ASSERT_TRUE (engine.submit (1, 2, { 0, { bitvm::proto::GroupNew{ kNode + 1 } } }));
    engine.process ({}, 0);
    auto tree = engine.tree();
    ASSERT_TRUE (tree);
    ASSERT_EQ (tree->entries.size(), 2U);
    EXPECT_EQ (tree->entries[1].id, kNode + 1);
}

TEST (Engine, SyncDoesNotClaimSuccessBeforeImmediateWorkIsApplied) {
    bitvm::Engine        engine;
    std::array<float, 2> sample;
    engine.process (sample, 1);
    ASSERT_TRUE (engine.submit (1, 1, { 0, { bitvm::proto::GroupNew{ kNode } } }));
    EXPECT_FALSE (engine.call (1, bitvm::proto::Sync{}));
    EXPECT_EQ (engine.tree()->entries.size(), 1U);
    std::array<float, 126> remainder;
    engine.process (remainder, 63);
    ASSERT_TRUE (engine.call (1, bitvm::proto::Sync{}));
    EXPECT_EQ (engine.tree()->entries.size(), 2U);
}

TEST (Engine, MovesDepthReplaceAndCycleErrors) {
    bitvm::Engine engine;
    const auto    head = bitvm::proto::AddAction::head;
    const auto    tail = bitvm::proto::AddAction::tail;
    ASSERT_TRUE (engine.submit (
        1,
        1,
        { 0,
          { bitvm::proto::GroupNew{ kNode, head, 0 },
            bitvm::proto::GroupNew{ kNode + 1, tail, 0 },
            bitvm::proto::GroupNew{ kNode + 2, tail, kNode },
            bitvm::proto::NodeMove{ kNode + 1, head, kNode } } }
    ));
    engine.process ({}, 0);
    auto tree = engine.tree();
    ASSERT_TRUE (tree);
    ASSERT_EQ (tree->entries.size(), 4U);
    EXPECT_EQ (tree->entries[1].id, kNode);
    EXPECT_EQ (tree->entries[2].id, kNode + 1);
    EXPECT_EQ (tree->entries[3].id, kNode + 2);
    EXPECT_EQ (engine.tree (0, 1)->entries.size(), 2U);
    ASSERT_TRUE (engine.submit (1, 2, { 0, { bitvm::proto::NodeMove{ kNode, head, kNode + 2 } } }));
    engine.process ({}, 0);
    bitvm::Notification notification;
    ASSERT_TRUE (engine.pop_notification (notification));
    EXPECT_EQ (notification.code, bitvm::proto::ErrorCode::invalid_arg);
    ASSERT_TRUE (engine.submit (
        1,
        3,
        { 0, { bitvm::proto::GroupNew{ kNode + 3, bitvm::proto::AddAction::replace, kNode } } }
    ));
    engine.process ({}, 0);
    tree = engine.tree();
    ASSERT_TRUE (tree);
    ASSERT_EQ (tree->entries.size(), 2U);
    EXPECT_EQ (tree->entries[1].id, kNode + 3);
    ASSERT_TRUE (engine.submit (1, 4, { 0, { bitvm::proto::GroupClear{ 0 } } }));
    engine.process ({}, 0);
    EXPECT_EQ (engine.tree()->entries.size(), 1U);
}

TEST (Engine, BusAndBufferAllocationAreFirstFitAndBounded) {
    bitvm::EngineConfig config;
    config.audio_buses   = 4;
    config.control_buses = 4;
    config.max_buffers   = 1;
    config.buffer_bytes  = 16;
    bitvm::Engine engine (config);
    auto result = engine.call (1, bitvm::proto::BusAlloc{ bitvm::proto::BusKind::audio, 2 });
    ASSERT_TRUE (result);
    EXPECT_EQ (std::get<bitvm::proto::BusAllocated> (*result).base, 2U);
    EXPECT_FALSE (engine.call (1, bitvm::proto::BusAlloc{ bitvm::proto::BusKind::audio, 1 }));
    EXPECT_FALSE (engine.call (1, bitvm::proto::BusFree{ bitvm::proto::BusKind::audio, 0 }));
    ASSERT_TRUE (engine.call (1, bitvm::proto::BusFree{ bitvm::proto::BusKind::audio, 2 }));
    result = engine.call (1, bitvm::proto::BusAlloc{ bitvm::proto::BusKind::audio, 1 });
    ASSERT_TRUE (result);
    EXPECT_EQ (std::get<bitvm::proto::BusAllocated> (*result).base, 2U);
    EXPECT_FALSE (engine.submit (1, 1, { 0, { bitvm::proto::CbusSet{ 0, { 3.F } } } }));
    ASSERT_TRUE (engine.call (1, bitvm::proto::BusAlloc{ bitvm::proto::BusKind::control, 2 }));
    ASSERT_TRUE (engine.submit (1, 1, { 0, { bitvm::proto::CbusSet{ 0, { 3.F } } } }));
    engine.process ({}, 0);
    EXPECT_FALSE (engine.call (1, bitvm::proto::BufAlloc{ 5, 1 }));
    ASSERT_TRUE (engine.call (1, bitvm::proto::BufAlloc{ 4, 1 }));
    EXPECT_FALSE (engine.call (1, bitvm::proto::BufAlloc{ 1, 1 }));
    ASSERT_TRUE (engine.call (1, bitvm::proto::BufFree{ 0 }));
    result = engine.call (1, bitvm::proto::BufAlloc{ 1, 1 });
    ASSERT_TRUE (result);
    EXPECT_EQ (std::get<bitvm::proto::BufAllocated> (*result).buf, 0U);
}

TEST (Engine, Client255CannotReleaseHardwareBusReservations) {
    bitvm::Engine engine;
    ASSERT_TRUE (engine.call (255, bitvm::proto::ClientReset{}));
    auto allocated = engine.call (1, bitvm::proto::BusAlloc{ bitvm::proto::BusKind::audio, 1 });
    ASSERT_TRUE (allocated);
    EXPECT_EQ (std::get<bitvm::proto::BusAllocated> (*allocated).base, 2U);
}

TEST (Engine, InvalidSetAbortsAndPreservesEarlierNonCreationChanges) {
    bitvm::Engine engine;
    auto          def = load_constant (engine);
    ASSERT_TRUE (engine.submit (
        1,
        1,
        { 0, { bitvm::proto::NodeNew{ kNode, def, bitvm::proto::AddAction::head, 0, false, {} } } }
    ));
    engine.process ({}, 0);
    ASSERT_TRUE (engine.submit (
        1,
        2,
        { 0,
          { bitvm::proto::NodeSet{ kNode, 0, bitvm::proto::Curve::lin, { { 0, 0.5F } } },
            bitvm::proto::GroupNew{ kNode + 1 },
            bitvm::proto::NodeSet{ kNode, 0, bitvm::proto::Curve::lin, { { 1, 1.F } } },
            bitvm::proto::NodeFree{ { kNode } } } }
    ));
    std::array<float, 2> output;
    engine.process (output, 1);
    EXPECT_EQ (output[0], 0.5F);
    EXPECT_EQ (engine.tree()->entries.size(), 2U);
}

TEST (Engine, DeepSubtreesAndSiblingMovesAreIterative) {
    bitvm::Engine        engine;
    bitvm::proto::Bundle create;
    for (std::uint32_t i = 0; i < 512; ++i) {
        create.commands.emplace_back (
            bitvm::proto::GroupNew{ kNode + i,
                                    bitvm::proto::AddAction::head,
                                    i == 0 ? 0 : kNode + i - 1 }
        );
    }
    ASSERT_TRUE (engine.submit (1, 1, create));
    engine.process ({}, 0);
    EXPECT_EQ (engine.tree()->entries.size(), 513U);
    ASSERT_TRUE (engine.submit (
        1,
        2,
        { 0,
          { bitvm::proto::NodeMove{ kNode + 511, bitvm::proto::AddAction::after, kNode },
            bitvm::proto::NodeMove{ kNode + 511, bitvm::proto::AddAction::before, kNode } } }
    ));
    engine.process ({}, 0);
    auto tree = engine.tree (0, 1);
    ASSERT_TRUE (tree);
    ASSERT_EQ (tree->entries.size(), 3U);
    EXPECT_EQ (tree->entries[1].id, kNode + 511);
    EXPECT_EQ (tree->entries[2].id, kNode);
    ASSERT_TRUE (engine.submit (1, 3, { 0, { bitvm::proto::GroupClear{ 0 } } }));
    engine.process ({}, 0);
    engine.collect();
    EXPECT_EQ (engine.status().groups, 1U);
    EXPECT_EQ (engine.tree()->entries.size(), 1U);
}

TEST (Engine, ResetRemovesOwnedNodesAndTheirForeignSubtreesOnly) {
    bitvm::Engine engine;
    ASSERT_TRUE (engine.submit (1, 1, { 0, { bitvm::proto::GroupNew{ kNode } } }));
    ASSERT_TRUE (engine.submit (
        2,
        2,
        { 0,
          { bitvm::proto::GroupNew{ 0x02000001, bitvm::proto::AddAction::head, kNode },
            bitvm::proto::GroupNew{ 0x02000002, bitvm::proto::AddAction::tail, 0 } } }
    ));
    engine.process ({}, 0);
    ASSERT_TRUE (engine.call (1, bitvm::proto::ClientReset{}));
    auto tree = engine.tree();
    ASSERT_TRUE (tree);
    ASSERT_EQ (tree->entries.size(), 2U);
    EXPECT_EQ (tree->entries[1].id, 0x02000002U);
    bitvm::Notification event;
    unsigned            ended = 0;
    while (engine.pop_notification (event)) {
        if (event.kind == bitvm::Notification::Kind::n_end) {
            EXPECT_EQ (event.client, event.id == kNode ? 1 : 2);
            ++ended;
        }
    }
    EXPECT_EQ (ended, 2U);
}

TEST (Engine, SampleLoadingDistinguishesFormatAndIoErrors) {
    bitvm::Engine engine;
    auto missing = engine.call (1, bitvm::proto::BufLoad{ "/no/such/sonasphere/sample.wav", 0, 0 });
    ASSERT_FALSE (missing);
    if (missing.error().code == bitvm::proto::ErrorCode::unsupported) {
        GTEST_SKIP() << "sample loading disabled";
    }
    EXPECT_EQ (missing.error().code, bitvm::proto::ErrorCode::io);
    char      path[] = "/tmp/sonasphere-buf-XXXXXX";
    const int fd     = ::mkstemp (path);
    ASSERT_GE (fd, 0);
    constexpr char text[] = "this is not an audio file";
    ASSERT_EQ (::write (fd, text, sizeof (text)), static_cast<ssize_t> (sizeof (text)));
    ::close (fd);
    auto invalid = engine.call (1, bitvm::proto::BufLoad{ path, 0, 0 });
    ::unlink (path);
    ASSERT_FALSE (invalid);
    EXPECT_EQ (invalid.error().code, bitvm::proto::ErrorCode::format);
}

}  // namespace
