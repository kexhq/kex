#include "../evaluator.hxx"
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
// their source string (see docs/regex-plan.md — a compiled pattern must not be
// embeddable in a .kexo artifact, so the source is the identity). The mutex
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

#endif // KEX_HAVE_PCRE2

} // namespace

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

        std::string out;
        out.reserve(s->value.size());
        for (unsigned char c : s->value) {
            // Escape ASCII non-alphanumerics; UTF-8 continuation bytes
            // (>= 0x80) are never metacharacters and must pass through
            // untouched or the escaped text stops being valid UTF-8.
            if (c < 0x80 && !std::isalnum(c) && c != '_') out.push_back('\\');
            out.push_back(static_cast<char>(c));
        }
        return Value::string(out);
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

#endif // KEX_HAVE_PCRE2
}

} // namespace kex::interpreter
