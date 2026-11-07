// libFuzzer target: binary decoding. besides "no crash" it checks the invariants that
// make the codec trustworthy: a message that decodes encodes back to the same bytes,
// and the disassembler accepts anything.

#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "bitvm/proto/codec.hpp"
#include "bitvm/proto/graphdef.hpp"
#include "bitvm/proto/text.hpp"
#include "bitvm/proto/validate.hpp"

using namespace bitvm::proto;

extern "C" int
LLVMFuzzerTestOneInput (const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes (reinterpret_cast<const std::byte*> (data), size);

    if (const auto message = decode (bytes)) {
        const auto again = encode (*message);
        if (!again || *again != std::vector<std::byte> (bytes.begin(), bytes.end())) {
            std::abort();
        }
        if (const auto* bundle = std::get_if<Bundle> (&message->body)) {
            static_cast<void> (validate (*bundle));
        }
        if (const auto* call = std::get_if<Call> (&message->body)) {
            if (const auto* load = std::get_if<DefLoad> (&call->command)) {
                static_cast<void> (load_graph (load->graph));
            }
        }
    }

    const TextContext ctx;
    static_cast<void> (disassemble (bytes, ctx));

    // the same bytes as a graph
    if (const auto graph = decode_graph (bytes)) {
        static_cast<void> (validate (*graph));
        static_cast<void> (disassemble_graph (*graph));
        // decode accepts dangling type and constant indices (the validator refuses them);
        // canonicalize and encode_graph refuse them too. what canonicalizes must roundtrip.
        if (const auto canon = canonicalize (*graph)) {
            const auto encoded = encode_graph (*graph);
            if (!encoded) {
                std::abort();
            }
            const auto back = decode_graph (*encoded);
            if (!back || *back != *canon) {
                std::abort();
            }
        }
    }
    return 0;
}
