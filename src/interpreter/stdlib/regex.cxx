#include "../evaluator.hxx"
#include "regex_support.hxx"
#include <mutex>
#include <unordered_map>

#ifdef KEX_HAVE_PCRE2
#include <pcre2.h>
#endif

namespace kex::interpreter {

namespace {

#ifdef KEX_HAVE_PCRE2

// Compiled patterns are cached by source text and never freed: a program's
// distinct pattern set is bounded by its source, and `Regex` values carry only
// their source string: a compiled pattern bakes in the host's PCRE version and
// must not be embeddable in a .kexo artifact, so the source is the identity. The mutex
// guards against concurrent access from scheduler fibers on separate threads.
std::unordered_map<std::string, pcre2_code*> g_patternCache;
std::mutex g_patternCacheMutex;

// PCRE2_UTF|PCRE2_UCP are pinned on, matching the `[unicode, ucp]` the BEAM
// backend must pass to re:compile/2. Without UCP, `\d` and `\w` silently fall
// back to ASCII and the two backends disagree on the same pattern.
constexpr uint32_t kCompileOptions = PCRE2_UTF | PCRE2_UCP;

struct CompileFailure {
    size_t offset;        // byte offset into the pattern
    std::string message;
};

// Compiles (or returns a cached) pattern. On failure returns nullptr and fills
// `failure`.
auto compilePattern(const std::string& source, CompileFailure& failure)
    -> pcre2_code* {
    {
        std::lock_guard<std::mutex> lock(g_patternCacheMutex);
        if (auto it = g_patternCache.find(source); it != g_patternCache.end())
            return it->second;
    }

    int errorCode = 0;
    PCRE2_SIZE errorOffset = 0;
    pcre2_code* code = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(source.c_str()), source.size(),
        kCompileOptions, &errorCode, &errorOffset, nullptr);

    if (!code) {
        PCRE2_UCHAR buffer[256];
        pcre2_get_error_message(errorCode, buffer, sizeof(buffer));
        failure.offset = errorOffset;
        failure.message = reinterpret_cast<const char*>(buffer);
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_patternCacheMutex);
    // Another fiber may have compiled the same source while this one worked;
    // keep the winner and discard the duplicate so the cache stays canonical.
    auto [it, inserted] = g_patternCache.emplace(source, code);
    if (!inserted) pcre2_code_free(code);
    return it->second;
}

// Byte offset -> character offset. Kex strings are logical text, but both
// engines report byte offsets into UTF-8 (Erlang `re` over binaries, PCRE2
// over the UTF-8 subject), so every offset crossing into Kex is converted —
// otherwise a pattern over non-ASCII reports different positions per backend.
auto charOffset(const std::string& subject, size_t byteOffset) -> int64_t {
    int64_t chars = 0;
    for (size_t i = 0; i < byteOffset && i < subject.size(); ++i)
        if ((static_cast<unsigned char>(subject[i]) & 0xC0) != 0x80) ++chars;
    return chars;
}

// Pulls the pattern source out of a `Regex` record (its only field).
auto regexSource(const ValuePtr& value) -> const std::string* {
    auto* rec = std::get_if<RecordValue>(&value->data);
    if (!rec || rec->typeName != "Regex") return nullptr;
    auto it = rec->fields.find("source");
    if (it == rec->fields.end()) return nullptr;
    auto* s = std::get_if<StringValue>(&it->second->data);
    return s ? &s->value : nullptr;
}

// Maps each named group to its group number. PCRE2 hands back a packed name
// table (2 bytes of big-endian group number, then a NUL-terminated name) —
// note this is by *number*, so a named group stays reachable under both its
// name-atom and its index, as the Match design requires.
auto namedGroups(pcre2_code* code) -> std::vector<std::pair<std::string, uint32_t>> {
    uint32_t nameCount = 0, nameEntrySize = 0;
    PCRE2_SPTR nameTable = nullptr;
    pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &nameCount);
    if (nameCount == 0) return {};
    pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &nameEntrySize);
    pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &nameTable);

    std::vector<std::pair<std::string, uint32_t>> result;
    for (uint32_t i = 0; i < nameCount; ++i) {
        PCRE2_SPTR entry = nameTable + i * nameEntrySize;
        const uint32_t groupNumber = (entry[0] << 8) | entry[1];
        result.emplace_back(reinterpret_cast<const char*>(entry + 2), groupNumber);
    }
    return result;
}

// Builds a `Match` from an ovector. Keys are group numbers (Int) plus name
// atoms for named groups — the same value reachable under both.
//
// A group that did not participate is left OUT of the map entirely, which is
// what makes `m.get(:opt)` return None while a group that matched empty
// returns Just(""). PCRE2 marks non-participation with PCRE2_UNSET, distinct
// from a zero-length match at a real offset; the BEAM backend must use
// re:run's `index` capture mode to preserve the same distinction, since its
// `binary` mode renders both as <<>>.
auto buildMatch(const std::string& subject, pcre2_code* code,
                const PCRE2_SIZE* ovector, uint32_t pairCount) -> ValuePtr {
    std::vector<std::pair<ValuePtr, ValuePtr>> entries;

    auto groupText = [&](uint32_t i) -> ValuePtr {
        return Value::string(
            subject.substr(ovector[2 * i], ovector[2 * i + 1] - ovector[2 * i]));
    };

    for (uint32_t i = 0; i < pairCount; ++i) {
        if (ovector[2 * i] == PCRE2_UNSET) continue;
        entries.emplace_back(Value::integer(static_cast<int64_t>(i)), groupText(i));
    }
    for (const auto& [name, number] : namedGroups(code)) {
        if (number >= pairCount || ovector[2 * number] == PCRE2_UNSET) continue;
        entries.emplace_back(Value::atom(name), groupText(number));
    }

    auto captures = std::make_shared<Value>();
    captures->data = MapValue{std::move(entries)};
    return Value::record("Match", {{"captures", captures}});
}

// Escapes every regex metacharacter individually. Deliberately NOT \Q...\E
// wrapping: a value containing \E closes the quoted span early and the rest
// becomes live pattern syntax — an injection hole and a spurious compile error
// at once. Per-character escaping has no terminator to escape out of.
auto quoteMeta(const std::string& text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        // UTF-8 lead/continuation bytes (>= 0x80) are never metacharacters and
        // must pass through untouched, or the escaped text stops being valid
        // UTF-8.
        if (c < 0x80 && !std::isalnum(c) && c != '_') out.push_back('\\');
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// Advances past one UTF-8 character. Iteration must step by a character, not
// a byte, or an empty match inside a multi-byte sequence would resume mid-
// character and PCRE2 would reject the subject as malformed UTF-8.
auto nextCharBoundary(const std::string& subject, size_t offset) -> size_t {
    if (offset >= subject.size()) return offset + 1;
    size_t next = offset + 1;
    while (next < subject.size() &&
           (static_cast<unsigned char>(subject[next]) & 0xC0) == 0x80)
        ++next;
    return next;
}

// One step of a global iteration. Returns false when no further match exists.
// `nextStart` implements the report-then-advance rule every engine uses: an
// empty match advances one character so iteration terminates. Empty matches
// are reported, not skipped, matching Ruby/Python/JS and Erlang's re.
auto matchAt(pcre2_code* code, const std::string& subject, size_t start,
             pcre2_match_data* matchData, size_t& matchStart, size_t& matchEnd,
             size_t& nextStart, int& groupCount) -> bool {
    if (start > subject.size()) return false;
    const int rc = pcre2_match(
        code, reinterpret_cast<PCRE2_SPTR>(subject.c_str()), subject.size(),
        start, 0, matchData, nullptr);
    if (rc <= 0) return false;

    const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(matchData);
    matchStart = ovector[0];
    matchEnd = ovector[1];
    groupCount = rc;
    nextStart = (matchEnd == matchStart) ? nextCharBoundary(subject, matchEnd)
                                         : matchEnd;
    return true;
}

#endif // KEX_HAVE_PCRE2

} // namespace

// --- Shared with String's own operations (see regex_support.hxx) -----------

namespace regexsupport {

auto isRegex(const ValuePtr& value) -> bool {
    auto* rec = std::get_if<RecordValue>(&value->data);
    return rec && rec->typeName == "Regex";
}

auto split(const std::string& subject, const ValuePtr& regex, int64_t limit)
    -> std::optional<std::vector<ValuePtr>> {
#ifndef KEX_HAVE_PCRE2
    (void)subject; (void)regex; (void)limit;
    return std::nullopt;
#else
    const auto* source = regexSource(regex);
    if (!source) return std::nullopt;

    // Ruby: "".split(",") is [], not [""].
    if (subject.empty()) return std::vector<ValuePtr>{};

    CompileFailure failure;
    pcre2_code* code = compilePattern(*source, failure);
    if (!code) return std::vector<ValuePtr>{Value::string(subject)};

    pcre2_match_data* matchData =
        pcre2_match_data_create_from_pattern(code, nullptr);
    std::vector<ValuePtr> fields;
    size_t start = 0, fieldStart = 0;
    size_t matchStart = 0, matchEnd = 0, nextStart = 0;
    int groupCount = 0;
    while (matchAt(code, subject, start, matchData, matchStart, matchEnd,
                   nextStart, groupCount)) {
        if (limit > 0 && static_cast<int64_t>(fields.size()) >= limit - 1) break;
        // A zero-width match sitting exactly at the field start would emit an
        // empty field forever; skipping it is what makes splitting on an empty
        // pattern yield characters ("abc" -> ["a","b","c"]).
        if (matchEnd == matchStart && matchStart == fieldStart) {
            start = nextStart;
            continue;
        }
        fields.push_back(
            Value::string(subject.substr(fieldStart, matchStart - fieldStart)));
        // Capture groups land between the fields they separated.
        const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(matchData);
        for (int g = 1; g < groupCount; ++g) {
            if (ovector[2 * g] == PCRE2_UNSET) continue;
            fields.push_back(Value::string(subject.substr(
                ovector[2 * g], ovector[2 * g + 1] - ovector[2 * g])));
        }
        fieldStart = matchEnd;
        start = nextStart;
    }
    pcre2_match_data_free(matchData);
    fields.push_back(Value::string(subject.substr(fieldStart)));

    // Any non-zero limit suppresses the trailing-empty trim.
    if (limit == 0)
        while (!fields.empty()) {
            auto* last = std::get_if<StringValue>(&fields.back()->data);
            if (!last || !last->value.empty()) break;
            fields.pop_back();
        }
    return fields;
#endif
}

} // namespace regexsupport

auto Evaluator::registerRegexBuiltins() -> void {
    defineModule("Regex");

#ifndef KEX_HAVE_PCRE2
    // Wasm has no PCRE2 yet (see CMakeLists.txt). Fail loudly at the call
    // rather than silently returning a non-matching Regex.
    auto unavailable = [](std::vector<ValuePtr>) -> ValuePtr {
        throw std::runtime_error(
            "Regex is unavailable in this build (PCRE2 not compiled in)");
    };
    defineIntrinsic("Regex::compile", unavailable);
    defineIntrinsic("Regex::quote", unavailable);
    defineIntrinsic("Regex::matches", unavailable);
    defineIntrinsic("Regex::matches?", unavailable);
    defineIntrinsic("Regex::scan", unavailable);
    defineIntrinsic("Regex::replace", unavailable);
    defineIntrinsic("Regex::split", unavailable);
#else

    // Regex::compile(source) -> Result<Regex, RegexError>
    //
    // The `Regex` record carries only its source; the compiled pcre2_code
    // lives in the cache above, keyed by that source. RegexError mirrors
    // ParseError's shape (source/position/message) and implements Errorable.
    defineIntrinsic("Regex::compile", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto* s = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!s)
            return Value::error(Value::record("RegexError", {
                {"source",   Value::string("")},
                {"position", Value::integer(0)},
                {"message",  Value::string("regex expects a String")},
            }));

        CompileFailure failure;
        if (!compilePattern(s->value, failure))
            return Value::error(Value::record("RegexError", {
                {"source",   Value::string(s->value)},
                {"position", Value::integer(charOffset(s->value, failure.offset))},
                {"message",  Value::string(failure.message)},
            }));

        return Value::ok(Value::record("Regex", {
            {"source", Value::string(s->value)},
        }));
    });

    // Regex::quote(s) -> String — escapes every metacharacter individually.
    //
    // Deliberately NOT \Q...\E wrapping: a value containing \E closes the
    // quoted span early and the remainder becomes live pattern syntax, which
    // is both an injection hole and a spurious compile error. Per-character
    // escaping has no terminator to escape out of. (Perl's qr/\Q$x\E/ is safe
    // only because its interpolation does exactly this underneath.)
    defineIntrinsic("Regex::quote", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto* s = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!s) return Value::string("");

        return Value::string(quoteMeta(s->value));
    });

    // Regex::tag(parts, values) -> Regex — the tagged-literal ABI.
    //
    // `` regex`\d+` `` lowers to regex(parts, values) and returns a BARE Regex,
    // not a Result: a literal is validated at compile time by validateRegex, so
    // it cannot fail. Interpolated values are escaped per character, so they
    // contribute text and never pattern syntax — which is what keeps the bare
    // return type honest.
    defineIntrinsic("Regex::tag", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::record("Regex", {{"source", Value::string("")}});
        auto* parts = std::get_if<ListValue>(&args[0]->data);
        const ListValue* values =
            args.size() >= 2 ? std::get_if<ListValue>(&args[1]->data) : nullptr;
        if (!parts) return Value::record("Regex", {{"source", Value::string("")}});

        std::string source;
        for (size_t i = 0; i < parts->elements.size(); ++i) {
            if (auto* p = std::get_if<StringValue>(&parts->elements[i]->data))
                source += p->value;
            if (values && i < values->elements.size())
                source += quoteMeta(values->elements[i]->toString());
        }

        CompileFailure failure;
        if (!compilePattern(source, failure))
            throw std::runtime_error("invalid regex `" + source +
                                     "`: " + failure.message);
        return Value::record("Regex", {{"source", Value::string(source)}});
    });

    // Regex::validate(source) -> (Integer, String)? — the compile-time check
    // behind validateRegex. None when the pattern compiles.
    //
    // The offset here is a BYTE offset, unlike RegexError.position: the
    // compiler maps it back onto the literal's source span, and
    // TaggedValidation.ByteSpan is defined in bytes.
    defineIntrinsic("Regex::validate", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto* s = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!s) return Value::none();
        CompileFailure failure;
        if (compilePattern(s->value, failure)) return Value::none();
        return Value::just(Value::tuple(
            {Value::integer(static_cast<int64_t>(failure.offset)),
             Value::string(failure.message)}));
    });

    // Regex::matches(subject, re) -> Match? — unanchored: finds the first
    // occurrence anywhere in the subject. Anchor with ^...$ in the pattern.
    defineIntrinsic("Regex::matches", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto* subject = std::get_if<StringValue>(&args[0]->data);
        const auto* source = regexSource(args[1]);
        if (!subject || !source) return Value::none();

        CompileFailure failure;
        pcre2_code* code = compilePattern(*source, failure);
        if (!code) return Value::none();

        pcre2_match_data* matchData =
            pcre2_match_data_create_from_pattern(code, nullptr);
        const int rc = pcre2_match(
            code, reinterpret_cast<PCRE2_SPTR>(subject->value.c_str()),
            subject->value.size(), 0, 0, matchData, nullptr);

        ValuePtr result = Value::none();
        if (rc > 0)
            result = Value::just(buildMatch(subject->value, code,
                                            pcre2_get_ovector_pointer(matchData),
                                            static_cast<uint32_t>(rc)));
        pcre2_match_data_free(matchData);
        return result;
    });

    // Regex::matches?(subject, re) -> Bool — the boolean shadow of matches,
    // and unanchored for the same reason: "does the pattern occur anywhere".
    defineIntrinsic("Regex::matches?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* subject = std::get_if<StringValue>(&args[0]->data);
        const auto* source = regexSource(args[1]);
        if (!subject || !source) return Value::boolean(false);

        CompileFailure failure;
        pcre2_code* code = compilePattern(*source, failure);
        if (!code) return Value::boolean(false);

        pcre2_match_data* matchData =
            pcre2_match_data_create_from_pattern(code, nullptr);
        const int rc = pcre2_match(
            code, reinterpret_cast<PCRE2_SPTR>(subject->value.c_str()),
            subject->value.size(), 0, 0, matchData, nullptr);
        pcre2_match_data_free(matchData);
        return Value::boolean(rc > 0);
    });

    // Regex::scan(subject, re) -> [Match] — every match, left to right.
    //
    // Always [Match], never bare strings: unlike Ruby, adding a capture group
    // to a pattern must not change the type flowing out of scan.
    defineIntrinsic("Regex::scan", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto* subject = std::get_if<StringValue>(&args[0]->data);
        const auto* source = regexSource(args[1]);
        if (!subject || !source) return Value::list({});

        CompileFailure failure;
        pcre2_code* code = compilePattern(*source, failure);
        if (!code) return Value::list({});

        pcre2_match_data* matchData =
            pcre2_match_data_create_from_pattern(code, nullptr);
        std::vector<ValuePtr> matches;
        size_t start = 0, matchStart = 0, matchEnd = 0, nextStart = 0;
        int groupCount = 0;
        while (matchAt(code, subject->value, start, matchData, matchStart,
                       matchEnd, nextStart, groupCount)) {
            matches.push_back(buildMatch(subject->value, code,
                                         pcre2_get_ovector_pointer(matchData),
                                         static_cast<uint32_t>(groupCount)));
            start = nextStart;
        }
        pcre2_match_data_free(matchData);
        return Value::list(std::move(matches));
    });

    // Regex::replace(subject, re, replacement) -> String
    //
    // Global (Ruby's gsub, not sub) and literal: the replacement string is
    // inserted verbatim, with no $1/\1 backreference mini-language. To use
    // captures, pass a block instead — it receives the Match.
    defineIntrinsic("Regex::replace", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 3) return args.empty() ? Value::string("") : args[0];
        auto* subject = std::get_if<StringValue>(&args[0]->data);
        const auto* source = regexSource(args[1]);
        if (!subject || !source) return args[0];

        CompileFailure failure;
        pcre2_code* code = compilePattern(*source, failure);
        if (!code) return args[0];

        // Either a literal replacement string or a block taking the Match.
        auto* literal = std::get_if<StringValue>(&args[2]->data);
        auto* block = std::get_if<FunctionValue>(&args[2]->data);
        if (!literal && (!block || !block->native)) return args[0];

        pcre2_match_data* matchData =
            pcre2_match_data_create_from_pattern(code, nullptr);
        std::string out;
        size_t start = 0, copied = 0, matchStart = 0, matchEnd = 0, nextStart = 0;
        int groupCount = 0;
        while (matchAt(code, subject->value, start, matchData, matchStart,
                       matchEnd, nextStart, groupCount)) {
            out.append(subject->value, copied, matchStart - copied);
            if (literal) {
                out.append(literal->value);
            } else {
                auto replacement = block->native({buildMatch(
                    subject->value, code, pcre2_get_ovector_pointer(matchData),
                    static_cast<uint32_t>(groupCount))});
                out.append(replacement->toString());
            }
            copied = matchEnd;
            // An empty match consumed nothing, so the character it sits before
            // still has to be copied through as the cursor steps over it.
            if (nextStart > matchEnd && matchEnd < subject->value.size()) {
                out.append(subject->value, matchEnd, nextStart - matchEnd);
                copied = nextStart;
            }
            start = nextStart;
        }
        pcre2_match_data_free(matchData);
        if (copied < subject->value.size())
            out.append(subject->value, copied, std::string::npos);
        return Value::string(out);
    });

    // Regex::split(subject, re, limit) -> [String] — Ruby's semantics:
    //   * trailing empty fields dropped, leading ones kept
    //   * limit > 0 caps the field count, leaving the remainder unsplit
    //   * limit < 0 keeps trailing empties; limit == 0 is the default
    //   * capture groups are interleaved into the result
    defineIntrinsic("Regex::split", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto* subject = std::get_if<StringValue>(&args[0]->data);
        if (!subject) return Value::list({});

        int64_t limit = 0;
        if (args.size() >= 3)
            if (auto* i = std::get_if<IntValue>(&args[2]->data)) limit = i->value;

        // A plain String separator splits literally. `using Regex` makes the
        // BEAM backend route every `.split` through this module, including
        // `"a,b".split(",")`, so both entry points accept both separator kinds.
        if (auto* sep = std::get_if<StringValue>(&args[1]->data)) {
            std::vector<ValuePtr> parts;
            if (sep->value.empty()) {
                for (size_t i = 0; i < subject->value.size();) {
                    size_t next = i + 1;
                    while (next < subject->value.size() &&
                           (static_cast<unsigned char>(subject->value[next]) & 0xC0) == 0x80)
                        ++next;
                    parts.push_back(Value::string(subject->value.substr(i, next - i)));
                    i = next;
                }
            } else {
                size_t start = 0, pos;
                while ((pos = subject->value.find(sep->value, start)) != std::string::npos) {
                    parts.push_back(Value::string(subject->value.substr(start, pos - start)));
                    start = pos + sep->value.size();
                }
                parts.push_back(Value::string(subject->value.substr(start)));
            }
            return Value::list(std::move(parts));
        }

        auto fields = regexsupport::split(subject->value, args[1], limit);
        return fields ? Value::list(std::move(*fields)) : Value::list({});
    });

#endif // KEX_HAVE_PCRE2
}

} // namespace kex::interpreter
