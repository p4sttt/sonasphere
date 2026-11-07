#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitvm/buses.hpp"
#include "bitvm/graph.hpp"
#include "bitvm/proto/catalog.hpp"
#include "bitvm/proto/text.hpp"

namespace bitvm
{
namespace
{

std::shared_ptr<const CompiledGraph>
compile_text (std::string_view source) {
    auto graph = proto::assemble_graph (source);
    if (!graph) {
        ADD_FAILURE() << proto::to_string (graph.error());
        return {};
    }
    auto compiled = CompiledGraph::compile (*graph);
    if (!compiled) {
        ADD_FAILURE() << compiled.error().message;
        return {};
    }
    return *compiled;
}

TEST (Buses, ClearsAudioAndPreservesControl) {
    Buses buses (2, 2, 4);
    buses.add_audio (1, 3, 0.25F);
    buses.add_audio (1, 3, 0.5F);
    buses.write_control (1, 3.0F);
    EXPECT_FLOAT_EQ (buses.read_audio (1, 3), 0.75F);
    EXPECT_FLOAT_EQ (buses.read_audio (1, 2), 0.0F);
    buses.clear_audio();
    EXPECT_FLOAT_EQ (buses.read_audio (1, 3), 0.0F);
    EXPECT_FLOAT_EQ (buses.read_control (1), 3.0F);
}

TEST (Buses, IgnoresOutOfRangeIndicesAndKeepsFiniteSums) {
    Buses buses (1, 1, 2);
    buses.add_audio (1, 0, 10.0F);
    buses.add_audio (0, 2, 10.0F);
    buses.write_control (1, 10.0F);
    EXPECT_FLOAT_EQ (buses.read_audio (1, 0), 0.0F);
    EXPECT_FLOAT_EQ (buses.read_audio (0, 2), 0.0F);
    EXPECT_FLOAT_EQ (buses.read_control (1), 0.0F);
    buses.add_audio (0, 0, std::numeric_limits<float>::max());
    buses.add_audio (0, 0, std::numeric_limits<float>::max());
    EXPECT_TRUE (std::isfinite (buses.read_audio (0, 0)));
}

TEST (CompiledGraph, RejectsInvalidTopologyAndUnknownUnits) {
    proto::GraphDef graph;
    graph.name  = "invalid";
    graph.types = { "missing" };
    graph.units = { { 0, proto::Rate::ar, 1, {} } };
    EXPECT_FALSE (CompiledGraph::compile (graph));
    graph.types           = { "add" };
    graph.units[0].inputs = { { proto::InputSource::unit, 0, 0 },
                              { proto::InputSource::unit, 0, 0 } };
    EXPECT_FALSE (CompiledGraph::compile (graph));
}

TEST (GraphInstance, ImplementsEveryCatalogEntry) {
    constexpr std::array<std::string_view, 9> definitions = {
        "add.ar 3 2",  "sub.ar 3 2", "mul.ar 3 2", "div.ar 3 2", "sin_osc.ar",
        "env_perc.ar", "env_asr.ar", "out.ar 0 1", "in.ar 0"
    };
    ASSERT_EQ (proto::catalog().size(), definitions.size());
    for (const auto definition : definitions) {
        SCOPED_TRACE (definition);
        EXPECT_NE (
            compile_text (std::string ("graph entry\n") + std::string (definition)),
            nullptr
        );
    }
}

TEST (GraphInstance, ArithmeticAtEveryRateAndZeroDivision) {
    constexpr std::array<std::string_view, 5> expressions = { "add", "sub", "mul", "div", "div" };
    constexpr std::array<float, 5>            results     = { 5.0F, 1.0F, 6.0F, 1.5F, 0.0F };
    for (const auto rate : { "ir", "kr", "ar" }) {
        for (std::size_t index = 0; index < expressions.size(); ++index) {
            SCOPED_TRACE (std::string (expressions[index]) + "." + rate);
            const auto source   = std::string ("graph arithmetic\n%value = ") +
                                  std::string (expressions[index]) + "." + rate + " 3 " +
                                  (index == 4 ? "0" : "2") + "\nout.ar 0 %value";
            auto       compiled = compile_text (source);
            ASSERT_NE (compiled, nullptr);
            GraphInstance instance (compiled, 48000, 4);
            Buses         buses (1, 0, 4);
            for (std::uint16_t offset = 0; offset < 4; ++offset) {
                EXPECT_FALSE (instance.process (buses, offset));
                EXPECT_FLOAT_EQ (buses.read_audio (0, offset), results[index]);
            }
        }
    }
}

TEST (GraphInstance, InitUnitsUseConstructorParameterValues) {
    auto compiled = compile_text (
        "graph init\nparam value 2\n%a = add.ir $value 1\n"
        "%b = mul.ir %a 2\nout.ar 0 %b"
    );
    ASSERT_NE (compiled, nullptr);
    const std::array<proto::Pair, 1> params = { proto::Pair{ 0, 3.0F } };
    GraphInstance                    instance (compiled, 48000, 4, params);
    EXPECT_EQ (instance.param_count(), 1);
    EXPECT_EQ (compiled->graph().params[0].name, "value");
    instance.set_param (0, 20.0F, 0);
    Buses buses (1, 0, 4);
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 8.0F);
}

TEST (GraphInstance, AudioSineUsesLatchedInitialPhase) {
    auto compiled = compile_text (
        "graph sine\nparam phase 0.25\n"
        "%signal = sin_osc.ar 2 $phase\nout.ar 0 %signal"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 8, 4);
    instance.set_param (0, 0.0F, 0);
    Buses                          buses (1, 0, 4);
    constexpr std::array<float, 4> expected = { 1, 0, -1, 0 };
    for (std::uint16_t offset = 0; offset < 4; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        EXPECT_NEAR (buses.read_audio (0, offset), expected[offset], 1e-6F);
    }
}

TEST (GraphInstance, ControlSineAdvancesOnlyByActiveFramesAfterMidBlockStart) {
    auto compiled = compile_text ("graph sine\n%signal = sin_osc.kr 1\nout.ar 0 %signal");
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 8, 4);
    Buses         buses (1, 0, 4);
    EXPECT_FALSE (instance.process (buses, 2));
    EXPECT_FALSE (instance.process (buses, 3));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 2), 0.0F);
    EXPECT_FLOAT_EQ (buses.read_audio (0, 3), 0.0F);
    buses.clear_audio();
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_NEAR (buses.read_audio (0, 0), 1.0F, 1e-6F);
    EXPECT_FALSE (instance.process (buses, 1));
    EXPECT_NEAR (buses.read_audio (0, 1), 1.0F, 1e-6F);
}

TEST (GraphInstance, ControlReadsKeepFirstAudioSampleForTheWholeBlock) {
    auto compiled = compile_text (
        "graph rate\n%gate = sin_osc.ar 1 0.25\n"
        "%env = env_asr.ar %gate 0 1 0 0\nout.ar 0 %env"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 8, 4);
    Buses         buses (1, 0, 4);
    for (std::uint16_t offset = 0; offset < 4; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        EXPECT_FLOAT_EQ (buses.read_audio (0, offset), 1.0F);
    }
    buses.clear_audio();
    for (std::uint16_t offset = 0; offset < 4; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        EXPECT_FLOAT_EQ (buses.read_audio (0, offset), 0.0F);
    }
}

TEST (GraphInstance, BusAddressUsesItsControlReadRate) {
    auto compiled = compile_text (
        "graph bus\nparam bus 0\n%address = add.ar $bus 0\n"
        "out.ar %address 1"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 48000, 4);
    Buses         buses (2, 0, 4);
    EXPECT_FALSE (instance.process (buses, 0));
    instance.set_param (0, 1.0F, 0);
    for (std::uint16_t offset = 1; offset < 4; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        EXPECT_FLOAT_EQ (buses.read_audio (0, offset), 1.0F);
        EXPECT_FLOAT_EQ (buses.read_audio (1, offset), 0.0F);
    }
    buses.clear_audio();
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (1, 0), 1.0F);
}

TEST (GraphInstance, AudioReadsOfParametersStayConstantWithinTheBlock) {
    auto compiled = compile_text (
        "graph parameter\nparam value 0.25\n%double = mul.ar $value 2\n"
        "out.ar 0 $value %double"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 48000, 4);
    Buses         buses (2, 0, 4);
    EXPECT_FALSE (instance.process (buses, 0));
    instance.set_param (0, 1.0F, 0);
    for (std::uint16_t offset = 1; offset < 4; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        EXPECT_FLOAT_EQ (buses.read_audio (0, offset), 0.25F);
        EXPECT_FLOAT_EQ (buses.read_audio (1, offset), 0.5F);
    }
    buses.clear_audio();
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 1.0F);
    EXPECT_FLOAT_EQ (buses.read_audio (1, 0), 2.0F);
}

TEST (GraphInstance, ResumingAfterAPausedBlockRefreshesControlWithoutAdvancingPausedTime) {
    auto compiled = compile_text (
        "graph resume\n%control = in.kr 0\n%osc = sin_osc.kr 1\nout.ar 0 %control %osc"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 8, 4);
    Buses         buses (2, 1, 4);
    buses.write_control (0, 1.0F);
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FALSE (instance.process (buses, 1));
    buses.clear_audio();
    buses.write_control (0, 3.0F);
    EXPECT_FALSE (instance.process (buses, 2));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 2), 3.0F);
    EXPECT_NEAR (buses.read_audio (1, 2), 1.0F, 1e-6F);
    EXPECT_FALSE (instance.process (buses, 3));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 3), 3.0F);
    EXPECT_NEAR (buses.read_audio (1, 3), 1.0F, 1e-6F);
    buses.clear_audio();
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_NEAR (buses.read_audio (1, 0), 0.0F, 1e-6F);
}

TEST (GraphInstance, RampsAdvanceOnlyForActiveFramesAndExposeControlSnapshots) {
    auto compiled = compile_text ("graph ramp\nparam value 0\nout.ar 0 $value");
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 48000, 4);
    Buses         buses (1, 0, 4);
    instance.set_param (0, 1.0F, 8);
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FALSE (instance.process (buses, 1));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.125F);
    EXPECT_FLOAT_EQ (buses.read_audio (0, 1), 0.125F);
    buses.clear_audio();
    for (std::uint16_t offset = 0; offset < 4; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        EXPECT_FLOAT_EQ (buses.read_audio (0, offset), 0.375F);
    }
    buses.clear_audio();
    for (std::uint16_t offset = 0; offset < 4; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        EXPECT_FLOAT_EQ (buses.read_audio (0, offset), 0.875F);
    }
    buses.clear_audio();
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 1.0F);
    instance.set_param (0, 0.0F, 8);
    buses.clear_audio();
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.875F);
    instance.set_param (0, 0.5F, 2);
    for (std::uint16_t offset = 1; offset < 4; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        EXPECT_FLOAT_EQ (buses.read_audio (0, offset), 0.875F);
    }
    buses.clear_audio();
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.5F);
    instance.set_param (99, 20.0F, 0);
}

TEST (GraphInstance, PercussionHasExactEndpointsAndDone) {
    auto compiled = compile_text (
        "graph perc\n%env = env_perc.ar 0.002 0.002 1 0 1\n"
        "out.ar 0 %env"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance                  instance (compiled, 1000, 8);
    Buses                          buses (1, 0, 8);
    constexpr std::array<float, 5> expected = { 0, 0.5F, 1, 0.5F, 0 };
    for (std::uint16_t offset = 0; offset < expected.size(); ++offset) {
        EXPECT_EQ (instance.process (buses, offset), offset == 4);
        EXPECT_FLOAT_EQ (buses.read_audio (0, offset), expected[offset]);
    }
}

TEST (GraphInstance, PercussionCurveMatchesTheSpecifiedExponential) {
    auto compiled = compile_text (
        "graph perc\n%env = env_perc.ar 0.002 0.002 1 -4\n"
        "out.ar 0 %env"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 1000, 8);
    Buses         buses (1, 0, 8);
    for (std::uint16_t offset = 0; offset < 5; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
    }
    EXPECT_NEAR (buses.read_audio (0, 1), 0.88079708F, 1e-6F);
    EXPECT_NEAR (buses.read_audio (0, 3), 0.11920292F, 1e-6F);
}

TEST (GraphInstance, EnvelopeInitInputsCannotBeChangedAfterConstruction) {
    auto compiled = compile_text (
        "graph perc\nparam attack 0.002\nparam release 0.002\nparam level 1\nparam done 1\n"
        "%env = env_perc.ar $attack $release $level 0 $done\nout.ar 0 %env"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 1000, 8);
    instance.set_param (0, 0.0F, 0);
    instance.set_param (1, 0.0F, 0);
    instance.set_param (2, 10.0F, 0);
    instance.set_param (3, 0.0F, 0);
    Buses buses (1, 0, 8);
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.0F);
    EXPECT_FALSE (instance.process (buses, 1));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 1), 0.5F);
    EXPECT_FALSE (instance.process (buses, 2));
    EXPECT_FALSE (instance.process (buses, 3));
    EXPECT_TRUE (instance.process (buses, 4));
}

TEST (GraphInstance, AsrReleasesAndRetriggersFromItsCurrentValue) {
    auto compiled = compile_text (
        "graph asr\nparam gate 1\n%env = env_asr.ar $gate 0.002 1 0.004 1\nout.ar 0 %env"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 1000, 1);
    Buses         buses (1, 0, 1);
    const auto    sample = [&] () {
        buses.clear_audio();
        const bool done = instance.process (buses, 0);
        return std::pair{ buses.read_audio (0, 0), done };
    };
    EXPECT_FLOAT_EQ (sample().first, 0.0F);
    EXPECT_FLOAT_EQ (sample().first, 0.5F);
    EXPECT_FLOAT_EQ (sample().first, 1.0F);
    instance.set_param (0, 0.0F, 0);
    EXPECT_FLOAT_EQ (sample().first, 1.0F);
    EXPECT_FLOAT_EQ (sample().first, 0.75F);
    instance.set_param (0, 1.0F, 0);
    EXPECT_FLOAT_EQ (sample().first, 0.5F);
    EXPECT_FLOAT_EQ (sample().first, 0.75F);
    EXPECT_FLOAT_EQ (sample().first, 1.0F);
    instance.set_param (0, 0.0F, 0);
    EXPECT_FALSE (sample().second);
    EXPECT_FALSE (sample().second);
    EXPECT_FALSE (sample().second);
    EXPECT_FALSE (sample().second);
    const auto finished = sample();
    EXPECT_TRUE (finished.second);
    EXPECT_FLOAT_EQ (finished.first, 0.0F);
}

TEST (GraphInstance, NonPositiveInitialAsrGateEndsImmediately) {
    auto compiled = compile_text ("graph asr\n%env = env_asr.ar -1 0.1 1 0.1 1\nout.ar 0 %env");
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 48000, 4);
    Buses         buses (1, 0, 4);
    EXPECT_TRUE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.0F);
}

TEST (GraphInstance, ZeroLengthPercussionEndsImmediately) {
    auto compiled = compile_text ("graph empty\n%env = env_perc.ar 0 0 1 0 1\nout.ar 0 %env");
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 48000, 4);
    Buses         buses (1, 0, 4);
    EXPECT_TRUE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.0F);
}

TEST (GraphInstance, ControlPercussionHoldsItsOutputAndEndsOnTheNextControlFrame) {
    auto compiled =
        compile_text ("graph perc\n%env = env_perc.kr 0.002 0.002 1 0 1\nout.ar 0 %env");
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 1000, 2);
    Buses         buses (1, 0, 2);
    EXPECT_FALSE (instance.process (buses, 1));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 1), 0.0F);
    buses.clear_audio();
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FALSE (instance.process (buses, 1));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.5F);
    EXPECT_FLOAT_EQ (buses.read_audio (0, 1), 0.5F);
    buses.clear_audio();
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FALSE (instance.process (buses, 1));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.5F);
    EXPECT_FLOAT_EQ (buses.read_audio (0, 1), 0.5F);
    buses.clear_audio();
    EXPECT_TRUE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.0F);
}

TEST (GraphInstance, InitReadsLatchAnUpstreamAudioUnitAtTheFirstFrame) {
    auto compiled = compile_text (
        "graph phase\nparam phase 0.25\n%phase = add.ar $phase 0\n"
        "%osc = sin_osc.ar 0 %phase\nout.ar 0 %osc"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 48000, 4);
    Buses         buses (1, 0, 4);
    EXPECT_FALSE (instance.process (buses, 0));
    instance.set_param (0, 0.0F, 0);
    for (std::uint16_t offset = 1; offset < 4; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        EXPECT_FLOAT_EQ (buses.read_audio (0, offset), 1.0F);
    }
    buses.clear_audio();
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 1.0F);
}

TEST (GraphInstance, RoutesAllSixtyFourChannelsAtTheCatalogBoundary) {
    std::string source = "graph channels\nout.ar 0";
    for (std::size_t channel = 0; channel < 64; ++channel) {
        source += " 0.25";
    }
    source += "\n%read:64 = in.ar 0\nout.ar 64 %read.63";
    auto compiled = compile_text (source);
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 48000, 4);
    Buses         buses (65, 0, 4);
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (63, 0), 0.25F);
    EXPECT_FLOAT_EQ (buses.read_audio (64, 0), 0.25F);
}

TEST (GraphInstance, MultichannelAudioReadsEarlierWritesAndSumsOutputs) {
    auto compiled = compile_text (
        "graph routing\nout.ar 0.6 0.25 0.5\n%read:3 = in.ar 1\n"
        "out.ar 0 %read %read.1 %read.2\nout.ar 0 0.25"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 48000, 4);
    Buses         buses (3, 0, 4);
    EXPECT_FALSE (instance.process (buses, 0));
    EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.5F);
    EXPECT_FLOAT_EQ (buses.read_audio (1, 0), 0.75F);
    EXPECT_FLOAT_EQ (buses.read_audio (2, 0), 0.5F);
}

TEST (GraphInstance, ControlOutputsOverwriteAndPersistBetweenBlocks) {
    auto compiled = compile_text (
        "graph control\nout.kr 0 0.25 0.5\nout.kr 0 0.75\n%read:2 = in.kr 0\n"
        "out.ar 0 %read %read.1"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 48000, 4);
    Buses         buses (2, 2, 4);
    for (std::uint16_t offset = 0; offset < 4; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        EXPECT_FLOAT_EQ (buses.read_audio (0, offset), 0.75F);
        EXPECT_FLOAT_EQ (buses.read_audio (1, offset), 0.5F);
    }
    buses.clear_audio();
    EXPECT_FLOAT_EQ (buses.read_control (0), 0.75F);
    EXPECT_FLOAT_EQ (buses.read_control (1), 0.5F);
}

TEST (GraphInstance, OutOfRangeBusNumbersReadSilenceAndSkipWrites) {
    for (const auto address : { "-1", "-0.1", "3.4028235e38" }) {
        SCOPED_TRACE (address);
        auto compiled = compile_text (
            std::string ("graph bounds\nout.ar ") + address + " 1\n%read = in.ar " + address +
            "\nout.ar 0 %read"
        );
        ASSERT_NE (compiled, nullptr);
        GraphInstance instance (compiled, 48000, 4);
        Buses         buses (1, 0, 4);
        EXPECT_FALSE (instance.process (buses, 0));
        EXPECT_FLOAT_EQ (buses.read_audio (0, 0), 0.0F);
    }
}

TEST (GraphInstance, ExtremeFiniteInputsCannotProduceNonFiniteOutput) {
    auto compiled = compile_text (
        "graph extreme\n%sum = add.ar 3.4028235e38 3.4028235e38\n"
        "%product = mul.ar %sum %sum\n%quotient = div.ar %product 1.40129846e-45\n"
        "%osc = sin_osc.ar %quotient -3.4028235e38\n"
        "%env = env_perc.ar 0.002 0.002 3.4028235e38 3.4028235e38\n"
        "out.ar 0 %sum %product %quotient %osc %env"
    );
    ASSERT_NE (compiled, nullptr);
    GraphInstance instance (compiled, 1000, 8);
    Buses         buses (5, 0, 8);
    for (std::uint16_t offset = 0; offset < 8; ++offset) {
        EXPECT_FALSE (instance.process (buses, offset));
        for (std::uint32_t bus = 0; bus < 5; ++bus) {
            EXPECT_TRUE (std::isfinite (buses.read_audio (bus, offset)));
        }
    }
}

}  // namespace
}  // namespace bitvm
