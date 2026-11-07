// libFuzzer target: text assembler and .bvg parser. the first input byte picks the
// parser. what parses must print and parse again to the same value.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "bitvm/proto/text.hpp"

using namespace bitvm::proto;

namespace
{

TextContext
make_context () {
    TextContext ctx;
    ctx.time_base_ns = 1'000'000;
    ctx.param_names  = [] (std::uint32_t def) -> const std::vector<std::string>* {
        static const std::vector<std::string> kNames = { "freq", "amp" };
        return def % 2 == 1 ? &kNames : nullptr;
    };
    ctx.read_file = [] (std::string_view) -> std::expected<std::string, std::string> {
        return std::string ("graph f\nin.ar 0\n");
    };
    return ctx;
}

void
check_messages (std::string_view text) {
    auto ctx      = make_context();
    auto messages = assemble (text, ctx);
    if (!messages) {
        return;
    }
    const auto  ctx2 = make_context();
    std::string printed;
    for (const auto& m : *messages) {
        printed += disassemble (m, ctx2);
    }
    auto ctx3  = make_context();
    auto again = assemble (printed, ctx3);
    if (!again || *again != *messages) {
        std::abort();
    }
}

void
check_graph (std::string_view text) {
    auto graph = assemble_graph (text);
    if (!graph) {
        return;
    }
    auto again = assemble_graph (disassemble_graph (*graph));
    if (!again || *again != *graph) {
        std::abort();
    }
}

}  // namespace

extern "C" int
LLVMFuzzerTestOneInput (const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::string_view text (reinterpret_cast<const char*> (data) + 1, size - 1);
    switch (data[0] % 3) {
        case 0:
            check_messages (text);
            break;
        case 1:
            check_graph (text);
            break;
        default:
            if (const auto bytes = base64_decode (text)) {
                if (base64_encode (*bytes) != text) {
                    std::abort();
                }
            }
            break;
    }
    return 0;
}
