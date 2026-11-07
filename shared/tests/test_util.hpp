#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "bitvm/proto/codec.hpp"
#include "bitvm/proto/messages.hpp"
#include "bitvm/proto/types.hpp"

namespace bitvm::proto::testing
{

using Bytes = std::vector<std::byte>;

inline Bytes
bytes_of (std::initializer_list<int> values) {
    Bytes out;
    for (const int v : values) {
        out.push_back (static_cast<std::byte> (v));
    }
    return out;
}

// "01 ff 0A" -> bytes; spaces and newlines are ignored
inline Bytes
from_hex (const std::string& text) {
    Bytes out;
    int   nibble = -1;
    for (const char c : text) {
        int v = -1;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            continue;
        }
        if (nibble < 0) {
            nibble = v;
        } else {
            out.push_back (static_cast<std::byte> (nibble * 16 + v));
            nibble = -1;
        }
    }
    return out;
}

inline std::string
to_hex (const Bytes& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           out;
    for (const auto b : bytes) {
        out += kDigits[(static_cast<unsigned> (b) >> 4) & 0xF];
        out += kDigits[static_cast<unsigned> (b) & 0xF];
    }
    return out;
}

inline std::string
read_text_file (const std::string& name) {
    std::ifstream      in (std::string (BITVM_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline void
write_u32 (Bytes& b, std::size_t pos, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        b[pos + i] = static_cast<std::byte> ((v >> (8 * i)) & 0xFF);
    }
}

inline void
write_u16 (Bytes& b, std::size_t pos, std::uint16_t v) {
    b[pos]     = static_cast<std::byte> (v & 0xFF);
    b[pos + 1] = static_cast<std::byte> (v >> 8);
}

// sets the header length field to the real size
inline void
fix_length (Bytes& b) {
    write_u32 (b, 0, static_cast<std::uint32_t> (b.size()));
}

inline Bytes
encode_ok (const Message& m) {
    auto bytes = encode (m);
    if (!bytes) {
        return {};
    }
    return *bytes;
}

// one message of every kind, command, call, result and event, with edge values.
// order is stable: golden files depend on `canonical_samples()` below.
std::vector<Message> edge_samples ();

// readable canonical messages that the golden files in tests/data describe
std::vector<Message> canonical_samples ();

}  // namespace bitvm::proto::testing
