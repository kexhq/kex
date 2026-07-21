// Standalone dumper: load the prebuilt kex_prelude.beam and print every
// signature the compiled KexI exposes (module exports + receiver functions),
// so we can audit them against src/semantic/stdlib_signatures.cxx.
//
// Build:
//   clang++ -std=c++20 -I src tools/dump_prelude_kexi.cxx \
//     -L build -lkex_lib -o build/dump_prelude_kexi
// (linking however kex_lib is linked; see CMakeLists.txt)

#include "beam/kexi_registry.hxx"
#include "semantic/imported_interfaces.hxx"
#include "semantic/types.hxx"
#include <iostream>

using namespace kex;

static auto typeStr(const semantic::TypePtr& t) -> std::string {
    return semantic::typeToString(t);
}

static auto sigStr(const semantic::Signature& s) -> std::string {
    std::string out = s.name + "(";
    for (size_t i = 0; i < s.params.size(); ++i) {
        if (i) out += ", ";
        out += typeStr(s.params[i]);
    }
    out += ") -> " + typeStr(s.result);
    return out;
}

int main() {
    beam::KexiRegistry registry;
    auto errors = registry.loadUnit("build/runtime/beam/kex_prelude.beam");
    if (!errors.empty()) {
        std::cerr << "Failed to load prelude:\n";
        for (const auto& e : errors) std::cerr << "  " << e.message << "\n";
        return 1;
    }

    auto interfaces = registry.buildSemanticInterfaces();

    std::cout << "== Module exports ==\n";
    for (const auto& [name, mod] : interfaces.modules) {
        std::cout << (mod.isFoul ? "foul " : "") << "module " << name
                  << " (backend=" << mod.backendModule
                  << ", auto=" << mod.automaticImport << ")\n";
        for (const auto& [fn, list] : mod.exports)
            for (const auto& f : list)
                std::cout << "  " << sigStr(f.signature)
                          << (f.signature.isFoul ? " [foul]" : "") << "\n";
    }

    std::cout << "\n== Receiver functions ==\n";
    for (const auto& [name, list] : interfaces.receiverFunctions) {
        std::cout << "receiver " << name << "\n";
        for (const auto& f : list)
            std::cout << "  " << sigStr(f.signature)
                      << "  [backend=" << f.backendModule
                      << "::" << f.backendFunction << "/" << f.backendArity
                      << (f.signature.isFoul ? " foul" : "")
                      << "]\n";
    }

    std::cout << "\n== Trait definitions ==\n";
    for (const auto& t : interfaces.traits) {
        std::cout << "trait " << t.name << "\n";
        for (const auto& m : t.requiredMethods)
            std::cout << "  " << sigStr(m)
                      << (m.isFoul ? " [foul]" : "") << "\n";
    }

    std::cout << "\n== Trait conformances ==\n";
    for (const auto& c : interfaces.traitConformances)
        std::cout << "  " << c.typeName << " : " << c.traitName << "\n";

    std::cout << "\n== ADTs ==\n";
    for (const auto& a : interfaces.adts) {
        std::cout << "type " << a.name << " =";
        for (const auto& c : a.constructors) std::cout << " " << c;
        std::cout << "\n";
    }

    return 0;
}
