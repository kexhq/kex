#include "resolve_core.hxx"
#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <unordered_set>
#include <variant>

namespace kex::semantic::resolve_core {

auto resolveSourceModulePath(SemanticDB* db,
                             const std::string& currentModule,
                             const std::string& requested) -> std::string {
    std::string resolved = requested;
    if (db && !db->hasModule(resolved)) {
        if (auto discovered = db->ensureModule(requested, currentModule))
            resolved = *discovered;
    }
    if (db && !db->hasModule(resolved) && !currentModule.empty()) {
        auto context = currentModule;
        while (!context.empty()) {
            const auto relative = context + "." + requested;
            if (db->hasModule(relative)) { resolved = relative; break; }
            const auto dot = context.rfind('.');
            if (dot == std::string::npos) break;
            context.resize(dot);
        }
    }
    return resolved;
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

// Clause (1b): a nominal head names a declared record/ADT/distinct
// type. Bare heads match by last segment (a bare head provably
// emitted bare, so the spelling is self-proving); qualified heads
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

auto typesAlphaEqual(const TypePtr& a, const TypePtr& b,
                     std::unordered_map<int, int>& aRanks,
                     std::unordered_map<int, int>& bRanks) -> bool {
    if (!a || !b) return !a && !b;
    if (a->kind.index() != b->kind.index()) return false;
    return std::visit(
        [&](const auto& aNode) -> bool {
            using T = std::decay_t<decltype(aNode)>;
            const auto& bNode = std::get<T>(b->kind);
            if constexpr (std::is_same_v<T, PrimitiveType>) {
                return aNode.kind == bNode.kind;
            } else if constexpr (std::is_same_v<T, SizedIntType>) {
                return aNode.bits == bNode.bits &&
                       aNode.isSigned == bNode.isSigned;
            } else if constexpr (std::is_same_v<T, SizedFloatType>) {
                return aNode.bits == bNode.bits;
            } else if constexpr (std::is_same_v<T, NamedType>) {
                if (aNode.name != bNode.name ||
                    aNode.typeArgs.size() != bNode.typeArgs.size())
                    return false;
                for (size_t i = 0; i < aNode.typeArgs.size(); i++)
                    if (!typesAlphaEqual(aNode.typeArgs[i], bNode.typeArgs[i],
                                         aRanks, bRanks))
                        return false;
                return true;
            } else if constexpr (std::is_same_v<T, FuncType>) {
                if (aNode.block != bNode.block ||
                    aNode.params.size() != bNode.params.size())
                    return false;
                for (size_t i = 0; i < aNode.params.size(); i++)
                    if (!typesAlphaEqual(aNode.params[i], bNode.params[i],
                                         aRanks, bRanks))
                        return false;
                return typesAlphaEqual(aNode.result, bNode.result, aRanks,
                                       bRanks);
            } else if constexpr (std::is_same_v<T, TupleType>) {
                if (aNode.elements.size() != bNode.elements.size())
                    return false;
                for (size_t i = 0; i < aNode.elements.size(); i++)
                    if (!typesAlphaEqual(aNode.elements[i], bNode.elements[i],
                                         aRanks, bRanks))
                        return false;
                return true;
            } else if constexpr (std::is_same_v<T, ListType>) {
                return typesAlphaEqual(aNode.element, bNode.element, aRanks,
                                       bRanks);
            } else if constexpr (std::is_same_v<T, MapType>) {
                return typesAlphaEqual(aNode.key, bNode.key, aRanks, bRanks) &&
                       typesAlphaEqual(aNode.value, bNode.value, aRanks,
                                       bRanks);
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                return typesAlphaEqual(aNode.inner, bNode.inner, aRanks,
                                       bRanks);
            } else if constexpr (std::is_same_v<T, UnionType>) {
                if (aNode.members.size() != bNode.members.size())
                    return false;
                for (size_t i = 0; i < aNode.members.size(); i++)
                    if (!typesAlphaEqual(aNode.members[i], bNode.members[i],
                                         aRanks, bRanks))
                        return false;
                return true;
            } else if constexpr (std::is_same_v<T, IntersectionType>) {
                if (aNode.members.size() != bNode.members.size())
                    return false;
                for (size_t i = 0; i < aNode.members.size(); i++)
                    if (!typesAlphaEqual(aNode.members[i], bNode.members[i],
                                         aRanks, bRanks))
                        return false;
                return true;
            } else if constexpr (std::is_same_v<T, RecordType>) {
                if (aNode.fields.size() != bNode.fields.size()) return false;
                for (size_t i = 0; i < aNode.fields.size(); i++) {
                    if (aNode.fields[i].first != bNode.fields[i].first)
                        return false;
                    if (!typesAlphaEqual(aNode.fields[i].second,
                                         bNode.fields[i].second, aRanks,
                                         bRanks))
                        return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, TypeVar>) {
                auto rank = [](std::unordered_map<int, int>& ranks, int id) {
                    auto found = ranks.find(id);
                    if (found != ranks.end()) return found->second;
                    const auto fresh = static_cast<int>(ranks.size());
                    ranks.emplace(id, fresh);
                    return fresh;
                };
                return rank(aRanks, aNode.id) == rank(bRanks, bNode.id);
            } else if constexpr (std::is_same_v<T, UnknownType>) {
                return true;
            } else if constexpr (std::is_same_v<T, VoidType>) {
                return true;
            } else if constexpr (std::is_same_v<T, ConstrainedType>) {
                if (aNode.traitName != bNode.traitName) return false;
                if ((aNode.genericId == 0) != (bNode.genericId == 0))
                    return false;
                if (aNode.genericId == 0) return true;
                auto rank = [](std::unordered_map<int, int>& ranks, int id) {
                    auto found = ranks.find(id);
                    if (found != ranks.end()) return found->second;
                    const auto fresh = static_cast<int>(ranks.size());
                    ranks.emplace(id, fresh);
                    return fresh;
                };
                return rank(aRanks, aNode.genericId) ==
                       rank(bRanks, bNode.genericId);
            } else {
                return false;
            }
        },
        a->kind);
}

// Clause (0b): a gradual (partially unknown) receiver may carry a
// value of another type — static dispatch would lie where the
// dispatcher adapts.
auto mentionsUnknown(const TypePtr& type) -> bool {
    if (!type) return true;
    return std::visit(
        [](const auto& node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, UnknownType>) {
                return true;
            } else if constexpr (std::is_same_v<T, NamedType>) {
                for (const auto& arg : node.typeArgs)
                    if (mentionsUnknown(arg)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, FuncType>) {
                for (const auto& param : node.params)
                    if (mentionsUnknown(param)) return true;
                return mentionsUnknown(node.result);
            } else if constexpr (std::is_same_v<T, TupleType>) {
                for (const auto& element : node.elements)
                    if (mentionsUnknown(element)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, ListType>) {
                return mentionsUnknown(node.element);
            } else if constexpr (std::is_same_v<T, MapType>) {
                return mentionsUnknown(node.key) ||
                       mentionsUnknown(node.value);
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                return mentionsUnknown(node.inner);
            } else if constexpr (std::is_same_v<T, UnionType>) {
                for (const auto& member : node.members)
                    if (mentionsUnknown(member)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, IntersectionType>) {
                for (const auto& member : node.members)
                    if (mentionsUnknown(member)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, RecordType>) {
                for (const auto& [_, field] : node.fields)
                    if (mentionsUnknown(field)) return true;
                return false;
            } else {
                return false;
            }
        },
        type->kind);
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

auto signaturesAlphaEqual(const std::vector<TypePtr>& a,
                         const std::vector<TypePtr>& b) -> bool {
    if (a.size() != b.size()) return false;
    std::unordered_map<int, int> aRanks;
    std::unordered_map<int, int> bRanks;
    for (size_t i = 0; i < a.size(); i++)
        if (!typesAlphaEqual(a[i], b[i], aRanks, bRanks)) return false;
    return true;
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
                return head == "List";
            } else if constexpr (std::is_same_v<T, MapType>) {
                return head == "Map";
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
    const std::vector<ImportedFunction>& candidates,
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
    // (1b): a nominal head names a declared record/ADT/distinct type.
    if (winnerHead->nominal &&
        !nominalHeadDeclared(winnerHead->spelling, ifaces))
        return std::nullopt;
    // The call receiver, aliases expanded; gradual receivers stay
    // dynamic (0b).
    const TypePtr expanded =
        expandAlias ? expandAlias(receiverType) : receiverType;
    if (mentionsUnknown(expanded)) return std::nullopt;

    const auto& module = winner.backendModule;
    const auto arity = winner.backendArity;
    const auto& head = winnerHead->spelling;

    // Union members' heads, recursing through nested unions. An
    // unheadable member poisons the whole peer: the emitter split it
    // into per-member entries the consumer cannot enumerate.
    auto unionMemberHeads =
        [](const TypePtr& type, auto&& self, int depth)
        -> std::optional<std::vector<std::string>> {
        if (depth > 8) return std::nullopt;
        const auto* un = type ? std::get_if<UnionType>(&type->kind) : nullptr;
        if (!un) return std::nullopt;
        std::vector<std::string> heads;
        for (const auto& member : un->members) {
            if (member && std::holds_alternative<UnionType>(member->kind)) {
                auto nested = self(member, self, depth + 1);
                if (!nested) return std::nullopt;
                heads.insert(heads.end(), nested->begin(), nested->end());
                continue;
            }
            auto memberHead = writtenTargetHead(member);
            if (!memberHead) return std::nullopt;
            heads.push_back(std::move(memberHead->spelling));
        }
        return heads;
    };

    // (5): the (module, source-arity, head) bucket holds one
    // signature — else the definition is signature-mangled with no
    // `name/Head`. Source arity is the backend arity minus the
    // dictionary slots (one per constrained parameter, the
    // capability context excluded — it never enters the beam
    // arity); grouping by backend arity would split dictionary
    // twins the emitter overloads together. Foulness joins the
    // bucket too: the emitter's key is foul-blind, so a foul twin
    // overloads a pure definition even with identical parameters.
    auto sourceArity = [](const ImportedFunction& fn) {
        int dicts = 0;
        for (size_t i = 1; i < fn.signature.params.size(); i++)
            if (fn.signature.params[i] &&
                std::holds_alternative<ConstrainedType>(
                    fn.signature.params[i]->kind))
                dicts++;
        return fn.backendArity - dicts;
    };
    const int winnerSourceArity = sourceArity(winner);
    for (const auto& peer : candidates) {
        if (!peer.isReceiverMethod || peer.remoteSlot) continue;
        if (peer.backendModule != module) continue;
        if (sourceArity(peer) != winnerSourceArity) continue;
        if (peer.signature.params.empty()) continue;
        const auto& peerReceiver = peer.signature.params[0];
        bool sameBucket = false;
        if (const auto peerHead = writtenTargetHead(peerReceiver))
            sameBucket = peerHead->spelling == head;
        else if (peerReceiver &&
                 std::holds_alternative<UnionType>(peerReceiver->kind)) {
            const auto members = unionMemberHeads(peerReceiver,
                                                  unionMemberHeads, 0);
            sameBucket = members &&
                         std::find(members->begin(), members->end(), head) !=
                             members->end();
        }
        if (!sameBucket) continue;
        if (peer.signature.isFoul != winner.signature.isFoul ||
            !signaturesAlphaEqual(peer.signature.params,
                                  winner.signature.params))
            return std::nullopt;
    }

    // (12): the winner's head is the only head covering this call.
    // Trait peers are excluded (the dispatcher tries them last, so a
    // concrete match wins regardless).
    std::unordered_set<std::string> covering;
    auto peerCovers = [&](const TypePtr& peerReceiver) -> bool {
        if (const auto peerHead = writtenTargetHead(peerReceiver))
            return receiverCoversHead(expanded, peerHead->spelling, ifaces);
        if (!peerReceiver) return false;
        if (std::holds_alternative<ConstrainedType>(peerReceiver->kind))
            return false;
        if (std::holds_alternative<UnionType>(peerReceiver->kind)) {
            const auto members = unionMemberHeads(peerReceiver,
                                                  unionMemberHeads, 0);
            if (!members) return true;
            for (const auto& member : *members)
                if (receiverCoversHead(expanded, member, ifaces))
                    return true;
            return false;
        }
        return true;
    };
    for (const auto& peer : candidates) {
        if (!peer.isReceiverMethod || peer.remoteSlot) continue;
        if (peer.backendModule != module || peer.backendArity != arity)
            continue;
        if (peer.signature.params.empty()) continue;
        const auto& peerReceiver = peer.signature.params[0];
        if (std::holds_alternative<TypeVar>(peerReceiver->kind) ||
            std::holds_alternative<UnknownType>(peerReceiver->kind))
            return std::nullopt;
        // `''` peers (optional-sugar, tuple, func,
        // structural-record targets) bind the bare name, and the
        // duplicate merge concatenates make definitions before the
        // dispatcher — their clauses shadow the concrete ones.
        if (std::holds_alternative<OptionalType>(peerReceiver->kind) ||
            std::holds_alternative<TupleType>(peerReceiver->kind) ||
            std::holds_alternative<FuncType>(peerReceiver->kind) ||
            std::holds_alternative<RecordType>(peerReceiver->kind))
            return std::nullopt;
        if (peerCovers(peerReceiver)) {
            if (const auto peerHead = writtenTargetHead(peerReceiver))
                covering.insert(peerHead->spelling);
            else if (std::holds_alternative<UnionType>(peerReceiver->kind)) {
                const auto members = unionMemberHeads(peerReceiver,
                                                      unionMemberHeads, 0);
                if (!members) return std::nullopt;
                for (const auto& member : *members)
                    if (receiverCoversHead(expanded, member, ifaces))
                        covering.insert(member);
            } else {
                return std::nullopt;
            }
        }
    }
    if (covering.size() != 1 || !covering.count(head)) return std::nullopt;

    // A module function (free-function UFCS entry) with a covering
    // or unprovable receiver shadows through the argument
    // dispatcher, which the duplicate merge runs before the
    // receiver one. Non-covering headed peers cannot match, so
    // they are left alone.
    for (const auto& peer : candidates) {
        if (peer.isReceiverMethod || peer.remoteSlot) continue;
        if (peer.backendModule != module || peer.backendArity != arity)
            continue;
        if (peer.signature.params.empty()) continue;
        const auto peerHead =
            writtenTargetHead(peer.signature.params[0]);
        if (!peerHead) return std::nullopt;
        if (receiverCoversHead(expanded, peerHead->spelling, ifaces))
            return std::nullopt;
    }

    // (DS): Range/Pid keep dispatcher behavior when their shadowers
    // are present — the dispatcher drops their clauses.
    if (head == "Range" || head == "Pid") {
        const std::string& shadower = head == "Range" ? "List" : "Task";
        const auto shadowed = [&](const TypePtr& peerReceiver) {
            if (const auto peerHead = writtenTargetHead(peerReceiver))
                return peerHead->spelling == shadower;
            if (peerReceiver &&
                std::holds_alternative<UnionType>(peerReceiver->kind)) {
                const auto members = unionMemberHeads(peerReceiver,
                                                      unionMemberHeads, 0);
                return members &&
                       std::find(members->begin(), members->end(),
                                 shadower) != members->end();
            }
            return false;
        };
        for (const auto& peer : candidates) {
            if (!peer.isReceiverMethod || peer.remoteSlot) continue;
            if (peer.backendModule != module || peer.backendArity != arity)
                continue;
            if (peer.signature.params.empty()) continue;
            if (shadowed(peer.signature.params[0])) return std::nullopt;
        }
    }

    // (13b): the defining module answers this name for ≥2 distinct
    // heads (all arities — collision is name-wide), else the
    // definition kept the bare name and no `name/Head` exists. Trait
    // peers count by trait name, as the emitter's owners do; `''`
    // members contribute nothing, as they do there.
    std::unordered_set<std::string> owners;
    for (const auto& peer : candidates) {
        if (!peer.isReceiverMethod || peer.remoteSlot) continue;
        if (peer.backendModule != module) continue;
        if (peer.signature.params.empty()) continue;
        const auto& peerReceiver = peer.signature.params[0];
        if (const auto peerHead = writtenTargetHead(peerReceiver)) {
            owners.insert(peerHead->spelling);
            continue;
        }
        if (!peerReceiver) continue;
        if (const auto* constrained =
                std::get_if<ConstrainedType>(&peerReceiver->kind)) {
            if (!constrained->traitName.empty())
                owners.insert(constrained->traitName);
            continue;
        }
        if (std::holds_alternative<UnionType>(peerReceiver->kind)) {
            const auto members = unionMemberHeads(peerReceiver,
                                                  unionMemberHeads, 0);
            if (!members) continue;
            owners.insert(members->begin(), members->end());
        }
    }
    if (owners.size() < 2) return std::nullopt;

    // `name/Head`, spelled the way `mangleReceiverImplementation`
    // (lower_internal.hxx) spells it.
    return winner.sourceName + "/" + head;
}

} // namespace kex::semantic::resolve_core
