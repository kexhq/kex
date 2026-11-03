#include "resolve_core.hxx"
#include <algorithm>

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

} // namespace kex::semantic::resolve_core
