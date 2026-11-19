#include "diagnostic_codes.hxx"

#include <algorithm>
#include <cctype>

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

} // namespace kex::diag
