#include "../evaluator.hxx"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <array>
#ifndef _WIN32
#include <arpa/inet.h>
#endif

namespace kex::interpreter {
namespace {

struct ParsedURI {
    std::string source, scheme, authority, userinfo, host, port, path, query, fragment;
    bool hasAuthority = false, hasQuery = false, hasFragment = false;
};

auto uriError(const std::string& kind, const std::string& message,
              std::optional<int64_t> position = std::nullopt) -> ValuePtr {
    return Value::error(Value::record("URIError", {
        {"kind", Value::variant(kind, "URIErrorKind")},
        {"message", Value::string(message)},
        {"position", position ? Value::just(Value::integer(*position)) : Value::none()},
    }));
}

auto textOf(const ValuePtr& value) -> const std::string* {
    if (auto* text = std::get_if<StringValue>(&value->data)) return &text->value;
    if (auto* record = std::get_if<RecordValue>(&value->data)) {
        auto found = record->fields.find("source");
        if (found != record->fields.end())
            if (auto* text = std::get_if<StringValue>(&found->second->data)) return &text->value;
    }
    return nullptr;
}

auto validEscapes(const std::string& text, size_t* bad = nullptr) -> bool {
    auto hex = [](unsigned char c) { return std::isxdigit(c) != 0; };
    for (size_t i = 0; i < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) < 0x20 || text[i] == 0x7f || text[i] == ' ') {
            if (bad) *bad = i;
            return false;
        }
        if (text[i] == '%' && (i + 2 >= text.size() || !hex(text[i + 1]) || !hex(text[i + 2]))) {
            if (bad) *bad = i;
            return false;
        }
    }
    return true;
}

auto parseURI(const std::string& source, bool asciiOnly) -> std::optional<ParsedURI> {
    if (asciiOnly)
        for (unsigned char c : source) if (c >= 0x80) return std::nullopt;
    size_t bad = 0;
    if (!validEscapes(source, &bad)) return std::nullopt;
    ParsedURI out;
    out.source = source;
    size_t end = source.size();
    auto hash = source.find('#');
    if (hash != std::string::npos) { out.hasFragment = true; out.fragment = source.substr(hash + 1); end = hash; }
    auto question = source.find('?');
    if (question != std::string::npos && question < end) {
        out.hasQuery = true; out.query = source.substr(question + 1, end - question - 1); end = question;
    }
    auto colon = source.find(':');
    auto slash = source.find('/');
    if (colon != std::string::npos && colon < end && (slash == std::string::npos || colon < slash)) {
        if (colon == 0 || !std::isalpha(static_cast<unsigned char>(source[0]))) return std::nullopt;
        for (size_t i = 1; i < colon; ++i) {
            auto c = static_cast<unsigned char>(source[i]);
            if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return std::nullopt;
        }
        out.scheme = source.substr(0, colon);
        std::transform(out.scheme.begin(), out.scheme.end(), out.scheme.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    } else colon = std::string::npos;
    size_t start = colon == std::string::npos ? 0 : colon + 1;
    if (source.compare(start, 2, "//") == 0) {
        out.hasAuthority = true;
        auto authorityEnd = source.find('/', start + 2);
        if (authorityEnd == std::string::npos || authorityEnd > end) authorityEnd = end;
        out.authority = source.substr(start + 2, authorityEnd - start - 2);
        if (out.authority.empty()) return std::nullopt;
        auto at = out.authority.rfind('@');
        std::string hostport = out.authority;
        if (at != std::string::npos) { out.userinfo = out.authority.substr(0, at); hostport = out.authority.substr(at + 1); }
        if (!hostport.empty() && hostport.front() == '[') {
            auto close = hostport.find(']');
            if (close == std::string::npos) return std::nullopt;
            out.host = hostport.substr(0, close + 1);
            if (close + 1 < hostport.size()) {
                if (hostport[close + 1] != ':') return std::nullopt;
                out.port = hostport.substr(close + 2);
            }
        } else {
            auto portColon = hostport.rfind(':');
            if (portColon != std::string::npos) {
                out.host = hostport.substr(0, portColon);
                out.port = hostport.substr(portColon + 1);
            } else out.host = hostport;
        }
        if (out.host.empty()) return std::nullopt;
        if (!out.port.empty()) {
            if (!std::all_of(out.port.begin(), out.port.end(), [](unsigned char c) { return std::isdigit(c); })) return std::nullopt;
            try { if (std::stoul(out.port) > 65535) return std::nullopt; } catch (...) { return std::nullopt; }
        } else if (!hostport.empty() && hostport.back() == ':') return std::nullopt;
        start = authorityEnd;
    }
    out.path = source.substr(start, end - start);
    if (out.hasAuthority && !out.path.empty() && out.path.front() != '/') return std::nullopt;
    return out;
}

auto hexValue(char c) -> unsigned { return c <= '9' ? c - '0' : (std::tolower(c) - 'a' + 10); }

auto decodeComponent(const std::string& text, bool plus) -> std::optional<std::string> {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (plus && text[i] == '+') out += ' ';
        else if (text[i] == '%') {
            if (i + 2 >= text.size() || !std::isxdigit(static_cast<unsigned char>(text[i + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(text[i + 2]))) return std::nullopt;
            out += static_cast<char>((hexValue(text[i + 1]) << 4) | hexValue(text[i + 2])); i += 2;
        } else out += text[i];
    }
    return out;
}

auto encodeComponent(const std::string& text, bool form) -> std::string {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out << static_cast<char>(c);
        else if (form && c == ' ') out << '+';
        else out << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned>(c);
    }
    return out.str();
}

auto queryValue(const std::string& source, bool form) -> ValuePtr {
    std::vector<ValuePtr> entries;
    size_t start = 0;
    while (start <= source.size()) {
        auto amp = source.find('&', start);
        auto piece = source.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        auto equal = piece.find('=');
        auto key = decodeComponent(piece.substr(0, equal), form);
        auto val = equal == std::string::npos ? std::optional<std::string>{} : decodeComponent(piece.substr(equal + 1), form);
        if (!key || (equal != std::string::npos && !val)) return uriError("InvalidEscape", "invalid percent escape in query");
        entries.push_back(Value::tuple({Value::string(*key), equal == std::string::npos ? Value::none() : Value::just(Value::string(*val))}));
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return Value::ok(Value::record("Query", {{"entries", Value::list(std::move(entries))}}));
}

auto queryEncode(const ValuePtr& query, bool form) -> std::string {
    auto* record = std::get_if<RecordValue>(&query->data);
    if (!record) return {};
    auto found = record->fields.find("entries");
    if (found == record->fields.end()) return {};
    auto* list = std::get_if<ListValue>(&found->second->data);
    if (!list) return {};
    std::string out;
    for (const auto& entry : list->elements) {
        auto* tuple = std::get_if<TupleValue>(&entry->data);
        if (!tuple || tuple->elements.size() != 2) continue;
        auto* key = std::get_if<StringValue>(&tuple->elements[0]->data);
        if (!key) continue;
        if (!out.empty()) out += '&';
        out += encodeComponent(key->value, form);
        if (auto* option = std::get_if<VariantValue>(&tuple->elements[1]->data); option && option->tag == "Just" && !option->args.empty())
            if (auto* value = std::get_if<StringValue>(&option->args[0]->data)) out += "=" + encodeComponent(value->value, form);
    }
    return out;
}

auto removeDotSegments(const std::string& input) -> std::string {
    const bool absolute = !input.empty() && input.front() == '/';
    const bool trailing = !input.empty() && input.back() == '/';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= input.size()) {
        auto slash = input.find('/', start);
        auto part = input.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string out = absolute ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += '/'; out += parts[i]; }
    if (trailing && !out.empty() && out.back() != '/') out += '/';
    return out;
}

auto normalized(const ParsedURI& uri) -> std::string {
    std::string out;
    if (!uri.scheme.empty()) out += uri.scheme + ":";
    if (uri.hasAuthority) {
        out += "//";
        if (!uri.userinfo.empty()) out += uri.userinfo + "@";
        auto host = uri.host;
        std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        out += host;
        if (!uri.port.empty() && !((uri.scheme == "http" && uri.port == "80") || (uri.scheme == "https" && uri.port == "443"))) out += ":" + uri.port;
    }
    auto path = removeDotSegments(uri.path);
    for (size_t i = 0; i + 2 < path.size(); ++i) if (path[i] == '%') { path[i + 1] = static_cast<char>(std::toupper(path[i + 1])); path[i + 2] = static_cast<char>(std::toupper(path[i + 2])); i += 2; }
    out += path;
    if (uri.hasQuery) out += "?" + uri.query;
    if (uri.hasFragment) out += "#" + uri.fragment;
    return out;
}

auto uriRecord(const std::string& type, std::string source) -> ValuePtr {
    return Value::record(type, {{"source", Value::string(std::move(source))}});
}

auto netError(const std::string& kind, const std::string& operation,
              const std::string& message) -> ValuePtr {
    return Value::error(Value::record("NetError", {
        {"kind", Value::variant(kind, "NetErrorKind")},
        {"operation", Value::variant(operation, "NetOperation")},
        {"message", Value::string(message)},
        {"phase", Value::none()}, {"progress", Value::none()},
    }));
}

auto lowerASCII(std::string text) -> std::string {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

auto utf8Codepoints(const std::string& text) -> std::optional<std::vector<uint32_t>> {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = text[i++]; uint32_t cp; size_t extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; }
        else return std::nullopt;
        if (i + extra > text.size()) return std::nullopt;
        for (size_t j = 0; j < extra; ++j) { unsigned char d = text[i++]; if ((d & 0xc0) != 0x80) return std::nullopt; cp = (cp << 6) | (d & 0x3f); }
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return std::nullopt;
        out.push_back(cp);
    }
    return out;
}

auto punycodeLabel(const std::string& label) -> std::optional<std::string> {
    auto input = utf8Codepoints(label); if (!input) return std::nullopt;
    bool allASCII = std::all_of(input->begin(), input->end(), [](uint32_t c){ return c < 128; });
    if (allASCII) return lowerASCII(label);
    auto digit = [](uint32_t d) { return static_cast<char>(d < 26 ? 'a' + d : '0' + d - 26); };
    auto adapt = [](uint64_t delta, uint64_t points, bool first) { delta = first ? delta / 700 : delta / 2; delta += delta / points; uint64_t k = 0; while (delta > 455) { delta /= 35; k += 36; } return k + 36 * delta / (delta + 38); };
    std::string out = "xn--"; size_t basic = 0;
    for (auto cp : *input) if (cp < 128) { out += static_cast<char>(std::tolower(cp)); ++basic; }
    size_t handled = basic; if (basic) out += '-'; uint64_t n = 128, delta = 0, bias = 72;
    while (handled < input->size()) {
        uint64_t m = UINT64_MAX; for (auto cp : *input) if (cp >= n && cp < m) m = cp;
        if (m == UINT64_MAX || m - n > (UINT64_MAX - delta) / (handled + 1)) return std::nullopt;
        delta += (m - n) * (handled + 1); n = m;
        for (auto cp : *input) {
            if (cp < n) { if (++delta == 0) return std::nullopt; }
            if (cp != n) continue;
            uint64_t q = delta;
            for (uint64_t k = 36;; k += 36) { uint64_t t = k <= bias ? 1 : (k >= bias + 26 ? 26 : k - bias); if (q < t) break; out += digit(t + (q - t) % (36 - t)); q = (q - t) / (36 - t); }
            out += digit(q); bias = adapt(delta, handled + 1, handled == basic); delta = 0; ++handled;
        }
        ++delta; ++n;
    }
    return out;
}

auto idnaHost(const std::string& host) -> std::optional<std::string> {
    std::string out; size_t start = 0;
    while (start <= host.size()) { auto dot = host.find('.', start); auto label = host.substr(start, dot == std::string::npos ? std::string::npos : dot - start); if (label.empty()) return std::nullopt; auto ascii = punycodeLabel(label); if (!ascii || ascii->size() > 63) return std::nullopt; if (!out.empty()) out += '.'; out += *ascii; if (dot == std::string::npos) break; start = dot + 1; }
    return out.size() <= 253 ? std::optional<std::string>(out) : std::nullopt;
}

auto headerEntries(const ValuePtr& value) -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    auto* record = std::get_if<RecordValue>(&value->data);
    if (!record) return out;
    auto found = record->fields.find("entries");
    if (found == record->fields.end()) return out;
    auto* list = std::get_if<ListValue>(&found->second->data);
    if (!list) return out;
    for (const auto& item : list->elements) {
        auto* tuple = std::get_if<TupleValue>(&item->data);
        if (!tuple || tuple->elements.size() != 2) continue;
        auto* name = std::get_if<StringValue>(&tuple->elements[0]->data);
        auto* val = std::get_if<StringValue>(&tuple->elements[1]->data);
        if (name && val) out.emplace_back(name->value, val->value);
    }
    return out;
}

auto headersValue(const std::vector<std::pair<std::string, std::string>>& entries) -> ValuePtr {
    std::vector<ValuePtr> values;
    for (const auto& [name, value] : entries)
        values.push_back(Value::tuple({Value::string(name), Value::string(value)}));
    return Value::record("Headers", {{"entries", Value::list(std::move(values))}});
}

auto validHeader(const std::string& name, const std::string& value) -> bool {
    if (name.empty()) return false;
    static const std::string separators = "()<>@,;:\\\"/[]?={} \t";
    for (unsigned char c : name) if (c <= 0x20 || c >= 0x7f || separators.find(c) != std::string::npos) return false;
    for (unsigned char c : value) if (c == '\r' || c == '\n' || c == 0 || (c < 0x20 && c != '\t')) return false;
    return true;
}

#ifndef _WIN32
struct IPValue { int family = 0; std::array<unsigned char, 16> bytes{}; };

auto parseIP(const std::string& text) -> std::optional<IPValue> {
    IPValue out;
    if (inet_pton(AF_INET, text.c_str(), out.bytes.data()) == 1) out.family = AF_INET;
    else if (inet_pton(AF_INET6, text.c_str(), out.bytes.data()) == 1) out.family = AF_INET6;
    else return std::nullopt;
    return out;
}

auto renderIP(const IPValue& value) -> std::string {
    char buffer[INET6_ADDRSTRLEN]{};
    return inet_ntop(value.family, value.bytes.data(), buffer, sizeof(buffer)) ? buffer : "";
}
#endif

} // namespace

auto Evaluator::registerNetBuiltins() -> void {
    defineIntrinsic("URI::parse", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty() || !textOf(args[0])) return uriError("InvalidSyntax", "URI must be text");
        const auto& source = *textOf(args[0]);
        size_t bad = 0;
        for (unsigned char c : source) if (c >= 0x80) return uriError("NonASCII", "URI contains non-ASCII text");
        if (!validEscapes(source, &bad)) return uriError(source[bad] == '%' ? "InvalidEscape" : "InvalidSyntax", "invalid URI text", bad);
        if (!parseURI(source, true)) return uriError("InvalidSyntax", "malformed URI reference");
        return Value::ok(uriRecord("URI", source));
    });
    defineIntrinsic("URL::parse", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty() || !textOf(args[0])) return uriError("InvalidSyntax", "URL must be text");
        const auto& source = *textOf(args[0]);
        auto parsed = parseURI(source, true);
        if (!parsed) return uriError("InvalidSyntax", "malformed URL");
        if (parsed->scheme.empty()) return uriError("NotAbsolute", "URL requires a scheme");
        if (!parsed->hasAuthority) return uriError("MissingAuthority", "URL requires an authority");
        return Value::ok(uriRecord("URL", source));
    });
    defineIntrinsic("URI::fromIRI", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty() || !textOf(args[0])) return uriError("InvalidSyntax", "IRI must be text");
        const auto& input = *textOf(args[0]);
        std::string encoded; size_t authority = input.find("://"), hostStart = authority == std::string::npos ? std::string::npos : authority + 3;
        size_t hostEnd = hostStart == std::string::npos ? std::string::npos : input.find_first_of("/:?#", hostStart);
        if (hostEnd == std::string::npos && hostStart != std::string::npos) hostEnd = input.size();
        for (size_t i = 0; i < input.size();) {
            if (i == hostStart) { auto display = input.substr(hostStart, hostEnd - hostStart); auto ascii = idnaHost(display); if (!ascii) return uriError("InvalidAuthority", "invalid internationalized hostname"); encoded += *ascii; i = hostEnd; continue; }
            unsigned char c = input[i++];
            if (c < 0x80) encoded += static_cast<char>(c);
            else { std::ostringstream part; part << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(c); encoded += part.str(); }
        }
        if (!parseURI(encoded, true)) return uriError("InvalidSyntax", "malformed IRI reference");
        return Value::ok(uriRecord("URI", encoded));
    });
    defineIntrinsic("URL::build", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 4 || !textOf(args[0]) || !textOf(args[1])) return uriError("InvalidSyntax", "URL.build requires scheme, host, path, and query");
        auto scheme = lowerASCII(*textOf(args[0])); auto host = lowerASCII(*textOf(args[1]));
        if (scheme.empty() || host.empty()) return uriError("MissingAuthority", "URL requires scheme and host");
        std::string source = scheme + "://" + host;
        auto* path = std::get_if<ListValue>(&args[2]->data);
        if (path) for (const auto& segment : path->elements) if (textOf(segment)) source += "/" + encodeComponent(*textOf(segment), false);
        auto query = queryEncode(args[3], false); if (!query.empty()) source += "?" + query;
        auto parsed = parseURI(source, true); if (!parsed) return uriError("InvalidSyntax", "built URL is invalid");
        return Value::ok(uriRecord("URL", source));
    });
    defineIntrinsic("URI::normalize", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto text = args.empty() ? nullptr : textOf(args[0]); auto parsed = text ? parseURI(*text, false) : std::nullopt;
        return uriRecord("URI", parsed ? normalized(*parsed) : "");
    });
    defineIntrinsic("URL::normalize", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto text = args.empty() ? nullptr : textOf(args[0]); auto parsed = text ? parseURI(*text, false) : std::nullopt;
        return uriRecord("URL", parsed ? normalized(*parsed) : "");
    });
    auto scheme = [](std::vector<ValuePtr> args) -> ValuePtr {
        auto text = args.empty() ? nullptr : textOf(args[0]); auto parsed = text ? parseURI(*text, false) : std::nullopt;
        return parsed && !parsed->scheme.empty() ? Value::just(Value::string(parsed->scheme)) : Value::none();
    };
    defineIntrinsic("URI::scheme", scheme);
    defineIntrinsic("URL::scheme", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto text = args.empty() ? nullptr : textOf(args[0]); auto parsed = text ? parseURI(*text, false) : std::nullopt;
        return Value::string(parsed ? parsed->scheme : "");
    });
    auto host = [](std::vector<ValuePtr> args, bool optional) -> ValuePtr {
        auto text = args.empty() ? nullptr : textOf(args[0]); auto parsed = text ? parseURI(*text, false) : std::nullopt;
        if (!parsed || parsed->host.empty()) return optional ? Value::none() : Value::record("Host", {{"display", Value::string("")}, {"ascii", Value::string("")}});
        auto ascii = parsed->host; std::transform(ascii.begin(), ascii.end(), ascii.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto value = Value::record("Host", {{"display", Value::string(parsed->host)}, {"ascii", Value::string(ascii)}});
        return optional ? Value::just(value) : value;
    };
    defineIntrinsic("URI::host", [host](std::vector<ValuePtr> args) { return host(std::move(args), true); });
    defineIntrinsic("URL::host", [host](std::vector<ValuePtr> args) { return host(std::move(args), false); });
    auto query = [](std::vector<ValuePtr> args) -> ValuePtr {
        auto text = args.empty() ? nullptr : textOf(args[0]); auto parsed = text ? parseURI(*text, false) : std::nullopt;
        if (!parsed || !parsed->hasQuery) return Value::none();
        auto result = queryValue(parsed->query, false); auto* ok = std::get_if<VariantValue>(&result->data);
        return ok && !ok->args.empty() ? Value::just(ok->args[0]) : Value::none();
    };
    defineIntrinsic("URI::query", query); defineIntrinsic("URL::query", query);
    defineIntrinsic("URI::queryParse", [](std::vector<ValuePtr> args) { return queryValue(args.empty() || !textOf(args[0]) ? "" : *textOf(args[0]), false); });
    defineIntrinsic("URI::formParse", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto parsed = queryValue(args.empty() || !textOf(args[0]) ? "" : *textOf(args[0]), true);
        auto* result = std::get_if<VariantValue>(&parsed->data);
        if (!result || result->tag != "Ok" || result->args.empty()) return parsed;
        auto* query = std::get_if<RecordValue>(&result->args[0]->data);
        return Value::ok(Value::record("Form", {{"entries", query->fields.at("entries")}}));
    });
    defineIntrinsic("URI::queryEncode", [](std::vector<ValuePtr> args) { return Value::string(args.empty() ? "" : queryEncode(args[0], false)); });
    defineIntrinsic("URI::formFrom", [](std::vector<ValuePtr> args) -> ValuePtr {
        std::vector<ValuePtr> entries;
        if (!args.empty()) if (auto* list = std::get_if<ListValue>(&args[0]->data)) for (auto& item : list->elements) {
            auto* tuple = std::get_if<TupleValue>(&item->data); if (!tuple || tuple->elements.size() != 2) continue;
            entries.push_back(Value::tuple({tuple->elements[0], Value::just(tuple->elements[1])}));
        }
        return Value::record("Form", {{"entries", Value::list(std::move(entries))}});
    });
    defineIntrinsic("URI::formEncode", [](std::vector<ValuePtr> args) { return Value::string(args.empty() ? "" : queryEncode(args[0], true)); });
    defineIntrinsic("URI::resolve", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2 || !textOf(args[0]) || !textOf(args[1])) return uriError("InvalidSyntax", "resolve requires two URIs");
        auto base = parseURI(*textOf(args[0]), false), ref = parseURI(*textOf(args[1]), false);
        if (!base || !ref || base->scheme.empty()) return uriError("NotAbsolute", "base URI must be absolute");
        if (!ref->scheme.empty()) return Value::ok(uriRecord("URI", normalized(*ref)));
        ParsedURI target = *ref; target.scheme = base->scheme;
        if (!ref->hasAuthority) {
            target.hasAuthority = base->hasAuthority; target.authority = base->authority; target.userinfo = base->userinfo; target.host = base->host; target.port = base->port;
            if (ref->path.empty()) { target.path = base->path; if (!ref->hasQuery) { target.hasQuery = base->hasQuery; target.query = base->query; } }
            else if (ref->path.front() != '/') { auto slash = base->path.rfind('/'); target.path = (slash == std::string::npos ? "" : base->path.substr(0, slash + 1)) + ref->path; }
        }
        return Value::ok(uriRecord("URI", normalized(target)));
    });
    defineIntrinsic("URL::resolve", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2 || !textOf(args[0]) || !textOf(args[1])) return uriError("InvalidSyntax", "resolve requires URL and URI");
        auto base = parseURI(*textOf(args[0]), false), ref = parseURI(*textOf(args[1]), false);
        if (!base || !ref) return uriError("InvalidSyntax", "malformed URI");
        ParsedURI target = *ref;
        if (target.scheme.empty()) { target.scheme = base->scheme; if (!target.hasAuthority) { target.hasAuthority = true; target.authority = base->authority; target.userinfo = base->userinfo; target.host = base->host; target.port = base->port; if (target.path.empty()) target.path = base->path; else if (target.path.front() != '/') { auto slash = base->path.rfind('/'); target.path = base->path.substr(0, slash + 1) + target.path; } if (!target.hasQuery && target.path == base->path) { target.hasQuery = base->hasQuery; target.query = base->query; } } }
        if (!target.hasAuthority) return uriError("MissingAuthority", "resolved URL has no authority");
        return Value::ok(uriRecord("URL", normalized(target)));
    });

    defineIntrinsic("Net::port", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return netError("Parse", "Connect", "port must be between 0 and 65535");
        auto value = asInteger(args[0]);
        if (!value || *value < 0 || *value > 65535) return netError("Parse", "Connect", "port must be between 0 and 65535");
        return Value::ok(Value::record("Port", {{"value", integerResult(*value)}}));
    });
    defineIntrinsic("Net::support", [](std::vector<ValuePtr>) -> ValuePtr {
#ifdef __EMSCRIPTEN__
        const bool browser = true;
#else
        const bool browser = false;
#endif
        auto support = [](bool compiled, bool usable) { return Value::record("SupportValue", {{"compiled?", Value::boolean(compiled)}, {"usable?", Value::boolean(usable)}}); };
        return Value::record("SupportReport", {
            {"dns", support(false, false)}, {"tcp", support(false, false)}, {"udp", support(false, false)},
            {"unix", support(false, false)}, {"tls", support(false, false)},
            {"httpClient", support(browser, browser)}, {"httpServer", support(false, false)},
            {"webSocketClient", support(browser, browser)}, {"webSocketServer", support(false, false)},
        });
    });
    defineIntrinsic("Net::unsupported", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto operation = args.empty() || !textOf(args[0]) ? std::string("Backend") : *textOf(args[0]);
        return netError("UnsupportedBackend", operation, operation + " is not available on this backend");
    });
    defineIntrinsic("NetDNS::name", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty() || !textOf(args[0])) return netError("Parse", "DNS", "DNS name must be text");
        auto display = *textOf(args[0]); auto ascii = idnaHost(display);
        if (!ascii) return netError("Parse", "DNS", "invalid DNS name");
        for (size_t start = 0; start < ascii->size();) { auto dot = ascii->find('.', start); auto label = ascii->substr(start, dot == std::string::npos ? std::string::npos : dot - start); if (label.front() == '-' || label.back() == '-' || !std::all_of(label.begin(), label.end(), [](unsigned char c){ return std::isalnum(c) || c == '-'; })) return netError("Parse", "DNS", "invalid DNS label"); if (dot == std::string::npos) break; start = dot + 1; }
        return Value::ok(Value::record("Name", {{"display", Value::string(display)}, {"ascii", Value::string(*ascii)}}));
    });
    defineIntrinsic("NetDNS::addresses", [](std::vector<ValuePtr>) -> ValuePtr {
        return netError("UnsupportedBackend", "DNS", "DNS is available only through a mock on the tree-walking backend");
    });

#ifndef _WIN32
    defineIntrinsic("NetIP::address", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty() || !textOf(args[0])) return netError("Parse", "Connect", "address must be text");
        auto parsed = parseIP(*textOf(args[0]));
        if (!parsed) return netError("Parse", "Connect", "invalid IP address");
        return Value::ok(uriRecord("Address", renderIP(*parsed)));
    });
    defineIntrinsic("NetIP::network", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty() || !textOf(args[0])) return netError("Parse", "Connect", "network must be text");
        auto source = *textOf(args[0]); auto slash = source.rfind('/');
        if (slash == std::string::npos) return netError("Parse", "Connect", "network requires a prefix length");
        auto ip = parseIP(source.substr(0, slash)); if (!ip) return netError("Parse", "Connect", "invalid network address");
        int prefix = -1; try { size_t used = 0; prefix = std::stoi(source.substr(slash + 1), &used); if (used != source.size() - slash - 1) prefix = -1; } catch (...) {}
        int bits = ip->family == AF_INET ? 32 : 128; if (prefix < 0 || prefix > bits) return netError("Parse", "Connect", "invalid network prefix");
        for (int bit = prefix; bit < bits; ++bit) ip->bytes[bit / 8] &= static_cast<unsigned char>(~(1u << (7 - bit % 8)));
        return Value::ok(uriRecord("Network", renderIP(*ip) + "/" + std::to_string(prefix)));
    });
    auto parsedAddressArg = [](const ValuePtr& value) { auto text = textOf(value); return text ? parseIP(*text) : std::optional<IPValue>{}; };
    defineIntrinsic("NetIP::addressString", [parsedAddressArg](std::vector<ValuePtr> args) { auto ip = args.empty() ? std::optional<IPValue>{} : parsedAddressArg(args[0]); return Value::string(ip ? renderIP(*ip) : ""); });
    defineIntrinsic("NetIP::networkString", [](std::vector<ValuePtr> args) { return Value::string(args.empty() || !textOf(args[0]) ? "" : *textOf(args[0])); });
    defineIntrinsic("NetIP::version", [parsedAddressArg](std::vector<ValuePtr> args) { auto ip = args.empty() ? std::optional<IPValue>{} : parsedAddressArg(args[0]); return Value::integer(ip && ip->family == AF_INET6 ? 6 : 4); });
    defineIntrinsic("NetIP::loopback?", [parsedAddressArg](std::vector<ValuePtr> args) { auto ip = args.empty() ? std::optional<IPValue>{} : parsedAddressArg(args[0]); return Value::boolean(ip && ((ip->family == AF_INET && ip->bytes[0] == 127) || (ip->family == AF_INET6 && std::all_of(ip->bytes.begin(), ip->bytes.end() - 1, [](auto b){ return b == 0; }) && ip->bytes[15] == 1))); });
    defineIntrinsic("NetIP::private?", [parsedAddressArg](std::vector<ValuePtr> args) { auto ip = args.empty() ? std::optional<IPValue>{} : parsedAddressArg(args[0]); bool yes = ip && ((ip->family == AF_INET && (ip->bytes[0] == 10 || (ip->bytes[0] == 172 && ip->bytes[1] >= 16 && ip->bytes[1] <= 31) || (ip->bytes[0] == 192 && ip->bytes[1] == 168))) || (ip->family == AF_INET6 && (ip->bytes[0] & 0xfe) == 0xfc)); return Value::boolean(yes); });
    defineIntrinsic("NetIP::unspecified?", [parsedAddressArg](std::vector<ValuePtr> args) { auto ip = args.empty() ? std::optional<IPValue>{} : parsedAddressArg(args[0]); return Value::boolean(ip && std::all_of(ip->bytes.begin(), ip->bytes.begin() + (ip->family == AF_INET ? 4 : 16), [](auto b){ return b == 0; })); });
    defineIntrinsic("NetIP::multicast?", [parsedAddressArg](std::vector<ValuePtr> args) { auto ip = args.empty() ? std::optional<IPValue>{} : parsedAddressArg(args[0]); return Value::boolean(ip && ((ip->family == AF_INET && ip->bytes[0] >= 224 && ip->bytes[0] <= 239) || (ip->family == AF_INET6 && ip->bytes[0] == 0xff))); });
    defineIntrinsic("NetIP::prefix", [](std::vector<ValuePtr> args) { auto text = args.empty() ? nullptr : textOf(args[0]); return Value::integer(text ? std::stoi(text->substr(text->rfind('/') + 1)) : 0); });
    defineIntrinsic("NetIP::contains?", [](std::vector<ValuePtr> args) { if (args.size() < 2 || !textOf(args[0]) || !textOf(args[1])) return Value::boolean(false); auto networkText=*textOf(args[0]); auto slash=networkText.rfind('/'); auto net=parseIP(networkText.substr(0,slash)), ip=parseIP(*textOf(args[1])); if(!net||!ip||net->family!=ip->family)return Value::boolean(false); int prefix=std::stoi(networkText.substr(slash+1)); for(int bit=0;bit<prefix;++bit)if((net->bytes[bit/8]&(1u<<(7-bit%8)))!=(ip->bytes[bit/8]&(1u<<(7-bit%8))))return Value::boolean(false); return Value::boolean(true); });
    auto networkEdge = [](std::vector<ValuePtr> args, bool last) -> ValuePtr { if(args.empty()||!textOf(args[0]))return uriRecord("Address",""); auto text=*textOf(args[0]); auto slash=text.rfind('/'); auto ip=parseIP(text.substr(0,slash)); int prefix=std::stoi(text.substr(slash+1)); int bits=ip->family==AF_INET?32:128; if(last)for(int bit=prefix;bit<bits;++bit)ip->bytes[bit/8]|=static_cast<unsigned char>(1u<<(7-bit%8)); return uriRecord("Address",renderIP(*ip)); };
    defineIntrinsic("NetIP::first", [networkEdge](std::vector<ValuePtr> args){return networkEdge(std::move(args),false);});
    defineIntrinsic("NetIP::last", [networkEdge](std::vector<ValuePtr> args){return networkEdge(std::move(args),true);});
#endif

    defineIntrinsic("NetHTTP::headers", [](std::vector<ValuePtr> args) -> ValuePtr { auto entries=args.empty()?std::vector<std::pair<std::string,std::string>>{}:headerEntries(Value::record("Headers",{{"entries",args[0]}})); for(auto& [n,v]:entries)if(!validHeader(n,v))return netError("Parse","HTTPClient","invalid HTTP header"); return Value::ok(headersValue(entries)); });
    defineIntrinsic("NetHTTP::parseHeaders", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty() || !textOf(args[0])) return netError("Parse", "HTTPClient", "headers must be text");
        std::vector<std::pair<std::string, std::string>> entries; std::istringstream input(*textOf(args[0])); std::string line;
        while (std::getline(input, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); if (line.empty()) continue; if (line.front() == ' ' || line.front() == '\t') return netError("Parse", "HTTPClient", "obsolete folded headers are rejected"); auto colon = line.find(':'); if (colon == std::string::npos) return netError("Parse", "HTTPClient", "header line requires a colon"); auto name=line.substr(0,colon), value=line.substr(colon+1); while(!value.empty()&&(value.front()==' '||value.front()=='\t'))value.erase(value.begin()); while(!value.empty()&&(value.back()==' '||value.back()=='\t'))value.pop_back(); if(!validHeader(name,value))return netError("Parse","HTTPClient","invalid HTTP header"); entries.emplace_back(name,value); }
        return Value::ok(headersValue(entries));
    });
    defineIntrinsic("NetHTTP::addHeader", [](std::vector<ValuePtr> args) { auto entries=headerEntries(args[0]); if(args.size()>=3&&textOf(args[1])&&textOf(args[2])&&validHeader(*textOf(args[1]),*textOf(args[2])))entries.emplace_back(*textOf(args[1]),*textOf(args[2])); return headersValue(entries); });
    defineIntrinsic("NetHTTP::setHeader", [](std::vector<ValuePtr> args) { auto entries=headerEntries(args[0]); if(args.size()<3||!textOf(args[1])||!textOf(args[2])||!validHeader(*textOf(args[1]),*textOf(args[2])))return headersValue(entries); auto key=lowerASCII(*textOf(args[1])); entries.erase(std::remove_if(entries.begin(),entries.end(),[&](auto& e){return lowerASCII(e.first)==key;}),entries.end()); entries.emplace_back(*textOf(args[1]),*textOf(args[2])); return headersValue(entries); });
    defineIntrinsic("NetHTTP::removeHeader", [](std::vector<ValuePtr> args) { auto entries=headerEntries(args[0]); if(args.size()>1&&textOf(args[1])){auto key=lowerASCII(*textOf(args[1]));entries.erase(std::remove_if(entries.begin(),entries.end(),[&](auto&e){return lowerASCII(e.first)==key;}),entries.end());} return headersValue(entries); });
    defineIntrinsic("NetHTTP::getAllHeaders", [](std::vector<ValuePtr> args) { std::vector<ValuePtr> out; if(args.size()>1&&textOf(args[1])){auto key=lowerASCII(*textOf(args[1]));for(auto&[n,v]:headerEntries(args[0]))if(lowerASCII(n)==key)out.push_back(Value::string(v));}return Value::list(std::move(out)); });
    defineIntrinsic("NetHTTP::getHeader", [](std::vector<ValuePtr> args) { if(args.size()>1&&textOf(args[1])){auto key=lowerASCII(*textOf(args[1]));for(auto&[n,v]:headerEntries(args[0]))if(lowerASCII(n)==key)return Value::just(Value::string(v));}return Value::none(); });
    defineIntrinsic("NetHTTP::status", [](std::vector<ValuePtr> args) -> ValuePtr { auto code=args.empty()?std::optional<mpz_class>{}:asInteger(args[0]); if(!code||*code<100||*code>599)return netError("Parse","HTTPClient","HTTP status must be between 100 and 599"); return Value::ok(Value::record("Status",{{"code",integerResult(*code)}})); });
    defineIntrinsic("NetHTTP::responseBinary", [](std::vector<ValuePtr> args) { return Value::record("Response", {{"status", Value::record("Status", {{"code", args[0]}})}, {"headers", args[2]}, {"body", args[1]}}); });
    defineIntrinsic("NetHTTP::responseText", [](std::vector<ValuePtr> args) { auto text = *textOf(args[1]); return Value::record("Response", {{"status", Value::record("Status", {{"code", args[0]}})}, {"headers", headersValue({{"Content-Type", "text/plain; charset=utf-8"}})}, {"body", Value::binary({text.begin(), text.end()})}}); });
    defineIntrinsic("NetHTTP::responseEmpty", [](std::vector<ValuePtr> args) { return Value::record("Response", {{"status", Value::record("Status", {{"code", args[0]}})}, {"headers", headersValue({})}, {"body", Value::binary({})}}); });
    defineIntrinsic("NetHTTP::get", [](std::vector<ValuePtr>) { return netError("UnsupportedBackend","HTTPClient","HTTP client is not available on the tree-walking backend"); });
    defineIntrinsic("NetHTTP::request", [](std::vector<ValuePtr>) { return netError("UnsupportedBackend","HTTPClient","HTTP client is not available on the tree-walking backend"); });
    for (const char* name : {"NetHTTPServer::start", "NetHTTPServer::serve",
                             "NetHTTPServer::stop", "NetHTTPServer::join"})
        defineIntrinsic(name, [](std::vector<ValuePtr>) { return netError("UnsupportedBackend","HTTPServer","HTTP server is not available on the tree-walking backend"); });
    defineIntrinsic("NetHTTPServer::running?", [](std::vector<ValuePtr>) { return Value::boolean(false); });
    defineIntrinsic("NetHTTPServer::localAddress", [](std::vector<ValuePtr>) { return Value::unit(); });
    for (const char* name : {"NetHTTPClient::open", "NetHTTPClient::request",
                             "NetHTTPClient::get", "NetHTTPClient::post"})
        defineIntrinsic(name, [](std::vector<ValuePtr>) { return netError("UnsupportedBackend","HTTPClient","HTTP client is not available on the tree-walking backend"); });
    defineIntrinsic("NetHTTPClient::statistics", [](std::vector<ValuePtr>) {
        return Value::record("ClientStatistics", {{"openConnections", Value::integer(0)},
                                                    {"requests", Value::integer(0)},
                                                    {"reusedConnections", Value::integer(0)}});
    });
    defineIntrinsic("NetHTTPClient::close", [](std::vector<ValuePtr>) {
        return Value::ok(Value::record("ClientCloseReport", {{"closedConnections", Value::integer(0)}}));
    });

    auto unsupportedSocket = [](const std::string& operation) {
        return [operation](std::vector<ValuePtr>) {
            return netError("UnsupportedBackend", operation,
                            operation + " is available only through a mock on the tree-walking backend");
        };
    };
    for (const char* name : {"NetTCP::connect", "NetTCP::listen", "NetTCP::accept",
                             "NetTCP::sendAll", "NetTCP::receiveChunk",
                             "NetTCP::receiveExactly", "NetTCP::receiveLine",
                             "NetTCP::shutdownWrite"})
        defineIntrinsic(name, unsupportedSocket("TCP"));
    for (const char* name : {"NetUDP::bind", "NetUDP::sendTo", "NetUDP::receiveFrom"})
        defineIntrinsic(name, unsupportedSocket("UDP"));
    for (const char* name : {"NetUnix::connect", "NetUnix::listen", "NetUnix::accept",
                             "NetUnix::sendAll", "NetUnix::receiveChunk"})
        defineIntrinsic(name, unsupportedSocket("Unix"));
    for (const char* name : {"NetTLS::connect", "NetTLS::sendAll", "NetTLS::receiveChunk"})
        defineIntrinsic(name, unsupportedSocket("TLS"));
    for (const char* name : {"NetSocket::sendAll", "NetSocket::receiveChunk", "NetSocket::accept"})
        defineIntrinsic(name, unsupportedSocket("Socket"));
    for (const char* name : {"NetTCP::close", "NetUDP::close", "NetUnix::close", "NetTLS::close"})
        defineIntrinsic(name, [](std::vector<ValuePtr>) { return Value::unit(); });
    for (const char* name : {"NetTCP::closed?", "NetUDP::closed?"})
        defineIntrinsic(name, [](std::vector<ValuePtr>) { return Value::boolean(true); });
    defineIntrinsic("NetSocket::close", [](std::vector<ValuePtr>) { return Value::unit(); });
    defineIntrinsic("NetSocket::closed?", [](std::vector<ValuePtr>) { return Value::boolean(true); });
    defineIntrinsic("NetUnix::address", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty() || !textOf(args[0]) || textOf(args[0])->empty() || textOf(args[0])->front() != '/')
            return netError("Parse", "Unix", "Unix socket path must be absolute");
        return Value::ok(Value::record("Address", {{"path", Value::string(*textOf(args[0]))}}));
    });
}

} // namespace kex::interpreter
