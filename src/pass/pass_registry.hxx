#pragma once
// Pipelines from text, after LLVM's PassBuilder: every pass of a level
// registered by name, so a pipeline can be written out
// (`unit(check,lower,module(guards,verify),emit-core)`), parsed back, and
// listed with what each pass does.
#include "adaptor.hxx"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kex::pass {

// One name in a textual pipeline, with the pipeline nested in it, if any.
struct PipelineElement {
    std::string name;
    std::vector<PipelineElement> inner;
};

struct ParsedPipeline {
    std::vector<PipelineElement> elements;
    // Empty when the text parsed.
    std::string error;
};

// `a,b(c,d),e`: names separated by commas, a nested pipeline in parentheses.
auto parsePipelineText(std::string_view text) -> ParsedPipeline;

template <typename IRUnit>
class PassRegistry {
public:
    using Factory = std::function<std::unique_ptr<PassConcept<IRUnit>>()>;
    // Builds a pass from the pipeline nested in its name, or says why not.
    using NestedFactory = std::function<std::unique_ptr<PassConcept<IRUnit>>(
        const std::vector<PipelineElement>& inner, std::string& error)>;

    template <typename Pass>
    auto registerPass(std::string_view description, std::function<Pass()> make) -> void {
        m_passes.insert_or_assign(std::string(Pass::name()),
                                  Entry{std::string(description), [make = std::move(make)] {
                                            return std::unique_ptr<PassConcept<IRUnit>>(
                                                std::make_unique<PassModel<IRUnit, Pass>>(make()));
                                        }});
    }
    template <typename Pass>
    auto registerPass(std::string_view description) -> void {
        registerPass<Pass>(description, [] { return Pass{}; });
    }
    // A pass that says what it does itself (`kDescription`).
    template <typename Pass>
    auto registerPass() -> void {
        registerPass<Pass>(Pass::kDescription);
    }

    // A level nested in this one: `module(...)` inside a unit pipeline.
    auto registerNested(std::string name, std::string description, NestedFactory make) -> void {
        m_nested.insert_or_assign(std::move(name), NestedEntry{std::move(description), std::move(make)});
    }

    // Adds `elements` to `pipeline`; the first problem, or nullopt.
    auto build(PassManager<IRUnit>& pipeline, const std::vector<PipelineElement>& elements) const
        -> std::optional<std::string> {
        for (const auto& element : elements) {
            if (element.inner.empty()) {
                const auto found = m_passes.find(element.name);
                if (found == m_passes.end())
                    return "unknown " + std::string(IRUnitLevel<IRUnit>::kName) + " pass '" +
                           element.name + "'";
                pipeline.addPass(found->second.make());
                continue;
            }
            const auto nested = m_nested.find(element.name);
            if (nested == m_nested.end())
                return "'" + element.name + "' takes no pipeline at the " +
                       std::string(IRUnitLevel<IRUnit>::kName) + " level";
            std::string error;
            auto pass = nested->second.make(element.inner, error);
            if (!pass) return error;
            pipeline.addPass(std::move(pass));
        }
        return std::nullopt;
    }
    auto build(PassManager<IRUnit>& pipeline, std::string_view text) const
        -> std::optional<std::string> {
        const auto parsed = parsePipelineText(text);
        if (!parsed.error.empty()) return parsed.error;
        return build(pipeline, parsed.elements);
    }

    // Every pass this level knows, with what it does.
    auto print(std::ostream& out, int indent = 2) const -> void {
        const std::string pad(static_cast<std::size_t>(indent), ' ');
        for (const auto& [name, entry] : m_passes) {
            out << pad;
            out.width(22);
            out << std::left << name << entry.description << "\n";
        }
        for (const auto& [name, entry] : m_nested) {
            out << pad;
            out.width(22);
            out << std::left << (name + "(...)") << entry.description << "\n";
        }
    }

private:
    struct Entry {
        std::string description;
        Factory make;
    };
    struct NestedEntry {
        std::string description;
        NestedFactory make;
    };
    std::map<std::string, Entry> m_passes;
    std::map<std::string, NestedEntry> m_nested;
};

// Lets `outer` pipelines nest `inner`'s: `module(guards,verify)` in a unit
// pipeline builds an InnerPassAdaptor over the passes `inner` registers.
template <typename Outer, typename Inner>
auto registerNestedLevel(PassRegistry<Outer>& outer, const PassRegistry<Inner>& inner,
                         std::string description) -> void {
    outer.registerNested(
        std::string(IRUnitLevel<Inner>::kName), std::move(description),
        [&inner](const std::vector<PipelineElement>& elements, std::string& error)
            -> std::unique_ptr<PassConcept<Outer>> {
            PassManager<Inner> pipeline;
            if (auto problem = inner.build(pipeline, elements)) {
                error = std::move(*problem);
                return nullptr;
            }
            return std::make_unique<PassModel<Outer, InnerPassAdaptor<Outer, Inner>>>(
                InnerPassAdaptor<Outer, Inner>(std::move(pipeline)));
        });
}

} // namespace kex::pass
