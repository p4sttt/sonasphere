#pragma once

#include "bitvm/proto/graphdef.hpp"

namespace bitvm::proto::testing
{

// the `ping` example from docs/protocol.md section 15 in canonical form
inline GraphDef
ping_graph () {
    GraphDef g;
    g.name       = "ping";
    g.params     = { { "freq", 440.0F }, { "amp", 0.2F } };
    g.consts     = { 0.0F, 0.005F, 0.4F, 1.0F, -4.0F };
    g.types      = { "sin_osc", "env_perc", "mul", "out" };
    const auto c = [] (std::uint16_t i) {
        return GraphInput{ InputSource::constant, i, 0 };
    };
    const auto p = [] (std::uint16_t i) {
        return GraphInput{ InputSource::param, i, 0 };
    };
    const auto u = [] (std::uint16_t i, std::uint8_t o = 0) {
        return GraphInput{ InputSource::unit, i, o };
    };
    g.units = {
        { 0, Rate::ar, 1, { p (0), c (0) } },
        { 1, Rate::kr, 1, { c (1), c (2), c (3), c (4), c (3) } },
        { 2, Rate::ar, 1, { u (0), u (1) } },
        { 2, Rate::ar, 1, { u (2), p (1) } },
        { 3, Rate::ar, 0, { c (0), u (3), u (3) } },
    };
    return g;
}

// the same graph written by hand, byte by byte
inline const char*
ping_hex () {
    return "42564744 0100"                                      // magic, version
           "0400 70696e67"                                      // name
           "0200 0400 66726571 0000dc43 0300 616d70 cdcc4c3e"   // params
           "0500 00000000 0ad7a33b cdcccc3e 0000803f 000080c0"  // consts
           "0400 0700 73696e5f6f7363 0800 656e765f70657263 0300 6d756c 0300 6f7574"  // types
           "0500"                                                                    // units
           "0000 02 01 02  01 0000 00  00 0000 00"
           "0100 01 01 05  00 0100 00  00 0200 00  00 0300 00  00 0400 00  00 0300 00"
           "0200 02 01 02  02 0000 00  02 0100 00"
           "0200 02 01 02  02 0200 00  01 0100 00"
           "0300 02 00 03  00 0000 00  02 0300 00  02 0300 00";
}

}  // namespace bitvm::proto::testing
