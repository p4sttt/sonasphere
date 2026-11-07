#pragma once

/// @file
/// @brief Level 1 static checks of a bundle (docs/protocol.md section 9).
///
/// Responsibility: the meaning of a bundle that does not depend on the state of the server.
/// sona runs these checks before sending, bitvm after decoding, so a generator bug is caught
/// early and the server drops the same bundles the client would have refused.
///
/// @note These are free functions on purpose. The message structures are plain data (see
/// `messages.hpp`) and the rules are a separate concern with their own reason to change;
/// members such as `Bundle::validate()` would tie the data module to this one and put
/// behaviour into every aggregate. Non-member functions also work on a bare `RtCommand`.

#include <cstdint>
#include <expected>
#include <string_view>

#include "bitvm/proto/messages.hpp"
#include "bitvm/proto/types.hpp"

namespace bitvm::proto
{

/// longest allowed `node.set` ramp, in seconds
inline constexpr float kMaxRamp = 3600.0F;

/// @brief the first rule a bundle breaks; the code is always `invalid_arg`
struct ValidationError {
    ErrorCode        code  = ErrorCode::invalid_arg;
    std::uint16_t    index = kNoCommand;  ///< command index in the bundle
    std::string_view detail;              ///< static text, never owned
};

/// @brief checks one command; the index in the error is `kNoCommand`.
[[nodiscard]] std::expected<void, ValidationError> validate (const RtCommand& command);

/// @brief checks the time and every command of a bundle; the first failure carries its index.
[[nodiscard]] std::expected<void, ValidationError> validate (const Bundle& bundle);

}  // namespace bitvm::proto
