#include <gtest/gtest.h>

#include <unistd.h>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include "bitvm/backend.hpp"
#include "bitvm/proto/codec.hpp"
#include "bitvm/proto/text.hpp"
#include "bitvm/score.hpp"

namespace
{

using namespace bitvm;

std::string
constant_score (std::string_view events) {
    auto graph = proto::assemble_graph ("graph constant\nparam amp 0.25\nout.ar 0 $amp $amp\n");
    EXPECT_TRUE (graph.has_value());
    const auto bytes = proto::encode_graph (*graph);
    EXPECT_TRUE (bytes.has_value());
    return "call def.load graph=base64:" + proto::base64_encode (*bytes) + "\n" +
           std::string (events);
}

EngineConfig
test_config (std::uint16_t channels = 2) {
    EngineConfig config;
    config.sample_rate  = 8000;
    config.block_size   = 64;
    config.out_channels = channels;
    return config;
}

std::filesystem::path
temp_path (std::string_view suffix) {
    static unsigned next = 0;
    return std::filesystem::temp_directory_path() /
           ("bitvm-offline-" + std::to_string (getpid()) + "-" + std::to_string (next++) +
            std::string (suffix));
}

std::vector<std::byte>
read_bytes (const std::filesystem::path& path) {
    std::ifstream     in (path, std::ios::binary);
    const std::string text ((std::istreambuf_iterator<char> (in)), {});
    const auto        bytes = std::as_bytes (std::span (text));
    return { bytes.begin(), bytes.end() };
}

std::uint32_t
u32 (std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= std::to_integer<std::uint32_t> (bytes[offset + i]) << (8 * i);
    }
    return value;
}

}  // namespace

TEST (Offline, AppliesBundlesAtExactFramesAndCollectsNotifications) {
    const auto score = parse_score (constant_score (
        "@+0.00025 node.new id=0x01000001 def=1 add=head target=0 $amp=0.5\n"
        "@+0.000625 node.free ids=0x01000001\n"
    ));
    ASSERT_TRUE (score.has_value()) << score.error().message;
    Engine         engine (test_config());
    OfflineBackend backend (engine.config());
    const auto     result = render_score (engine, *score, backend, { .duration_seconds = 0.001 });
    ASSERT_TRUE (result.has_value()) << result.error().message;
    EXPECT_EQ (engine.frame(), 8U);
    ASSERT_EQ (backend.samples().size(), 16U);
    for (std::size_t frame = 0; frame < 8; ++frame) {
        EXPECT_FLOAT_EQ (backend.samples()[2 * frame], frame >= 2 && frame < 5 ? 0.5F : 0.0F);
        EXPECT_FLOAT_EQ (backend.samples()[2 * frame + 1], backend.samples()[2 * frame]);
    }
    EXPECT_EQ (engine.status().nodes, 0U);
}

TEST (Offline, NowAndCallsFollowFileOrderWithoutAdvancingAudio) {
    const auto score = parse_score (constant_score (
        "@+0.00025 node.new id=0x01000001 def=1 add=head target=0\n"
        "call tree root=0x01000001\n"
        "call def.free def=1\n"
        "@now node.set id=0x01000001 $amp=0.75\n"
        "@0 node.run id=0x01000001 run=0\n"
        "@+0.0005 node.run id=0x01000001 run=1\n"
    ));
    ASSERT_TRUE (score.has_value()) << score.error().message;
    Engine         engine (test_config());
    OfflineBackend backend (engine.config());
    const auto     result = render_score (engine, *score, backend, { .duration_seconds = 0.001 });
    ASSERT_TRUE (result.has_value()) << result.error().message;
    ASSERT_EQ (backend.samples().size(), 16U);
    EXPECT_FLOAT_EQ (backend.samples()[4], 0.0F);
    EXPECT_FLOAT_EQ (backend.samples()[6], 0.0F);
    EXPECT_FLOAT_EQ (backend.samples()[8], 0.75F);
}

TEST (Offline, ResolvesGraphFilesRelativeToScoreAndTracksNamedParameters) {
    const auto directory = temp_path ("-dir");
    std::filesystem::create_directory (directory);
    {
        std::ofstream graph (directory / "tone.bvg");
        graph << "graph constant\nparam amp 0.25\nout.ar 0 $amp $amp\n";
        std::ofstream score (directory / "score.bvs");
        score << "call def.load graph=@tone.bvg\n"
                 "@now {\n"
                 "node.new id=0x01000001 def=1 add=head target=0 $amp=0.5\n"
                 "node.set id=0x01000001 $amp=0.75\n"
                 "}\n";
    }
    auto score = read_score (directory / "score.bvs");
    ASSERT_TRUE (score.has_value()) << score.error().message;
    const auto& bundle = std::get<proto::Bundle> (score->messages.back().body);
    EXPECT_EQ (std::get<proto::NodeSet> (bundle.commands.back()).params.front().index, 0U);
    std::filesystem::remove_all (directory);
}

TEST (Offline, BinaryAndTextRenderIdenticallyWithExtraOutputChannels) {
    const auto text = parse_score (
        constant_score ("@now node.new id=0x01000001 def=1 add=head target=0 $amp=0.125\n")
    );
    ASSERT_TRUE (text.has_value());
    const auto binary = encode_score (*text);
    ASSERT_TRUE (binary.has_value());
    const auto decoded = decode_score (*binary);
    ASSERT_TRUE (decoded.has_value()) << decoded.error().message;
    EXPECT_EQ (decoded->messages, text->messages);
    Engine         left (test_config (4));
    Engine         right (test_config (4));
    OfflineBackend a (left.config());
    OfflineBackend b (right.config());
    ASSERT_TRUE (render_score (left, *text, a, { .duration_seconds = 0.001 }));
    ASSERT_TRUE (render_score (right, *decoded, b, { .duration_seconds = 0.001 }));
    EXPECT_EQ (a.samples(), b.samples());
    ASSERT_EQ (a.samples().size(), 32U);
    EXPECT_FLOAT_EQ (a.samples()[0], 0.125F);
    EXPECT_FLOAT_EQ (a.samples()[1], 0.125F);
    EXPECT_FLOAT_EQ (a.samples()[2], 0.0F);
    EXPECT_FLOAT_EQ (a.samples()[3], 0.0F);
}

TEST (Offline, RejectsDescendingTimesAndNonScoreMessages) {
    EXPECT_FALSE (
        parse_score ("@+2 group.clear id=0\n@now group.clear id=0\n@+1 group.clear id=0\n")
    );
    EXPECT_FALSE (parse_score ("@-1 group.clear id=0\n"));
    EXPECT_FALSE (parse_score ("call status\nevent/2 xrun count=1\n"));
    EXPECT_FALSE (parse_score ("reply/1 sync ok\n"));
    EXPECT_FALSE (
        parse_score ("hello major=0 minor=1 catalog_hash=0 requested_client=1 events= name=test\n")
    );
}

TEST (Offline, RejectsBadBinaryHeadersTruncationAndPayloads) {
    auto text = parse_score ("call status\n");
    ASSERT_TRUE (text.has_value());
    auto binary = encode_score (*text);
    ASSERT_TRUE (binary.has_value());
    for (std::size_t length = 0; length < binary->size(); ++length) {
        if (length == 16) {
            continue;
        }
        EXPECT_FALSE (decode_score (std::span<const std::byte> (*binary).first (length))) << length;
    }
    for (std::size_t offset : { 0U, 4U, 6U, 8U, 16U, 20U }) {
        auto corrupt = *binary;
        corrupt[offset] ^= std::byte{ 0x7f };
        EXPECT_FALSE (decode_score (corrupt)) << offset;
    }
    Score bad{ { proto::Message{ 1, proto::Event{ proto::Xrun{ 1 } } } }, {} };
    EXPECT_FALSE (encode_score (bad));
    for (const auto& body : { proto::MessageBody (proto::Event{ proto::Xrun{ 1 } }),
                              proto::MessageBody (
                                  proto::Reply{ static_cast<std::uint16_t> (proto::Opcode::sync),
                                                proto::CallResult (proto::Empty{}) }
                              ) }) {
        const auto packet = proto::encode (proto::Message{ 1, body });
        ASSERT_TRUE (packet.has_value());
        auto rejected = *binary;
        rejected.resize (16);
        rejected.insert (rejected.end(), packet->begin(), packet->end());
        EXPECT_FALSE (decode_score (rejected));
    }
}

TEST (Offline, ReportsCallBundleAndDurationErrors) {
    std::size_t expected_index = 1;
    for (const auto& source :
         { std::string ("call def.free def=999\n"),
           constant_score ("@now node.new id=0x02000001 def=1 add=head target=0\n"),
           constant_score (
               "@now {\nnode.new id=0x01000001 def=1 add=head target=0\nnode.new id=0x01000001 "
               "def=1 add=head target=0\n}\n"
           ) }) {
        auto score = parse_score (source);
        ASSERT_TRUE (score.has_value()) << score.error().message;
        Engine         engine (test_config());
        OfflineBackend backend (engine.config());
        const auto result = render_score (engine, *score, backend, { .duration_seconds = 0.001 });
        ASSERT_FALSE (result.has_value());
        EXPECT_EQ (result.error().message_index, expected_index);
        expected_index = 2;
    }
    auto score = parse_score ("call status\n");
    ASSERT_TRUE (score.has_value());
    for (double seconds : { -1.0,
                            std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN(),
                            1e30 }) {
        Engine         engine (test_config());
        OfflineBackend backend (engine.config());
        EXPECT_FALSE (render_score (engine, *score, backend, { .duration_seconds = seconds }));
    }
}

TEST (Offline, ReportsLostNotificationsInsteadOfSilentlyIgnoringPotentialErrors) {
    auto config           = test_config();
    config.max_nodes      = 1;
    config.queue_capacity = 1;
    Engine engine (config);
    for (std::uint32_t seq = 1; seq <= 4; ++seq) {
        const proto::Bundle bundle{ 0,
                                    { proto::GroupNew{ 0x01000001, proto::AddAction::head, 0 },
                                      proto::NodeFree{ { 0x01000001 } } } };
        ASSERT_TRUE (engine.submit (1, seq, bundle));
        engine.process ({}, 0);
        engine.collect();
    }
    ASSERT_GT (engine.status().events_dropped, 0U);
    auto score = parse_score ("call status\n");
    ASSERT_TRUE (score.has_value());
    OfflineBackend backend (config);
    const auto     result = render_score (engine, *score, backend, { .duration_seconds = 0.0 });
    ASSERT_FALSE (result.has_value());
    EXPECT_NE (result.error().message.find ("notification"), std::string::npos);
}

TEST (Offline, WritesFloatWaveHeaderAndAudioAndReportsIoErrors) {
    const auto     path = temp_path (".wav");
    Engine         engine (test_config());
    OfflineBackend backend (engine.config(), path);
    const auto     score = parse_score (
        constant_score ("@now node.new id=0x01000001 def=1 add=head target=0 $amp=0.5\n")
    );
    ASSERT_TRUE (score.has_value());
    const auto rendered = render_score (engine, *score, backend, { .duration_seconds = 0.001 });
    ASSERT_TRUE (rendered.has_value()) << rendered.error().message;
    const auto bytes = read_bytes (path);
    ASSERT_EQ (bytes.size(), 108U);
    EXPECT_EQ (u32 (bytes, 4), 100U);
    EXPECT_EQ (u32 (bytes, 16), 16U);
    EXPECT_EQ (std::to_integer<unsigned> (bytes[20]), 3U);
    EXPECT_EQ (std::to_integer<unsigned> (bytes[22]), 2U);
    EXPECT_EQ (u32 (bytes, 24), 8000U);
    EXPECT_EQ (u32 (bytes, 40), 64U);
    EXPECT_FLOAT_EQ (std::bit_cast<float> (u32 (bytes, 44)), 0.5F);
    std::filesystem::remove (path);
    Engine         failed_engine (test_config());
    OfflineBackend failed_backend (failed_engine.config(), path / "missing.wav");
    EXPECT_FALSE (
        render_score (failed_engine, *score, failed_backend, { .duration_seconds = 0.001 })
    );
}

TEST (Offline, ZeroDurationWritesAnEmptyValidWaveAndDefaultDurationAddsATail) {
    const auto path  = temp_path (".wav");
    auto       score = parse_score ("call status\n");
    ASSERT_TRUE (score.has_value());
    Engine         engine (test_config());
    OfflineBackend backend (engine.config(), path);
    ASSERT_TRUE (render_score (engine, *score, backend, { .duration_seconds = 0.0 }));
    const auto bytes = read_bytes (path);
    ASSERT_EQ (bytes.size(), 44U);
    EXPECT_EQ (u32 (bytes, 4), 36U);
    EXPECT_EQ (u32 (bytes, 40), 0U);
    EXPECT_EQ (engine.frame(), 0U);
    std::filesystem::remove (path);

    score = parse_score ("@+0.001 group.clear id=0\n");
    ASSERT_TRUE (score.has_value());
    Engine         tail_engine (test_config());
    OfflineBackend memory (tail_engine.config());
    const auto     result = render_score (tail_engine, *score, memory);
    ASSERT_TRUE (result.has_value());
    EXPECT_EQ (result->frames, 8008U);
}

TEST (Offline, RejectsTooShortDurationBackendMismatchAndBrokenOutputWrites) {
    auto score = parse_score ("@+1 group.clear id=0\n");
    ASSERT_TRUE (score.has_value());
    Engine         engine (test_config());
    OfflineBackend backend (engine.config());
    EXPECT_FALSE (render_score (engine, *score, backend, { .duration_seconds = 0.5 }));
    EXPECT_TRUE (backend.samples().empty());
    auto           mismatch = test_config (1);
    OfflineBackend mono (mismatch);
    EXPECT_FALSE (render_score (engine, *score, mono));
    const std::array<float, 1> incomplete_frame{ 1.0F };
    EXPECT_FALSE (backend.write (incomplete_frame));
    ASSERT_TRUE (backend.finish());
    EXPECT_FALSE (backend.write ({}));

    if (std::filesystem::exists ("/dev/full")) {
        score = parse_score ("call status\n");
        ASSERT_TRUE (score.has_value());
        OfflineBackend failed (engine.config(), "/dev/full");
        EXPECT_FALSE (render_score (engine, *score, failed, { .duration_seconds = 0.001 }));
    }
}

TEST (Offline, ExplicitFileOutputWithAnEmptyPathFails) {
    OfflineBackend backend (test_config(), std::filesystem::path{});
    EXPECT_FALSE (backend.finish());
}

TEST (Offline, DurationUsesNanosecondResolutionWithoutSpuriousExtraFrames) {
    const auto score = parse_score ("call status\n");
    ASSERT_TRUE (score.has_value());
    for (const auto& [seconds, frames] : { std::pair{ 0.07, 3360U },
                                           std::pair{ 0.14, 6720U },
                                           std::pair{ 0.070001, 3361U },
                                           std::pair{ 0.000000001, 1U } }) {
        auto config        = test_config();
        config.sample_rate = 48000;
        Engine         engine (config);
        OfflineBackend backend (config);
        const auto result = render_score (engine, *score, backend, { .duration_seconds = seconds });
        ASSERT_TRUE (result.has_value()) << result.error().message;
        EXPECT_EQ (result->frames, frames) << seconds;
    }
    auto config        = test_config();
    config.sample_rate = 1;
    Engine         engine (config);
    OfflineBackend backend (config);
    const auto     result = render_score (engine, *score, backend, { .duration_seconds = 86400.0 });
    ASSERT_TRUE (result.has_value());
    EXPECT_EQ (result->frames, 86400U);
}

TEST (Offline, BracesInBareGraphPathsDoNotDelayParameterNameContextUpdates) {
    const auto directory = temp_path ("-dir");
    std::filesystem::create_directory (directory);
    {
        std::ofstream graph (directory / "tone{.bvg");
        graph << "graph constant\nparam amp 0.25\nout.ar 0 $amp $amp\n";
    }
    const auto score = parse_score (
        "call def.load graph=@tone{.bvg\n"
        "@now node.new id=0x01000001 def=1 add=head target=0 $amp=0.5\n",
        directory / "score.bvs"
    );
    ASSERT_TRUE (score.has_value()) << score.error().message;
    ASSERT_EQ (score->messages.size(), 2U);
    const auto& bundle = std::get<proto::Bundle> (score->messages.back().body);
    EXPECT_EQ (std::get<proto::NodeNew> (bundle.commands.front()).params.front().index, 0U);
    std::filesystem::remove_all (directory);
}
