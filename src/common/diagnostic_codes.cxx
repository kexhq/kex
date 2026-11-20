#include "diagnostic_codes.hxx"

#include "version.hxx"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace kex::diag {

auto entries() -> const std::vector<Entry>& {
    static const std::vector<Entry> all = {
#define KEX_DIAGNOSTIC(name, code, english) {Code::name, #name, code, english},
#define KEX_RETIRED_DIAGNOSTIC(code)
#include "diagnostic_codes/all.def"
#undef KEX_DIAGNOSTIC
#undef KEX_RETIRED_DIAGNOSTIC
    };
    return all;
}

auto retired() -> const std::vector<std::string_view>& {
    static const std::vector<std::string_view> all = {
#define KEX_DIAGNOSTIC(name, code, english)
#define KEX_RETIRED_DIAGNOSTIC(code) code,
#include "diagnostic_codes/all.def"
#undef KEX_DIAGNOSTIC
#undef KEX_RETIRED_DIAGNOSTIC
    };
    return all;
}

auto entry(Code code) -> const Entry* {
    const auto index = static_cast<std::size_t>(code);
    if (index == 0 || index > entries().size()) {
        return nullptr;
    }
    return &entries()[index - 1];
}

auto find(std::string_view id) -> const Entry* {
    const auto same = [&](std::string_view other) {
        return other.size() == id.size() &&
               std::equal(other.begin(), other.end(), id.begin(), [](char a, char b) {
                   return std::toupper(static_cast<unsigned char>(a)) ==
                          std::toupper(static_cast<unsigned char>(b));
               });
    };
    for (const auto& e : entries()) {
        if (same(e.id)) {
            return &e;
        }
    }
    return nullptr;
}

auto fill(std::string_view text, const Args& args) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            const auto close = text.find('}', i + 1);
            if (close != std::string_view::npos) {
                const auto name = text.substr(i + 1, close - i - 1);
                const auto found = std::find_if(
                    args.begin(), args.end(), [&](const Arg& arg) { return arg.first == name; });
                if (found != args.end()) {
                    out += found->second;
                    i = close;
                    continue;
                }
            }
        }
        out += text[i];
    }
    return out;
}

auto render(Code code, const Args& args) -> std::string {
    const auto* e = entry(code);
    return e ? fill(e->english, args) : std::string{};
}

auto slots(std::string_view text) -> std::vector<std::string> {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '{') {
            continue;
        }
        const auto close = text.find('}', i + 1);
        if (close == std::string_view::npos) {
            break;
        }
        std::string name{text.substr(i + 1, close - i - 1)};
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(std::move(name));
        }
        i = close;
    }
    return names;
}

auto idOf(Code code) -> std::string_view {
    const auto* e = entry(code);
    return e ? e->id : std::string_view{};
}

auto urlOf(Code code) -> std::string {
    const auto id = idOf(code);
    return id.empty() ? std::string{} : "https://oops.kex.run/" + std::string(id);
}

// `KEXET-0001`: the severity letter is the fourth character, the stage the
// fifth; wellFormed in tests/diagnostic_codes_test.cxx holds every code to
// that shape.
auto severityOf(const Entry& entry) -> std::string_view {
    return entry.id.size() > 3 && entry.id[3] == 'W' ? "warning" : "error";
}

auto stageOf(const Entry& entry) -> std::string_view {
    switch (entry.id.size() > 4 ? entry.id[4] : '?') {
        case 'L': return "lexical";
        case 'P': return "parse";
        case 'M': return "modules";
        case 'F': return "purity";
        case 'S': return "scope";
        case 'T': return "types";
        case 'C': return "compile-time";
        case 'B': return "lowering";
        default: return "unknown";
    }
}

namespace {

auto quoted(std::string_view text) -> std::string {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

} // namespace

auto entryJson(const Entry& entry) -> std::string {
    std::string out = "{\"code\": " + quoted(entry.id) + ", \"name\": " + quoted(entry.name) +
                      ", \"severity\": " + quoted(severityOf(entry)) +
                      ", \"stage\": " + quoted(stageOf(entry)) +
                      ", \"template\": " + quoted(entry.english) + ", \"slots\": [";
    const auto names = slots(entry.english);
    for (size_t i = 0; i < names.size(); ++i) {
        out += (i ? ", " : "") + quoted(names[i]);
    }
    return out + "], \"url\": " + quoted(urlOf(entry.code)) + "}";
}

auto catalogJson() -> std::string {
    std::string out = "{\n  \"version\": " + quoted(kex::versionNumber()) + ",\n  \"entries\": [";
    const auto& all = entries();
    for (size_t i = 0; i < all.size(); ++i) {
        out += (i ? ",\n    " : "\n    ") + entryJson(all[i]);
    }
    out += all.empty() ? "],\n  \"retired\": [" : "\n  ],\n  \"retired\": [";
    const auto& gone = retired();
    for (size_t i = 0; i < gone.size(); ++i) {
        out += (i ? ", " : "") + quoted(gone[i]);
    }
    return out + "]\n}\n";
}

} // namespace kex::diag
