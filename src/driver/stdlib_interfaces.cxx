#include "stdlib_interfaces.hxx"
#include "build_ir.hxx"
#include "../common/prelude_interfaces.hxx"
#include "../common/prelude_loader.hxx"
#include "../lexer/lexer.hxx"
#include "../module/discovery.hxx"
#include "../module/package_metadata.hxx"
#include "../parser/parser.hxx"
#include "../pass/options.hxx"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace kex::driver {

namespace {

// The names that bring in an opt-in stdlib file: the one its path gives it,
// when the file declares that module (`Data.Set` for data/set.kex, whose
// `module Data` nests `module Set`), as the resolver finds a module by its
// path; otherwise each module it declares at its top level (`Integer` in
// number.kex). `using Data` found no data.kex, and the three files of `Data`
// were never built.
auto modulesNamingFile(const std::string& path, const std::string& root)
    -> std::vector<std::string> {
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Lexer lexer(std::move(source), path);
    Parser parser(lexer.tokenizeAll(), path);
    auto program = parser.parseProgram();
    const auto declared = module::declaredModuleNames(program);
    const auto byPath =
        module::moduleNameForPath(std::filesystem::path(path).lexically_relative(root));
    if (declared.contains(byPath)) {
        return {byPath};
    }
    std::vector<std::string> names;
    for (const auto& item : program.items) {
        if (const auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
            module && *module) {
            names.push_back((*module)->name);
        }
    }
    return names;
}

} // namespace

auto checkedStdlibInterfaces(const semantic::ImportedInterfaces& prelude)
    -> std::optional<semantic::ImportedInterfaces> {
    // One program that brings in every opt-in module: each is a unit of its
    // build, checked against the prelude and the units below it, and
    // described from that check, as a program that uses it gets it.
    std::set<std::string> modules;
    const auto roots = standardLibraryModuleRoots();
    for (const auto& file : standardLibrarySourceFiles()) {
        for (const auto& root : roots) {
            if (const auto relative = std::filesystem::path(file).lexically_relative(root);
                !relative.empty() && *relative.begin() != "..") {
                for (auto& name : modulesNamingFile(file, root)) {
                    modules.insert(std::move(name));
                }
                break;
            }
        }
    }
    if (modules.empty()) {
        return std::nullopt;
    }
    std::string entrySource;
    for (const auto& name : modules) {
        entrySource += "using " + name + "\n";
    }
    entrySource += "\nmain do\nend\n";
    const std::string entryPath = "<stdlib interfaces>";
    Lexer lexer(entrySource, entryPath);
    Parser parser(lexer.tokenizeAll(), entryPath);
    auto entry = parser.parseProgram();

    build::Program program(
        entry, {entryPath, standardLibraryModuleRoots(), &prelude,
                /*checkGates=*/false});
    try {
        auto pipeline = build::buildProgramPipeline();
        auto callbacks = pass::standardCallbacks();
        if (!build::runBuild(program, pipeline, callbacks)) {
            std::fprintf(
                stderr, "warning: the standard library's interfaces were read from its "
                        "source: its check did not run\n");
            for (const auto& diagnostic : program.diagnostics()) {
                std::fprintf(
                    stderr, "  %s:%d:%d: %s\n", std::string(diagnostic.location.file).c_str(),
                    diagnostic.location.line, diagnostic.location.column,
                    diagnostic.message.c_str());
            }
            return std::nullopt;
        }
    } catch (const std::exception& problem) {
        std::fprintf(
            stderr,
            "warning: the standard library's interfaces were read from its "
            "source: %s\n",
            problem.what());
        return std::nullopt;
    }
    // The described units as the units above them see them (BelowAnalysis):
    // registered together, and declared one package.
    beam::KexiRegistry registry;
    module::PackageMetadata package;
    package.id = "kex.stdlib";
    for (const auto& described : program.described) {
        if (described.unit.modules.empty()) {
            continue;
        }
        package.unitIds.push_back(described.unit.modules.front().chunk.metadata.unitId);
        for (const auto& name : described.names) {
            if (std::find(
                    package.receiverProviders.begin(), package.receiverProviders.end(), name) ==
                package.receiverProviders.end()) {
                package.receiverProviders.push_back(name);
            }
        }
        registry.registerUnit(described.unit);
    }
    if (package.unitIds.empty()) {
        std::fprintf(
            stderr, "warning: the standard library's interfaces were read from its "
                    "source: no unit was described\n");
        return std::nullopt;
    }
    for (const auto& error : registry.declarePackage(package)) {
        std::fprintf(stderr, "warning: stdlib interfaces: %s\n", error.message.c_str());
    }
    // The prelude with these over it, as a program's units above them see
    // them (AvailableInterfacesAnalysis): where the prebuilt runtime carries
    // its own copy of an opt-in module, the checked one replaces it.
    // Each module compiled apart, as it is when a program imports it (a
    // `using` brings its functions in bare); its `make` functions travel with
    // their receivers only once a program reaches its unit
    // (markReachedUnits): listed for every program, `Data.Set`'s `add` and
    // WebSocket's `send` were candidates where nothing imported them.
    auto described = registry.buildSemanticInterfaces();
    for (auto& [_, module] : described.modules) {
        module.compiledApart = true;
    }
    return build::overlayDescribedInterfaces(&prelude, std::move(described), /*reached=*/false);
}

auto registerCheckedStdlibInterfaces() -> void {
    kex::checkedStdlibInterfacesHook() = [](const semantic::ImportedInterfaces& prelude) {
        return checkedStdlibInterfaces(prelude);
    };
}

} // namespace kex::driver
