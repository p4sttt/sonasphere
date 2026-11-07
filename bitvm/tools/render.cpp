#include <charconv>
#include <cmath>
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

#include "bitvm/backend.hpp"
#include "bitvm/score.hpp"

namespace
{

void
usage () {
    std::cerr << "usage: bitvm-render [--duration seconds] [--sample-rate rate] "
                 "[--channels n] input.bvs[b] output.wav\n";
}

template <typename T>
bool
parse_number (std::string_view text, T& value) {
    const auto result = std::from_chars (text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

}  // namespace

int
main (int argc, char** argv) {
    bitvm::EngineConfig           config;
    bitvm::RenderOptions          options;
    std::vector<std::string_view> paths;
    bool                          positional_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument (argv[i]);
        if (!positional_only && (argument == "--help" || argument == "-h")) {
            usage();
            return 0;
        }
        if (!positional_only && argument == "--") {
            positional_only = true;
            continue;
        }
        if (!positional_only && argument.starts_with ('-')) {
            if ((argument != "--duration" && argument != "--sample-rate" &&
                 argument != "--channels") ||
                i + 1 >= argc) {
                usage();
                return 2;
            }
            const std::string_view value (argv[++i]);
            if (argument == "--duration") {
                double seconds = 0;
                if (!parse_number (value, seconds) || !std::isfinite (seconds) || seconds < 0 ||
                    seconds > bitvm::kMaxRenderSeconds) {
                    std::cerr << "duration must be finite and between 0 and 86400 seconds\n";
                    return 2;
                }
                options.duration_seconds = seconds;
            } else if (argument == "--sample-rate") {
                if (!parse_number (value, config.sample_rate) || config.sample_rate == 0 ||
                    config.sample_rate > 768000) {
                    std::cerr << "sample rate must be between 1 and 768000\n";
                    return 2;
                }
            } else {
                if (!parse_number (value, config.out_channels) || config.out_channels == 0 ||
                    config.out_channels > 64) {
                    std::cerr << "channels must be between 1 and 64\n";
                    return 2;
                }
            }
        } else {
            paths.push_back (argument);
        }
    }
    if (paths.size() != 2) {
        usage();
        return 2;
    }
    const auto score = bitvm::read_score (paths[0]);
    if (!score) {
        std::cerr << "cannot read score: " << score.error().message << '\n';
        return 1;
    }
    try {
        bitvm::Engine         engine (config);
        bitvm::OfflineBackend backend (config, paths[1]);
        const auto            result = bitvm::render_score (engine, *score, backend, options);
        if (!result) {
            if (result.error().message_index != 0) {
                std::cerr << "score message " << result.error().message_index << ": ";
            }
            std::cerr << result.error().message << '\n';
            return 1;
        }
    } catch (const std::exception& exception) {
        std::cerr << "cannot render score: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
