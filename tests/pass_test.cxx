// The pass infrastructure (src/pass/), on a toy two-level IR: a Book of
// Pages, as LLVM's are a Module of Functions.
#include "test.hxx"
#include "../src/pass/adaptor.hxx"
#include "../src/pass/pass_registry.hxx"
#include "../src/pass/standard_instrumentation.hxx"

#include <sstream>
#include <string>
#include <vector>

namespace toy {

struct Page {
    std::string name;
    std::vector<int> words;
    bool answered = false;
};
struct Book {
    std::string title;
    std::vector<Page> pages;
};

auto unitName(const Page& page) -> std::string { return page.name; }
auto isAnswered(const Page& page) -> bool { return page.answered; }
auto printUnit(std::ostream& out, const Page& page) -> void {
    for (const auto word : page.words) out << word << " ";
}
auto unitName(const Book& book) -> std::string { return book.title; }
auto printUnit(std::ostream& out, const Book& book) -> void { out << book.pages.size() << " pages"; }

} // namespace toy

template <>
struct kex::pass::IRUnitLevel<toy::Page> {
    static constexpr std::string_view kName = "page";
};
template <>
struct kex::pass::IRUnitLevel<toy::Book> {
    static constexpr std::string_view kName = "book";
};
template <>
struct kex::pass::IRUnitChildren<toy::Book, toy::Page> {
    template <typename Visit>
    static auto forEach(toy::Book& book, Visit&& visit) -> void {
        for (auto& page : book.pages) visit(page);
    }
};

namespace toy {

using namespace kex::pass;

int sumRuns = 0;
int doubledRuns = 0;

struct SumAnalysis : AnalysisInfoMixin<SumAnalysis> {
    static constexpr std::string_view kName = "sum";
    using Result = int;
    auto run(Page& page, AnalysisManager<Page>&) -> Result {
        ++sumRuns;
        int sum = 0;
        for (const auto word : page.words) sum += word;
        return sum;
    }
};

// Computed from SumAnalysis: dropping that drops this.
struct DoubledSumAnalysis : AnalysisInfoMixin<DoubledSumAnalysis> {
    static constexpr std::string_view kName = "doubled-sum";
    using Result = int;
    using Dependencies = std::tuple<SumAnalysis>;
    auto run(Page& page, AnalysisManager<Page>& analyses) -> Result {
        ++doubledRuns;
        return 2 * analyses.result<SumAnalysis>(page);
    }
};

struct PageCountAnalysis : AnalysisInfoMixin<PageCountAnalysis> {
    static constexpr std::string_view kName = "page-count";
    using Result = std::size_t;
    auto run(Book& book, AnalysisManager<Book>&) -> Result { return book.pages.size(); }
};

struct AppendPass : PassInfoMixin<AppendPass> {
    static constexpr std::string_view kName = "append";
    int word = 1;
    auto run(Page& page, AnalysisManager<Page>&) -> PreservedAnalyses {
        page.words.push_back(word);
        return PreservedAnalyses::none();
    }
};

struct ReadSumPass : PassInfoMixin<ReadSumPass> {
    static constexpr std::string_view kName = "read-sum";
    std::vector<int>* seen;
    auto run(Page& page, AnalysisManager<Page>& analyses) -> PreservedAnalyses {
        seen->push_back(analyses.result<DoubledSumAnalysis>(page));
        return PreservedAnalyses::all();
    }
};

// Reads the book's page count through the outer proxy.
struct ReadOuterPass : PassInfoMixin<ReadOuterPass> {
    static constexpr std::string_view kName = "read-outer";
    const Book* book;
    std::vector<std::size_t>* seen;
    auto run(Page& page, AnalysisManager<Page>& analyses) -> PreservedAnalyses {
        const auto& outer = analyses.result<OuterAnalysisManagerProxy<Book, Page>>(page);
        const auto* count = outer.cachedResult<PageCountAnalysis>(*book);
        seen->push_back(count ? *count : 0);
        return PreservedAnalyses::all();
    }
};

struct RequiredPass : PassInfoMixin<RequiredPass> {
    static constexpr std::string_view kName = "required";
    static constexpr bool kRequired = true;
    int* runs;
    auto run(Page&, AnalysisManager<Page>&) -> PreservedAnalyses {
        ++*runs;
        return PreservedAnalyses::all();
    }
};

} // namespace toy

int main() {
    using namespace test;
    using namespace kex::pass;
    using namespace toy;

    describe("AnalysisManager", []() {
        it("computes a result once and keeps it while it is preserved", []() {
            AnalysisManager<Page> analyses;
            analyses.registerAnalysis<SumAnalysis>();
            Page page{"p", {1, 2, 3}};
            sumRuns = 0;
            assertEqual(analyses.result<SumAnalysis>(page), 6);
            assertEqual(analyses.result<SumAnalysis>(page), 6);
            assertEqual(sumRuns, 1);
            analyses.invalidate(page, PreservedAnalyses::all());
            assertTrue(analyses.cachedResult<SumAnalysis>(page) != nullptr);
            analyses.invalidate(page, PreservedAnalyses::none().preserve<SumAnalysis>());
            assertTrue(analyses.cachedResult<SumAnalysis>(page) != nullptr);
            analyses.invalidate(page, PreservedAnalyses::none());
            assertTrue(analyses.cachedResult<SumAnalysis>(page) == nullptr);
        });
        it("drops a result computed from one it drops", []() {
            AnalysisManager<Page> analyses;
            analyses.registerAnalysis<SumAnalysis>();
            analyses.registerAnalysis<DoubledSumAnalysis>();
            Page page{"p", {5}};
            assertEqual(analyses.result<DoubledSumAnalysis>(page), 10);
            // Keeps the doubled sum, not the sum it was computed from.
            analyses.invalidate(page, PreservedAnalyses::none().preserve<DoubledSumAnalysis>());
            assertTrue(analyses.cachedResult<SumAnalysis>(page) == nullptr);
            assertTrue(analyses.cachedResult<DoubledSumAnalysis>(page) == nullptr);
        });
        it("keeps each unit's results apart", []() {
            AnalysisManager<Page> analyses;
            analyses.registerAnalysis<SumAnalysis>();
            Page first{"a", {1}};
            Page second{"b", {2}};
            assertEqual(analyses.result<SumAnalysis>(first), 1);
            assertEqual(analyses.result<SumAnalysis>(second), 2);
            analyses.clear(first);
            assertTrue(analyses.cachedResult<SumAnalysis>(first) == nullptr);
            assertTrue(analyses.cachedResult<SumAnalysis>(second) != nullptr);
        });
        it("refuses an analysis nobody registered", []() {
            AnalysisManager<Page> analyses;
            Page page{"p", {}};
            bool threw = false;
            try {
                analyses.result<SumAnalysis>(page);
            } catch (const std::logic_error&) {
                threw = true;
            }
            assertTrue(threw);
        });
    });

    describe("PassManager", []() {
        it("runs passes in order and invalidates what each did not preserve", []() {
            AnalysisManager<Page> analyses;
            analyses.registerAnalysis<SumAnalysis>();
            analyses.registerAnalysis<DoubledSumAnalysis>();
            std::vector<int> seen;
            PassManager<Page> pipeline;
            pipeline.addPass(ReadSumPass{{}, &seen})
                .addPass(AppendPass{{}, 4})
                .addPass(ReadSumPass{{}, &seen});
            Page page{"p", {1}};
            const auto preserved = pipeline.run(page, analyses);
            assertTrue(seen == std::vector<int>{2, 10});
            assertFalse(preserved.allPreserved());
        });
        it("prints itself as a textual pipeline", []() {
            PassManager<Page> pipeline;
            pipeline.addPass(AppendPass{}).addPass(AppendPass{});
            std::ostringstream out;
            pipeline.printPipeline(out);
            assertEqual(out.str(), std::string("append,append"));
        });
    });

    describe("InnerPassAdaptor", []() {
        it("runs an inner pipeline over each child, reading the outer level", []() {
            PassInstrumentationCallbacks callbacks;
            AnalysisManager<Book> books(&callbacks);
            AnalysisManager<Page> pages(&callbacks);
            crossRegisterProxies(books, pages);
            books.registerAnalysis<PageCountAnalysis>();
            Book book{"b", {{"one", {1}}, {"two", {2}}}};
            books.result<PageCountAnalysis>(book);
            std::vector<std::size_t> seen;
            PassManager<Page> inner;
            inner.addPass(AppendPass{{}, 9}).addPass(ReadOuterPass{{}, &book, &seen});
            PassManager<Book> outer;
            outer.addPass(InnerPassAdaptor<Book, Page>(std::move(inner)));
            const auto preserved = outer.run(book, books);
            assertTrue(book.pages[0].words == std::vector<int>{1, 9});
            assertTrue(book.pages[1].words == std::vector<int>{2, 9});
            assertTrue(seen == std::vector<std::size_t>{2, 2});
            assertFalse(preserved.allPreserved());
            std::ostringstream out;
            outer.printPipeline(out);
            assertEqual(out.str(), std::string("page(append,read-outer)"));
        });
    });

    describe("PassInstrumentation", []() {
        it("skips a pass shouldRun refuses, unless it is required", []() {
            PassInstrumentationCallbacks callbacks;
            callbacks.registerShouldRun([](std::string_view, const UnitRef& unit) {
                return unit.name != "cached";
            });
            AnalysisManager<Page> analyses(&callbacks);
            int requiredRuns = 0;
            PassManager<Page> pipeline;
            pipeline.addPass(AppendPass{{}, 7}).addPass(RequiredPass{{}, &requiredRuns});
            Page cached{"cached", {}};
            Page fresh{"fresh", {}};
            pipeline.run(cached, analyses);
            pipeline.run(fresh, analyses);
            assertTrue(cached.words == std::vector<int>{});
            assertTrue(fresh.words == std::vector<int>{7});
            assertEqual(requiredRuns, 2);
        });
        it("runs only the required passes on a unit a cache answered", []() {
            AnalysisManager<Page> analyses;
            int requiredRuns = 0;
            PassManager<Page> pipeline;
            pipeline.addPass(AppendPass{{}, 7}).addPass(RequiredPass{{}, &requiredRuns});
            Page answered{"answered", {}, true};
            pipeline.run(answered, analyses);
            assertTrue(answered.words.empty());
            assertEqual(requiredRuns, 1);
        });
        it("times each pass by level and prints after the named one", []() {
            PassInstrumentationCallbacks callbacks;
            TimePasses timer;
            timer.registerCallbacks(callbacks);
            std::ostringstream printed;
            PrintAfter print(printed, {"append"}, false);
            print.registerCallbacks(callbacks);
            AnalysisManager<Page> analyses(&callbacks);
            PassManager<Page> pipeline;
            pipeline.addPass(AppendPass{{}, 3});
            Page page{"p", {}};
            pipeline.run(page, analyses);
            assertTrue(printed.str().find("page p after append") != std::string::npos,
                       printed.str());
            assertTrue(printed.str().find("3 ") != std::string::npos, printed.str());
            std::ostringstream report;
            timer.report(report);
            assertTrue(report.str().find("page append") != std::string::npos, report.str());
        });
    });

    describe("PassRegistry", []() {
        it("parses nested textual pipelines", []() {
            const auto parsed = parsePipelineText("a, b(c,d(e)) ,f");
            assertEqual(parsed.error, std::string());
            assertEqual(parsed.elements.size(), std::size_t{3});
            assertEqual(parsed.elements[1].name, std::string("b"));
            assertEqual(parsed.elements[1].inner[1].inner[0].name, std::string("e"));
        });
        it("says what is wrong with a malformed pipeline", []() {
            assertTrue(!parsePipelineText("a(b").error.empty());
            assertTrue(!parsePipelineText("a,").error.empty());
            assertTrue(!parsePipelineText("a()").error.empty());
            assertTrue(!parsePipelineText("a)").error.empty());
        });
        it("builds a nested pipeline from text, and refuses unknown names", []() {
            PassRegistry<Page> pageRegistry;
            pageRegistry.registerPass<AppendPass>("append a 1");
            PassRegistry<Book> bookRegistry;
            registerNestedLevel(bookRegistry, pageRegistry, "a page pipeline over each page");
            PassManager<Book> pipeline;
            assertTrue(!bookRegistry.build(pipeline, "page(append,append)"));
            std::ostringstream out;
            pipeline.printPipeline(out);
            assertEqual(out.str(), std::string("page(append,append)"));
            PassManager<Book> bad;
            const auto unknown = bookRegistry.build(bad, "page(nope)");
            assertTrue(unknown && unknown->find("unknown page pass 'nope'") != std::string::npos);
            const auto flat = bookRegistry.build(bad, "append");
            assertTrue(flat && flat->find("unknown book pass") != std::string::npos);
        });
    });

    return runAll();
}
