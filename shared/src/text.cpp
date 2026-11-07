#include "bitvm/proto/text.hpp"

#include <cmath>
#include <limits>
#include <unordered_map>

#include "bitvm/proto/codec.hpp"
#include "text_util.hpp"

namespace bitvm::proto
{
namespace
{

using text::Token;

// ---- field parsing ----

struct Field {
    std::string key;
    std::string value;
    std::size_t col       = 0;
    std::size_t value_col = 0;
    bool        used      = false;
};

// key=value fields of one command; keeps the first error
class Fields {
public:
    Fields (std::span<const Token> tokens, std::size_t line, std::size_t anchor_col) :
        line_ (line), anchor_col_ (anchor_col) {
        for (const auto& token : tokens) {
            add (token);
        }
    }

    [[nodiscard]] bool
    ok () const {
        return !error_.has_value();
    }

    [[nodiscard]] const TextError&
    error () const {
        return *error_;
    }

    void
    fail (std::size_t col, std::string message) {
        if (!error_) {
            error_ = TextError{ line_, col, std::move (message) };
        }
    }

    // reports unknown fields; call after all getters
    void
    finish () {
        for (const auto& f : fields_) {
            if (!f.used) {
                fail (f.col, "unknown field `" + f.key + "`");
            }
        }
        if (!params_taken_ && !params_.empty()) {
            fail (params_.front().col, "this command takes no parameters");
        }
    }

    template <typename T>
    T
    uint (std::string_view key, std::optional<T> def = std::nullopt) {
        const Field* f = find (key);
        if (f == nullptr) {
            return missing<T> (key, def);
        }
        std::uint64_t v = 0;
        if (!text::parse_uint (f->value, std::numeric_limits<T>::max(), v)) {
            fail (
                f->value_col,
                "invalid `" + std::string (key) + "`: expected an unsigned integer up to " +
                    std::to_string (std::numeric_limits<T>::max())
            );
            return T{};
        }
        return static_cast<T> (v);
    }

    std::int64_t
    i64 (std::string_view key) {
        const Field* f = find (key);
        if (f == nullptr) {
            return missing<std::int64_t> (key, std::nullopt);
        }
        std::int64_t v = 0;
        if (!text::parse_i64 (f->value, v)) {
            fail (f->value_col, "invalid `" + std::string (key) + "`: expected an integer");
            return 0;
        }
        return v;
    }

    float
    f32 (std::string_view key, std::optional<float> def = std::nullopt) {
        const Field* f = find (key);
        if (f == nullptr) {
            return missing<float> (key, def);
        }
        float v = 0.0F;
        if (!text::parse_f32 (f->value, v)) {
            fail (f->value_col, "invalid `" + std::string (key) + "`: expected a finite number");
            return 0.0F;
        }
        return v;
    }

    bool
    boolean (std::string_view key, std::optional<bool> def = std::nullopt) {
        const Field* f = find (key);
        if (f == nullptr) {
            return missing<bool> (key, def);
        }
        if (f->value != "0" && f->value != "1") {
            fail (f->value_col, "invalid `" + std::string (key) + "`: expected 0 or 1");
            return false;
        }
        return f->value == "1";
    }

    std::string
    str (std::string_view key, std::size_t max_len = kMaxString) {
        const Field* f = find (key);
        if (f == nullptr) {
            return missing<std::string> (key, std::nullopt);
        }
        auto s = text::unquote (f->value);
        if (!s) {
            fail (f->value_col + s.error().offset, s.error().message);
            return {};
        }
        if (s->size() > max_len) {
            fail (f->value_col, "string longer than " + std::to_string (max_len) + " bytes");
            return {};
        }
        return std::move (*s);
    }

    // raw text of a field plus its column, for fields with their own syntax
    std::optional<std::string_view>
    raw (std::string_view key, std::size_t& value_col) {
        const Field* f = find (key);
        if (f == nullptr) {
            missing<int> (key, std::nullopt);
            return std::nullopt;
        }
        value_col = f->value_col;
        return std::string_view (f->value);
    }

    template <typename E, typename Lookup>
    E
    enumeration (std::string_view key, Lookup lookup, std::optional<E> def = std::nullopt) {
        const Field* f = find (key);
        if (f == nullptr) {
            return missing<E> (key, def);
        }
        const auto v = lookup (std::string_view (f->value));
        if (!v) {
            fail (
                f->value_col,
                "invalid `" + std::string (key) + "`: unknown name `" + f->value + "`"
            );
            return E{};
        }
        return *v;
    }

    std::vector<std::uint32_t>
    u32_list (std::string_view key) {
        std::vector<std::uint32_t> out;
        const Field*               f = find (key);
        if (f == nullptr) {
            missing<int> (key, std::nullopt);
            return out;
        }
        if (f->value.empty()) {
            return out;
        }
        std::size_t col = f->value_col;
        for (const auto part : text::split (f->value, ',')) {
            std::uint64_t v = 0;
            if (!text::parse_uint (part, std::numeric_limits<std::uint32_t>::max(), v)) {
                fail (col, "invalid `" + std::string (key) + "`: expected unsigned integers");
                return out;
            }
            out.push_back (static_cast<std::uint32_t> (v));
            col += part.size() + 1;
        }
        return out;
    }

    std::vector<float>
    f32_list (std::string_view key) {
        std::vector<float> out;
        const Field*       f = find (key);
        if (f == nullptr) {
            missing<int> (key, std::nullopt);
            return out;
        }
        if (f->value.empty()) {
            return out;
        }
        std::size_t col = f->value_col;
        for (const auto part : text::split (f->value, ',')) {
            float v = 0.0F;
            if (!text::parse_f32 (part, v)) {
                fail (col, "invalid `" + std::string (key) + "`: expected finite numbers");
                return out;
            }
            out.push_back (v);
            col += part.size() + 1;
        }
        return out;
    }

    // `$index=value` and `$name=value`. names are resolved through `names` (may be null)
    std::vector<Pair>
    pairs (const std::vector<std::string>* names) {
        params_taken_ = true;
        std::vector<Pair> out;
        for (const auto& p : params_) {
            Pair                   pair;
            const std::string_view key   = p.key;
            std::uint64_t          index = 0;
            if (!key.empty() && key[0] >= '0' && key[0] <= '9') {
                if (!text::parse_uint (key, 0xFFFF, index)) {
                    fail (p.col, "parameter index out of range");
                    return out;
                }
            } else if (names == nullptr) {
                fail (p.col, "cannot resolve parameter name `" + p.key + "`: the def is unknown");
                return out;
            } else {
                bool found = false;
                for (std::size_t i = 0; i < names->size(); ++i) {
                    if ((*names)[i] == key) {
                        index = i;
                        found = true;
                        break;
                    }
                }
                if (!found || index > 0xFFFF) {
                    fail (p.col, "unknown parameter `" + p.key + "`");
                    return out;
                }
            }
            pair.index = static_cast<std::uint16_t> (index);
            if (!text::parse_f32 (p.value, pair.value)) {
                fail (p.value_col, "invalid parameter value: expected a finite number");
                return out;
            }
            out.push_back (pair);
        }
        return out;
    }

private:
    void
    add (const Token& token) {
        const bool is_param = token.raw.starts_with ('$');
        const auto eq       = token.raw.find ('=');
        if (eq == std::string::npos || eq == (is_param ? 1U : 0U)) {
            fail (token.col, is_param ? "expected `$name=value`" : "expected `key=value`");
            return;
        }
        Field f;
        f.key       = token.raw.substr (is_param ? 1 : 0, eq - (is_param ? 1U : 0U));
        f.value     = token.raw.substr (eq + 1);
        f.col       = token.col;
        f.value_col = token.col + eq + 1;
        if (is_param) {
            params_.push_back (std::move (f));
            return;
        }
        if (find (f.key) != nullptr) {
            fail (token.col, "duplicate field `" + f.key + "`");
            return;
        }
        fields_.push_back (std::move (f));
    }

    Field*
    find (std::string_view key) {
        for (auto& f : fields_) {
            if (f.key == key) {
                f.used = true;
                return &f;
            }
        }
        return nullptr;
    }

    template <typename T>
    T
    missing (std::string_view key, const std::optional<T>& def) {
        if (def) {
            return *def;
        }
        fail (anchor_col_, "missing field `" + std::string (key) + "`");
        return T{};
    }

    std::size_t              line_;
    std::size_t              anchor_col_;
    std::vector<Field>       fields_;
    std::vector<Field>       params_;
    bool                     params_taken_ = false;
    std::optional<TextError> error_;
};

// events=none | events=n_end,late,xrun
std::uint32_t
parse_events (Fields& f, std::string_view key) {
    std::size_t col = 0;
    const auto  raw = f.raw (key, col);
    if (!raw) {
        return 0;
    }
    if (*raw == "none") {
        return 0;
    }
    std::uint32_t bits = 0;
    for (const auto part : text::split (*raw, ',')) {
        if (part == "n_end") {
            bits |= kEventNEnd;
        } else if (part == "late") {
            bits |= kEventLate;
        } else if (part == "xrun") {
            bits |= kEventXrun;
        } else {
            f.fail (col, "unknown event `" + std::string (part) + "`");
            return 0;
        }
        col += part.size() + 1;
    }
    return bits;
}

// opcode written as a name or as 0x....
std::optional<std::uint16_t>
parse_opcode (std::string_view s) {
    if (const auto op = opcode_from_name (s)) {
        return static_cast<std::uint16_t> (*op);
    }
    std::uint64_t v = 0;
    if (s.starts_with ("0x") && text::parse_uint (s, 0xFFFF, v)) {
        return static_cast<std::uint16_t> (v);
    }
    return std::nullopt;
}

// ---- assembler ----

class Assembler {
public:
    explicit Assembler (TextContext& ctx) : ctx_ (ctx) {}

    std::expected<std::vector<Message>, TextError>
    run (std::string_view source) {
        std::vector<Line> lines;
        std::size_t       line_no = 0;
        std::size_t       pos     = 0;
        while (pos <= source.size()) {
            const auto end = source.find ('\n', pos);
            const auto sv  = source.substr (pos, end == std::string_view::npos ? end : end - pos);
            ++line_no;
            pos         = end == std::string_view::npos ? source.size() + 1 : end + 1;
            auto tokens = text::tokenize (sv, line_no);
            if (!tokens) {
                return std::unexpected (tokens.error());
            }
            if (!tokens->empty()) {
                lines.push_back (Line{ line_no, std::move (*tokens) });
            }
        }

        std::vector<Message> out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto head_line = lines[i].no;
            const auto head_col  = lines[i].tokens[0].col;
            auto       message   = assemble_message (lines, i);
            if (!message) {
                return std::unexpected (message.error());
            }
            // everything assembled must encode, so catch size and range problems here
            if (auto encoded = encode (*message); !encoded) {
                return std::unexpected (
                    TextError{ head_line,
                               head_col,
                               "message cannot be encoded: " +
                                   std::string (encoded.error().detail) }
                );
            }
            out.push_back (std::move (*message));
        }
        return out;
    }

private:
    struct Line {
        std::size_t        no = 0;
        std::vector<Token> tokens;
    };

    using Result = std::expected<Message, TextError>;

    static std::unexpected<TextError>
    error_at (std::size_t line, std::size_t col, std::string m) {
        return std::unexpected (TextError{ line, col, std::move (m) });
    }

    static std::unexpected<TextError>
    error_of (const Fields& f) {
        return std::unexpected (f.error());
    }

    // `word[/seq]`; returns the word and the seq if present
    struct Head {
        std::string_view             word;
        std::optional<std::uint32_t> seq;
    };

    static std::expected<Head, TextError>
    split_head (const Token& token, std::size_t line) {
        Head                   head;
        const std::string_view raw   = token.raw;
        const auto             slash = raw.find ('/');
        head.word                    = raw.substr (0, slash);
        if (slash != std::string_view::npos) {
            std::uint64_t v = 0;
            if (!text::parse_uint (raw.substr (slash + 1), 0xFFFFFFFF, v)) {
                return error_at (line, token.col + slash + 1, "invalid sequence number");
            }
            head.seq = static_cast<std::uint32_t> (v);
        }
        return head;
    }

    std::uint32_t
    seq_or_next (const Head& head) const {
        return head.seq ? *head.seq : ctx_.next_seq++;
    }

    std::expected<std::uint32_t, TextError>
    required_seq (const Head& head, const Token& token, std::size_t line) {
        if (!head.seq) {
            return error_at (line, token.col, "`" + std::string (head.word) + "` needs /<seq>");
        }
        return *head.seq;
    }

    Result
    assemble_message (const std::vector<Line>& lines, std::size_t& i) {
        const auto& line  = lines[i];
        const auto& first = line.tokens[0];
        if (first.raw.starts_with ('@')) {
            return assemble_bundle (lines, i);
        }
        auto head = split_head (first, line.no);
        if (!head) {
            return std::unexpected (head.error());
        }
        const auto rest = std::span<const Token> (line.tokens).subspan (1);
        if (head->word == "hello") {
            return assemble_hello (*head, rest, line.no, first.col);
        }
        if (head->word == "welcome") {
            return assemble_welcome (*head, rest, line.no, first);
        }
        if (head->word == "reject") {
            return assemble_reject (*head, rest, line.no, first);
        }
        if (head->word == "call") {
            return assemble_call (*head, rest, line.no, first);
        }
        if (head->word == "reply") {
            return assemble_reply (*head, rest, line.no, first);
        }
        if (head->word == "event") {
            return assemble_event (*head, rest, line.no, first);
        }
        return error_at (line.no, first.col, "unknown message `" + std::string (head->word) + "`");
    }

    // ---- hello, welcome, reject ----

    Result
    assemble_hello (
        const Head&            head,
        std::span<const Token> rest,
        std::size_t            line,
        std::size_t            col
    ) {
        Fields f (rest, line, col);
        Hello  m;
        m.major            = f.uint<std::uint16_t> ("major");
        m.minor            = f.uint<std::uint16_t> ("minor");
        m.catalog_hash     = f.uint<std::uint64_t> ("catalog_hash");
        m.requested_client = f.uint<std::uint8_t> ("requested_client");
        m.events           = parse_events (f, "events");
        m.name             = f.str ("name");
        f.finish();
        if (!f.ok()) {
            return error_of (f);
        }
        return Message{ seq_or_next (head), std::move (m) };
    }

    Result
    assemble_welcome (
        const Head&            head,
        std::span<const Token> rest,
        std::size_t            line,
        const Token&           first
    ) {
        auto seq = required_seq (head, first, line);
        if (!seq) {
            return std::unexpected (seq.error());
        }
        Fields  f (rest, line, first.col);
        Welcome m;
        m.major             = f.uint<std::uint16_t> ("major");
        m.minor             = f.uint<std::uint16_t> ("minor");
        m.catalog_hash      = f.uint<std::uint64_t> ("catalog_hash");
        m.client_id         = f.uint<std::uint8_t> ("client_id");
        m.id_base           = f.uint<std::uint32_t> ("id_base");
        m.id_count          = f.uint<std::uint32_t> ("id_count");
        m.sample_rate       = f.uint<std::uint32_t> ("sample_rate");
        m.block_size        = f.uint<std::uint16_t> ("block_size");
        m.out_channels      = f.uint<std::uint16_t> ("out_channels");
        m.in_channels       = f.uint<std::uint16_t> ("in_channels");
        m.audio_buses       = f.uint<std::uint32_t> ("audio_buses");
        m.control_buses     = f.uint<std::uint32_t> ("control_buses");
        m.max_buffers       = f.uint<std::uint32_t> ("max_buffers");
        m.output_latency_ns = f.i64 ("output_latency_ns");
        m.late_tolerance_ns = f.i64 ("late_tolerance_ns");
        m.max_message       = f.uint<std::uint32_t> ("max_message");
        m.server_now_ns     = f.i64 ("server_now_ns");
        m.server            = f.str ("server");
        f.finish();
        if (!f.ok()) {
            return error_of (f);
        }
        return Message{ *seq, std::move (m) };
    }

    Result
    assemble_reject (
        const Head&            head,
        std::span<const Token> rest,
        std::size_t            line,
        const Token&           first
    ) {
        auto seq = required_seq (head, first, line);
        if (!seq) {
            return std::unexpected (seq.error());
        }
        Fields f (rest, line, first.col);
        Reject m;
        m.code    = f.enumeration<ErrorCode> ("code", error_code_from_name);
        m.message = f.str ("msg");
        f.finish();
        if (!f.ok()) {
            return error_of (f);
        }
        return Message{ *seq, std::move (m) };
    }

    // ---- bundle ----

    std::expected<std::int64_t, TextError>
    parse_time (const Token& token, std::size_t line, std::string_view spec) {
        const std::size_t col = token.col + 1;
        if (spec == "now") {
            return 0;
        }
        if (spec.starts_with ('+')) {
            double secs = 0.0;
            if (!text::parse_f64 (spec.substr (1), secs) || secs < 0.0 || secs > 9.0e9) {
                return error_at (line, col + 1, "invalid relative time");
            }
            const auto ns = static_cast<std::int64_t> (std::llround (secs * 1.0e9));
            if (ctx_.time_base_ns > std::numeric_limits<std::int64_t>::max() - ns) {
                return error_at (line, col, "time overflow");
            }
            return ctx_.time_base_ns + ns;
        }
        std::uint64_t v = 0;
        if (!text::parse_uint (spec, std::numeric_limits<std::int64_t>::max(), v)) {
            return error_at (line, col, "invalid time: expected now, nanoseconds or +seconds");
        }
        return static_cast<std::int64_t> (v);
    }

    Result
    assemble_bundle (const std::vector<Line>& lines, std::size_t& i) {
        const auto& line     = lines[i];
        const auto& first    = line.tokens[0];
        const auto  spec_end = first.raw.find ('/');
        const auto  spec     = std::string_view (first.raw).substr (
            1,
            spec_end == std::string::npos ? std::string::npos : spec_end - 1
        );
        auto time = parse_time (first, line.no, spec);
        if (!time) {
            return std::unexpected (time.error());
        }
        std::optional<std::uint32_t> seq;
        if (spec_end != std::string::npos) {
            std::uint64_t v = 0;
            if (!text::parse_uint (
                    std::string_view (first.raw).substr (spec_end + 1),
                    0xFFFFFFFF,
                    v
                )) {
                return error_at (line.no, first.col + spec_end + 1, "invalid sequence number");
            }
            seq = static_cast<std::uint32_t> (v);
        }

        Bundle bundle;
        bundle.time                       = *time;
        const std::span<const Token> rest = std::span<const Token> (line.tokens).subspan (1);
        if (rest.empty()) {
            return error_at (line.no, first.col + first.raw.size(), "expected a command or `{`");
        }
        if (rest.size() == 1 && rest[0].raw == "{") {
            std::size_t k = i + 1;
            for (; k < lines.size(); ++k) {
                const auto& t = lines[k].tokens;
                if (t.size() == 1 && t[0].raw == "}") {
                    break;
                }
                auto command = rt_command (t, lines[k].no);
                if (!command) {
                    return std::unexpected (command.error());
                }
                bundle.commands.push_back (std::move (*command));
            }
            if (k == lines.size()) {
                return error_at (line.no, rest[0].col, "unterminated bundle block");
            }
            if (bundle.commands.empty()) {
                return error_at (line.no, rest[0].col, "empty bundle block");
            }
            i = k;
        } else {
            auto command = rt_command (rest, line.no);
            if (!command) {
                return std::unexpected (command.error());
            }
            bundle.commands.push_back (std::move (*command));
        }
        return Message{ seq ? *seq : ctx_.next_seq++, std::move (bundle) };
    }

    const std::vector<std::string>*
    names_of (std::optional<std::uint32_t> def) const {
        if (!def || !ctx_.param_names) {
            return nullptr;
        }
        return ctx_.param_names (*def);
    }

    std::optional<std::uint32_t>
    def_of_node (NodeId id) const {
        if (const auto it = node_defs_.find (id); it != node_defs_.end()) {
            return it->second;
        }
        return ctx_.node_def ? ctx_.node_def (id) : std::nullopt;
    }

    std::expected<RtCommand, TextError>
    rt_command (std::span<const Token> tokens, std::size_t line) {
        const auto& name = tokens[0];
        const auto  op   = opcode_from_name (name.raw);
        if (!op || classify (static_cast<std::uint16_t> (*op)) != OpcodeClass::rt) {
            const bool is_call =
                op && classify (static_cast<std::uint16_t> (*op)) == OpcodeClass::call;
            return error_at (
                line,
                name.col,
                (is_call ? "call used inside a bundle: `" : "unknown bundle command `") + name.raw +
                    "`"
            );
        }
        Fields    f (tokens.subspan (1), line, name.col);
        RtCommand result;
        switch (*op) {
            case Opcode::node_new: {
                NodeNew c;
                c.id     = f.uint<std::uint32_t> ("id");
                c.def    = f.uint<std::uint32_t> ("def");
                c.add    = f.enumeration<AddAction> ("add", add_action_from_name);
                c.target = f.uint<std::uint32_t> ("target");
                c.paused = f.boolean ("paused", false);
                c.params = f.pairs (names_of (c.def));
                if (f.ok()) {
                    node_defs_[c.id] = c.def;
                }
                result = std::move (c);
                break;
            }
            case Opcode::group_new: {
                GroupNew c;
                c.id     = f.uint<std::uint32_t> ("id");
                c.add    = f.enumeration<AddAction> ("add", add_action_from_name);
                c.target = f.uint<std::uint32_t> ("target");
                result   = c;
                break;
            }
            case Opcode::node_free: {
                NodeFree c;
                c.ids  = f.u32_list ("ids");
                result = std::move (c);
                break;
            }
            case Opcode::node_set: {
                NodeSet c;
                c.id     = f.uint<std::uint32_t> ("id");
                c.ramp   = f.f32 ("ramp", 0.0F);
                c.curve  = f.enumeration<Curve> ("curve", curve_from_name, Curve::lin);
                c.params = f.pairs (names_of (def_of_node (c.id)));
                result   = std::move (c);
                break;
            }
            case Opcode::node_run: {
                NodeRun c;
                c.id   = f.uint<std::uint32_t> ("id");
                c.run  = f.boolean ("run");
                result = c;
                break;
            }
            case Opcode::node_move: {
                NodeMove c;
                c.id     = f.uint<std::uint32_t> ("id");
                c.add    = f.enumeration<AddAction> ("add", add_action_from_name);
                c.target = f.uint<std::uint32_t> ("target");
                result   = c;
                break;
            }
            case Opcode::group_clear: {
                GroupClear c;
                c.id   = f.uint<std::uint32_t> ("id");
                result = c;
                break;
            }
            default: {
                CbusSet c;
                c.index  = f.uint<std::uint32_t> ("index");
                c.values = f.f32_list ("values");
                result   = std::move (c);
                break;
            }
        }
        f.finish();
        if (!f.ok()) {
            return error_of (f);
        }
        return result;
    }

    // ---- call ----

    Result
    assemble_call (
        const Head&            head,
        std::span<const Token> rest,
        std::size_t            line,
        const Token&           first
    ) {
        if (rest.empty()) {
            return error_at (line, first.col + first.raw.size(), "expected a call name");
        }
        const auto& name = rest[0];
        const auto  op   = opcode_from_name (name.raw);
        if (!op || classify (static_cast<std::uint16_t> (*op)) != OpcodeClass::call) {
            return error_at (line, name.col, "unknown call `" + name.raw + "`");
        }
        Fields      f (rest.subspan (1), line, name.col);
        CallCommand command;
        switch (*op) {
            case Opcode::sync: {
                command = Sync{};
                break;
            }
            case Opcode::status: {
                command = Status{};
                break;
            }
            case Opcode::tree: {
                Tree c;
                c.root  = f.uint<std::uint32_t> ("root");
                c.depth = f.uint<std::uint16_t> ("depth", std::uint16_t{ 0 });
                command = c;
                break;
            }
            case Opcode::client_reset: {
                command = ClientReset{};
                break;
            }
            case Opcode::def_load: {
                command = DefLoad{ load_graph_field (f) };
                break;
            }
            case Opcode::def_free: {
                DefFree c;
                c.def   = f.uint<std::uint32_t> ("def");
                command = c;
                break;
            }
            case Opcode::buf_alloc: {
                BufAlloc c;
                c.frames   = f.uint<std::uint32_t> ("frames");
                c.channels = f.uint<std::uint16_t> ("channels");
                command    = c;
                break;
            }
            case Opcode::buf_load: {
                BufLoad c;
                c.path   = f.str ("path", kMaxPath);
                c.start  = f.uint<std::uint32_t> ("start", std::uint32_t{ 0 });
                c.frames = f.uint<std::uint32_t> ("frames", std::uint32_t{ 0 });
                command  = std::move (c);
                break;
            }
            case Opcode::buf_free: {
                BufFree c;
                c.buf   = f.uint<std::uint32_t> ("buf");
                command = c;
                break;
            }
            case Opcode::bus_alloc: {
                BusAlloc c;
                c.kind  = f.enumeration<BusKind> ("kind", bus_kind_from_name);
                c.count = f.uint<std::uint16_t> ("count");
                command = c;
                break;
            }
            default: {
                BusFree c;
                c.kind  = f.enumeration<BusKind> ("kind", bus_kind_from_name);
                c.base  = f.uint<std::uint32_t> ("base");
                command = c;
                break;
            }
        }
        f.finish();
        if (!f.ok()) {
            return error_of (f);
        }
        return Message{ seq_or_next (head), Call{ std::move (command) } };
    }

    // graph=base64:... or graph=@path
    std::vector<std::byte>
    load_graph_field (Fields& f) {
        std::size_t col = 0;
        const auto  raw = f.raw ("graph", col);
        if (!raw) {
            return {};
        }
        if (raw->starts_with ("base64:")) {
            auto bytes = base64_decode (raw->substr (7));
            if (!bytes) {
                f.fail (col + 7, "invalid base64");
                return {};
            }
            return std::move (*bytes);
        }
        if (!raw->starts_with ('@')) {
            f.fail (col, "expected base64:<data> or @<path>");
            return {};
        }
        if (!ctx_.read_file) {
            f.fail (col, "no file loader: cannot read graph files");
            return {};
        }
        const std::string path (raw->substr (1));
        auto              contents = ctx_.read_file (path);
        if (!contents) {
            f.fail (col, "cannot read `" + path + "`: " + contents.error());
            return {};
        }
        auto graph = assemble_graph (*contents);
        if (!graph) {
            f.fail (col, path + ":" + to_string (graph.error()));
            return {};
        }
        auto bytes = encode_graph (*graph);
        if (!bytes) {
            f.fail (col, path + ": " + bytes.error().message);
            return {};
        }
        return std::move (*bytes);
    }

    // ---- reply ----

    Result
    assemble_reply (
        const Head&            head,
        std::span<const Token> rest,
        std::size_t            line,
        const Token&           first
    ) {
        auto seq = required_seq (head, first, line);
        if (!seq) {
            return std::unexpected (seq.error());
        }
        if (rest.size() < 2) {
            const auto& last = rest.empty() ? first : rest.back();
            return error_at (
                line,
                last.col + last.raw.size(),
                "expected `<call> ok ...` or `<call> err=...`"
            );
        }
        const auto opcode = parse_opcode (rest[0].raw);
        if (!opcode) {
            return error_at (line, rest[0].col, "unknown call `" + rest[0].raw + "`");
        }
        Reply reply;
        reply.opcode = *opcode;
        if (rest[1].raw == "ok") {
            if (classify (*opcode) != OpcodeClass::call) {
                return error_at (line, rest[0].col, "`" + rest[0].raw + "` is not a call");
            }
            Fields f (rest.subspan (2), line, rest[1].col);
            reply.outcome = read_result (static_cast<Opcode> (*opcode), f);
            f.finish();
            if (!f.ok()) {
                return error_of (f);
            }
        } else {
            if (!rest[1].raw.starts_with ("err=")) {
                return error_at (line, rest[1].col, "expected `ok` or `err=<code>`");
            }
            Fields    f (rest.subspan (1), line, rest[1].col);
            CallError e;
            e.code    = f.enumeration<ErrorCode> ("err", error_code_from_name);
            e.message = f.str ("msg");
            f.finish();
            if (f.ok() && e.code == ErrorCode::ok) {
                f.fail (rest[1].col, "`err` cannot be ok");
            }
            if (!f.ok()) {
                return error_of (f);
            }
            reply.outcome = std::move (e);
        }
        return Message{ *seq, std::move (reply) };
    }

    static CallResult
    read_result (Opcode op, Fields& f) {
        switch (op) {
            case Opcode::status: {
                StatusInfo s;
                s.now_ns            = f.i64 ("now_ns");
                s.sample_rate       = f.uint<std::uint32_t> ("sample_rate");
                s.block_size        = f.uint<std::uint16_t> ("block_size");
                s.cpu_avg           = f.f32 ("cpu_avg");
                s.cpu_peak          = f.f32 ("cpu_peak");
                s.nodes             = f.uint<std::uint32_t> ("nodes");
                s.groups            = f.uint<std::uint32_t> ("groups");
                s.defs              = f.uint<std::uint32_t> ("defs");
                s.buffers           = f.uint<std::uint32_t> ("buffers");
                s.rt_queue_used     = f.uint<std::uint32_t> ("rt_queue_used");
                s.rt_queue_capacity = f.uint<std::uint32_t> ("rt_queue_capacity");
                s.late_played       = f.uint<std::uint64_t> ("late_played");
                s.late_dropped      = f.uint<std::uint64_t> ("late_dropped");
                s.skipped           = f.uint<std::uint64_t> ("skipped");
                s.xruns             = f.uint<std::uint64_t> ("xruns");
                s.events_dropped    = f.uint<std::uint64_t> ("events_dropped");
                return s;
            }
            case Opcode::tree: {
                return read_tree (f);
            }
            case Opcode::def_load: {
                return DefLoaded{ f.uint<std::uint32_t> ("def") };
            }
            case Opcode::buf_alloc: {
                return BufAllocated{ f.uint<std::uint32_t> ("buf") };
            }
            case Opcode::buf_load: {
                BufLoaded b;
                b.buf         = f.uint<std::uint32_t> ("buf");
                b.frames      = f.uint<std::uint32_t> ("frames");
                b.channels    = f.uint<std::uint16_t> ("channels");
                b.sample_rate = f.uint<std::uint32_t> ("sample_rate");
                return b;
            }
            case Opcode::bus_alloc: {
                return BusAllocated{ f.uint<std::uint32_t> ("base") };
            }
            default: {
                return Empty{};
            }
        }
    }

    // entries=id:parent:kind:def:running,...
    static TreeInfo
    read_tree (Fields& f) {
        TreeInfo t;
        t.truncated     = f.boolean ("truncated");
        std::size_t col = 0;
        const auto  raw = f.raw ("entries", col);
        if (!raw || raw->empty()) {
            return t;
        }
        for (const auto entry : text::split (*raw, ',')) {
            const auto    parts  = text::split (entry, ':');
            std::uint64_t id     = 0;
            std::uint64_t parent = 0;
            std::uint64_t def    = 0;
            const auto    kind = parts.size() == 5 ? node_kind_from_name (parts[2]) : std::nullopt;
            if (parts.size() != 5 || !kind || !text::parse_uint (parts[0], 0xFFFFFFFF, id) ||
                !text::parse_uint (parts[1], 0xFFFFFFFF, parent) ||
                !text::parse_uint (parts[3], 0xFFFFFFFF, def) ||
                (parts[4] != "0" && parts[4] != "1")) {
                f.fail (col, "invalid tree entry: expected id:parent:kind:def:running");
                return t;
            }
            TreeEntry e;
            e.id      = static_cast<std::uint32_t> (id);
            e.parent  = static_cast<std::uint32_t> (parent);
            e.kind    = *kind;
            e.def     = static_cast<std::uint32_t> (def);
            e.running = parts[4] == "1";
            t.entries.push_back (e);
            col += entry.size() + 1;
        }
        return t;
    }

    // ---- event ----

    Result
    assemble_event (
        const Head&            head,
        std::span<const Token> rest,
        std::size_t            line,
        const Token&           first
    ) {
        auto seq = required_seq (head, first, line);
        if (!seq) {
            return std::unexpected (seq.error());
        }
        if (rest.empty()) {
            return error_at (line, first.col + first.raw.size(), "expected an event name");
        }
        const auto& name = rest[0];
        const auto  op   = opcode_from_name (name.raw);
        if (!op || classify (static_cast<std::uint16_t> (*op)) != OpcodeClass::event) {
            return error_at (line, name.col, "unknown event `" + name.raw + "`");
        }
        Fields    f (rest.subspan (1), line, name.col);
        EventData data;
        switch (*op) {
            case Opcode::n_end: {
                data = NEnd{ f.uint<std::uint32_t> ("id") };
                break;
            }
            case Opcode::late: {
                Late e;
                e.played      = f.uint<std::uint32_t> ("played");
                e.dropped     = f.uint<std::uint32_t> ("dropped");
                e.max_late_ns = f.i64 ("max_late_ns");
                data          = e;
                break;
            }
            case Opcode::xrun: {
                data = Xrun{ f.uint<std::uint32_t> ("count") };
                break;
            }
            default: {
                ErrorEvent e;
                e.bundle_seq    = f.uint<std::uint32_t> ("bundle_seq");
                e.index         = f.uint<std::uint16_t> ("index");
                std::size_t col = 0;
                if (const auto raw = f.raw ("opcode", col)) {
                    if (const auto code = parse_opcode (*raw)) {
                        e.opcode = *code;
                    } else {
                        f.fail (col, "invalid `opcode`: expected a name or 0x....");
                    }
                }
                e.code    = f.enumeration<ErrorCode> ("code", error_code_from_name);
                e.level   = f.uint<std::uint8_t> ("level");
                e.message = f.str ("msg");
                data      = std::move (e);
                break;
            }
        }
        f.finish();
        if (!f.ok()) {
            return error_of (f);
        }
        return Message{ *seq, Event{ std::move (data) } };
    }

    TextContext&                              ctx_;
    std::unordered_map<NodeId, std::uint32_t> node_defs_;
};

}  // namespace

std::string
to_string (const TextError& error) {
    return std::to_string (error.line) + ":" + std::to_string (error.column) + ": " + error.message;
}

std::expected<std::vector<Message>, TextError>
assemble (std::string_view text, TextContext& ctx) {
    Assembler assembler (ctx);
    return assembler.run (text);
}

}  // namespace bitvm::proto
