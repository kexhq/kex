#include "resolve_core.hxx"
#include "../common/well_known.hxx"
#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <variant>

namespace kex::semantic::resolve_core {

auto resolveModulePathIn(const std::string& currentModule,
                         const std::string& requested,
                         const std::function<bool(const std::string&)>& exists)
    -> std::string {
    if (exists(requested)) return requested;
    for (auto context = currentModule; !context.empty();) {
        const auto relative = context + "." + requested;
        if (exists(relative)) return relative;
        const auto dot = context.rfind('.');
        if (dot == std::string::npos) break;
        context.resize(dot);
    }
    return requested;
}

auto mergeImportOrigin(std::vector<ImportScope>& scopes,
                       const std::string& name, ImportOrigin origin)
    -> MergeOutcome {
    if (scopes.empty()) scopes.emplace_back();
    ImportOrigin* previous = nullptr;
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        if (auto found = it->find(name); found != it->end()) {
            previous = &found->second;
            break;
        }
    }
    if (previous && previous->module != origin.module) {
        // Only two BARE names can collide. Either side being a receiver
        // method settles it: the bare name belongs to whichever import
        // actually claims it, and a second receiver method changes
        // nothing (kexhq/kex#272).
        if (origin.receiverMethod) {
            // Leave the standing origin alone — a bare-name owner keeps
            // the name, and another method has nothing to claim.
        } else if (previous->receiverMethod) {
            *previous = {origin.module, origin.explicitImport, false};
        } else if (origin.explicitImport && !previous->explicitImport) {
            *previous = {origin.module, true, false};
        } else if (!origin.explicitImport && previous->explicitImport) {
            // The standing explicit import keeps the name; this one
            // binds nothing locally.
            return {.proceed = false};
        } else {
            return {.proceed = false,
                    .ambiguity = "ambiguous name `" + name +
                        "`, imported from both `" + previous->module +
                        "` and `" + origin.module + "`"};
        }
    } else if (!previous) {
        scopes.back()[name] = std::move(origin);
    }
    return {};
}

auto usingImportsNames(const std::optional<std::string>& alias,
                       const std::vector<std::string>& only) -> bool {
    return !alias || !only.empty();
}

auto usingSelectsName(const std::string& name,
                      const std::vector<std::string>& only,
                      const std::vector<std::string>& except) -> bool {
    if (!only.empty() &&
        std::find(only.begin(), only.end(), name) == only.end())
        return false;
    return std::find(except.begin(), except.end(), name) == except.end();
}

namespace {

auto lastSegment(const std::string& dotted) -> std::string_view {
    const auto dot = dotted.rfind('.');
    if (dot == std::string::npos) return std::string_view(dotted);
    return std::string_view(dotted).substr(dot + 1);
}

// Whether a nominal head names a declared record/ADT/distinct type. Bare
// heads match by last segment (a bare head provably emitted bare, so the
// spelling is self-proving); qualified heads
// match records exactly only (a qualified alias/opaque/ADT head may
// have emitted bare — `ownedByModule` qualifies what `makeTargetNames`
// leaves alone — while records qualify on both sides).
auto nominalHeadDeclared(const std::string& head,
                         const ImportedInterfaces& ifaces) -> bool {
    const bool qualified = head.find('.') != std::string::npos;
    if (qualified) return ifaces.recordFields.count(head) > 0;
    const auto seg = lastSegment(head);
    for (const auto& [key, _] : ifaces.recordFields)
        if (lastSegment(key) == seg) return true;
    for (const auto& adt : ifaces.adts)
        if (lastSegment(adt.name) == seg) return true;
    for (const auto& [key, _] : ifaces.distinctTypes)
        if (lastSegment(key) == seg) return true;
    return false;
}

} // namespace

auto writtenTargetHead(const TypePtr& type) -> std::optional<TargetHead> {
    if (!type) return std::nullopt;
    return std::visit(
        [](const auto& node) -> std::optional<TargetHead> {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, PrimitiveType>) {
                switch (node.kind) {
                    case PrimitiveType::Integer:
                        return TargetHead{"Integer"};
                    case PrimitiveType::Char: return TargetHead{"Char"};
                    case PrimitiveType::String: return TargetHead{"String"};
                    case PrimitiveType::Bool: return TargetHead{"Bool"};
                    case PrimitiveType::Atom:
                        if (!node.atomName.empty()) return std::nullopt;
                        return TargetHead{"Atom"};
                    case PrimitiveType::Unit: return std::nullopt;
                }
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, SizedIntType>) {
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, SizedFloatType>) {
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, NamedType>) {
                if (node.name.empty() || node.name == "Block" ||
                    node.name == "Optional")
                    return std::nullopt;
                return TargetHead{node.name, true};
            } else if constexpr (std::is_same_v<T, ListType>) {
                return TargetHead{"List"};
            } else if constexpr (std::is_same_v<T, MapType>) {
                return TargetHead{"Map"};
            } else {
                return std::nullopt;
            }
        },
        type->kind);
}

auto receiverCoversHead(const TypePtr& receiver, const std::string& head,
                        const ImportedInterfaces& ifaces) -> bool {
    if (!receiver || head.empty()) return false;
    return std::visit(
        [&](const auto& node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, PrimitiveType>) {
                switch (node.kind) {
                    case PrimitiveType::Integer: return head == "Integer";
                    case PrimitiveType::Char: return head == "Char";
                    case PrimitiveType::String: return head == "String";
                    case PrimitiveType::Bool: return head == "Bool";
                    case PrimitiveType::Atom: return head == "Atom";
                    case PrimitiveType::Unit: return false;
                }
                return false;
            } else if constexpr (std::is_same_v<T, NamedType>) {
                return node.name == head ||
                       lastSegment(node.name) == lastSegment(head);
            } else if constexpr (std::is_same_v<T, ListType>) {
                return head == kex::names::kListType;
            } else if constexpr (std::is_same_v<T, MapType>) {
                return head == kex::names::kMapType;
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                return head == "Optional";
            } else if constexpr (std::is_same_v<T, RecordType>) {
                return nominalHeadDeclared(head, ifaces);
            } else {
                return false;
            }
        },
        receiver->kind);
}

auto importDispatchSpelling(
    const ImportedFunction& winner, const TypePtr& receiverType,
    const ImportedInterfaces& ifaces, AliasExpander expandAlias)
    -> std::optional<std::string> {
    // Emergency disable (and before/after measurement): with
    // KEX_NO_DEVIRT set, every imported call keeps the dispatcher.
    if (std::getenv("KEX_NO_DEVIRT")) return std::nullopt;
    // (0): a pure make-block winner with a receiver. Module
    // functions (`isReceiverMethod == false`) keep the bare name —
    // their `name/T` belongs to a same-headed make block, if one
    // exists at all — and remote slots lower to a server call, not
    // to this function. Foul winners keep the dispatcher: `callE`
    // appends the capability context by consulting a registry keyed
    // on the BARE name, so a mangled foul call would go out one
    // argument short of its definition.
    if (!winner.isReceiverMethod || winner.remoteSlot ||
        winner.signature.isFoul || winner.signature.params.empty())
        return std::nullopt;
    // (1): a provable emitted head for the winner's written receiver.
    const auto winnerHead = writtenTargetHead(winner.signature.params[0]);
    if (!winnerHead) return std::nullopt;
    // (1b): a nominal head names a declared record/ADT/distinct type
    // (aliases and opaques keep the dispatcher).
    if (winnerHead->nominal &&
        !nominalHeadDeclared(winnerHead->spelling, ifaces))
        return std::nullopt;
    // The call receiver, aliases expanded; gradual receivers stay
    // dynamic (0b).
    const TypePtr expanded =
        expandAlias ? expandAlias(receiverType) : receiverType;
    // Clause (0b): a gradual (partially unknown) receiver may carry a value of
    // another type — static dispatch would lie where the dispatcher adapts.
    // A missing type is as unknown as `?`.
    if (mentionsUnknown(expanded, NullType::Unknown)) return std::nullopt;

    const auto& head = winnerHead->spelling;
    // The call's receiver is of the winner's head: every value it can hold
    // is one of that type.
    if (!receiverCoversHead(expanded, head, ifaces)) return std::nullopt;
    // A value of that type reaches `name/Head` through the dispatcher, as its
    // module recorded when it emitted it (KexiMethod::implementation: the
    // routes lowering laid out, nothing merged in ahead). That one fact
    // replaces the routing clauses (5), (12), (13b), (DS) and the `''` and
    // module-function vetoes, which re-derived it from the overloads.
    if (winner.implementation != winner.sourceName + "/" + head)
        return std::nullopt;

    // `name/Head`, spelled the way `mangleReceiverImplementation`
    // (lower_internal.hxx) spells it.
    return winner.sourceName + "/" + head;
}

} // namespace kex::semantic::resolve_core
