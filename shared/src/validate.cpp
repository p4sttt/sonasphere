#include "bitvm/proto/validate.hpp"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace bitvm::proto
{
namespace
{

std::unexpected<ValidationError>
invalid (std::string_view detail) {
    return std::unexpected (ValidationError{ ErrorCode::invalid_arg, kNoCommand, detail });
}

std::expected<void, ValidationError>
check (const NodeNew& c) {
    if (c.target == c.id) {
        return invalid ("node.new: target equals id");
    }
    if (c.params.size() > kMaxParams) {
        return invalid ("node.new: too many params");
    }
    return {};
}

std::expected<void, ValidationError>
check (const GroupNew& c) {
    if (c.target == c.id) {
        return invalid ("group.new: target equals id");
    }
    return {};
}

std::expected<void, ValidationError>
check (const NodeFree& c) {
    if (c.ids.empty()) {
        return invalid ("node.free: empty id list");
    }
    if (std::ranges::find (c.ids, kRootGroup) != c.ids.end()) {
        return invalid ("node.free: root group cannot be freed");
    }
    return {};
}

std::expected<void, ValidationError>
check (const NodeSet& c) {
    // written so that nan fails too, although decode never lets one through
    if (!(c.ramp >= 0.0F && c.ramp <= kMaxRamp)) {
        return invalid ("node.set: ramp outside 0..3600");
    }
    if (c.params.size() > kMaxParams) {
        return invalid ("node.set: too many params");
    }
    return {};
}

std::expected<void, ValidationError>
check (const NodeMove& c) {
    if (c.add == AddAction::replace) {
        return invalid ("node.move: replace is not allowed");
    }
    if (c.target == c.id) {
        return invalid ("node.move: target equals id");
    }
    return {};
}

std::expected<void, ValidationError>
check (const CbusSet& c) {
    if (c.values.empty()) {
        return invalid ("cbus.set: empty value list");
    }
    return {};
}

std::expected<void, ValidationError>
check (const NodeRun&) {
    return {};
}

std::expected<void, ValidationError>
check (const GroupClear&) {
    return {};
}

}  // namespace

std::expected<void, ValidationError>
validate (const RtCommand& command) {
    return std::visit (
        [] (const auto& c) {
            return check (c);
        },
        command
    );
}

std::expected<void, ValidationError>
validate (const Bundle& bundle) {
    if (bundle.time < 0) {
        return std::unexpected (
            ValidationError{ ErrorCode::invalid_arg, kNoCommand, "bundle: negative time" }
        );
    }
    if (bundle.commands.empty()) {
        return std::unexpected (
            ValidationError{ ErrorCode::invalid_arg, kNoCommand, "bundle: no commands" }
        );
    }
    for (std::size_t i = 0; i < bundle.commands.size(); ++i) {
        if (auto ok = validate (bundle.commands[i]); !ok) {
            auto error  = ok.error();
            error.index = static_cast<std::uint16_t> (std::min<std::size_t> (i, 0xFFFE));
            return std::unexpected (error);
        }
    }
    return {};
}

}  // namespace bitvm::proto
