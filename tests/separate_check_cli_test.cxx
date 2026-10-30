// The separate check (KEX_SEPARATE=1) checks each unit of a program against
// the interfaces of the units below it, and must agree with the check of the
// whole program. Each module here is shaped like one whose interface once
// left something out: constructors called through their module, defaulted
// parameters, named block arguments, structural type aliases, `serving`
// slots and `within:` on them, a module in two parts, a capability nested in a module, a constant,
// a nested module compiled on its own, and a module function another
// module's `make` function shares a name with, or one that takes no
// receiver its signature line does not name.
//
// With KEX_SEPARATE_CORE each unit that checked cleanly is also lowered on
// its own; compiled and run, it must print what the merged build prints.
#include "test.hxx"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>

using namespace test;

namespace {

struct Package {
    std::filesystem::path root;

    Package() {
        char path[] = "/tmp/kex_separate_check_XXXXXX";
        root = mkdtemp(path);
        write("lib/shapes.kex", R"KEX(module Shapes

type Shape = Dot | Circle(Integer)

area : Shape -> Integer
let area(Dot) = 0
let area(Circle(r)) = r * r
)KEX");
        write("lib/columns.kex", R"KEX(module Columns

record Table do
  name : String
end

record Column do
  label : String
end

make Table do
  let text(label: Atom, primaryKey: Bool = false, nullable: Bool = false, doc: String = "") -> Column = Column { label: "${label}" }
end

define : Block<Table, Column> -> String
let define(column) = column(Table { name: "users" }).label

change : Atom -> Block<Table, Column> -> Block<Table, Column> -> String
let change(name: Atom, from: Block<Table, Column>, to: Block<Table, Column>) -> String do
  let table = Table { name: "${name}" }
  "${from(table).label} ${to(table).label}"
end
)KEX");
        write("lib/web/html.kex", R"KEX(module Web.Html do
  record Node do
    words : String
  end

  text : String -> Node
  let text(words) = Node { words: words }
end
)KEX");
        write("lib/web/plugs.kex", R"KEX(module Web.Plugs

record Request do
  path : String
end

type Step = Request -> Request
type Pair = (Integer, Bool)

step : Step -> Step
let step(body) = body
)KEX");
        write("lib/tally.kex", R"KEX(module Tally

record Counter do
  count : Integer = 0
end

serving Counter do
  get ::> Reply<Integer>
  slot get -> Reply<Integer> = Kex.Intrinsic.Process.reply(@count)
end
)KEX");
        write("lib/store.kex", R"KEX(capability Store do
  foul saved(name: String) -> Bool = true
end

module Store do
  record Blob do
    name : String
  end

  picture? : String -> Bool
  let picture?(kind) = kind.startsWith?("image/")
end
)KEX");
        write("lib/tickers.kex", R"KEX(module Tickers

capability Ticker do
  foul now() -> Integer do
    return 1000
  end
end

record FrozenTicker do
  at : Integer
end

make FrozenTicker, implement: Ticker do
  foul now() -> Integer = @at
end

foul stamp() -> String = "at ${Ticker.now()}"
)KEX");
        write("lib/web/route.kex", R"KEX(module Web.Route

get : String -> Integer -> String
let get(path, times) = "${path} ${times}"
)KEX");
        write("lib/statuses.kex", R"KEX(module Statuses

STATUSES : [(String, String)]
let STATUSES = [("a", "away"), ("b", "busy")]
)KEX");
        write("lib/outer.kex", R"KEX(module Outer

shown : String
let shown() = Outer.Inner.named()
)KEX");
        write("lib/outer/inner.kex", R"KEX(module Outer.Inner

named : String
let named() = "inner"
)KEX");
        write("lib/notes.kex", R"KEX(capability Notes do
  foul take(key: String) -> String do
    return Notes.local().take(key)
  end
end

module Notes do
  record Local do
    prefix : String
  end

  make Local, implement: Notes do
    foul take(key: String) -> String = "${@prefix}${key}"
  end

  local : Local
  foul local = Local { prefix: "local:" }

  noted : String -> String
  foul noted(key) = "noted ${Notes.take(key)}"
end
)KEX");
        write("lib/marks.kex", R"KEX(module Marks do
  record Mark do
    text : String
  end

  exclaimed : String -> String
  let exclaimed(text) = "${text}!"
end

make Marks.Mark do
  let loud -> String = @text.exclaimed
end
)KEX");
    }

    ~Package() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    auto write(const std::string& path, const std::string& contents) -> void {
        std::filesystem::create_directories((root / path).parent_path());
        std::ofstream file(root / path);
        file << contents;
    }

    // `kex --check` of app/main.kex with the separate check beside the
    // merged one; its output, both checks' diagnostics.
    auto check() -> std::string {
        const auto output = root / "output";
        const auto command = "KEX_SEPARATE=1 KEX_CACHE=" + (root / "cache").string() +
            " " + std::string(KEX_BINARY_PATH) + " --no-colors --check --source-root " +
            (root / "app").string() + " --source-root " + (root / "lib").string() + " " +
            (root / "app" / "main.kex").string() + " > " + output.string() + " 2>&1";
        (void)std::system(command.c_str());
        return read(output);
    }

    // The program built as one and as units lowered apart, each compiled and
    // run: what each printed, merged first.
    auto run() -> std::pair<std::string, std::string> {
        const auto roots = " --source-root " + (root / "app").string() +
            " --source-root " + (root / "lib").string() + " ";
        const auto build = "KEX_SEPARATE=1 KEX_SEPARATE_CORE=" + (root / "separate").string() +
            " KEX_CACHE=" + (root / "cache").string() + " " + std::string(KEX_BINARY_PATH) +
            " --no-colors -E -o " + (root / "merged").string() + roots +
            (root / "app" / "main.kex").string() + " > " + (root / "build.out").string() +
            " 2>&1";
        (void)std::system(build.c_str());
        const auto runAs = [&](const std::string& kind) {
            const auto cores = root / kind;
            const auto beams = root / (kind + "-beam");
            std::filesystem::create_directories(beams);
            const auto output = root / (kind + ".out");
            const auto command = "erlc -o " + beams.string() + " " + cores.string() +
                "/*.core > " + output.string() + " 2>&1 && " + std::string(KEX_BINARY_PATH) +
                " --no-colors --run " + (beams / "kex_main.beam").string() + " >> " +
                output.string() + " 2>&1";
            (void)std::system(command.c_str());
            return read(output);
        };
        return {runAs("merged"), runAs("separate")};
    }

    static auto read(const std::filesystem::path& path) -> std::string {
        std::ifstream file(path);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
};

const auto program = R"KEX(using Shapes
using Columns, only: [define, change]
using Web.Plugs, only: [Request, Step, Pair, step]
using Web.Html, as: Html
using Tally, only: [Counter]
using Store, only: [Blob]
using Tickers
using Statuses, only: [STATUSES]
using Outer
using Web.Route, only: [get]
using Notes
using Marks

foul read(server: Server<Counter>) -> Result<Integer, CallError> = server.get()
foul readSoon(server: Server<Counter>) -> Result<Integer, CallError> = server.get(within: 100)

let shapes() -> [(String, Shapes.Shape)] = [("ring", Shapes.Circle(2)), ("dot", Shapes.Dot)]

let passed() -> Step = step do |request|
  request
end

let flagged() -> Pair = (1, true)

main do
  IO.printLine(Shapes.area(Shapes.Circle(3)))
  IO.printLine(define do |t| t.text(:id, nullable: true) end)
  IO.printLine(change(:users, from: { |t| t.text(:a) }, to: { |t| t.text(:b, doc: "b") }))
  IO.printLine(Store.picture?("image/png"))
  IO.printLine(Blob { name: "blob" }.name)
  with Tickers.Ticker = FrozenTicker { at: 7 } do
    IO.printLine(stamp())
  end
  IO.printLine(STATUSES.map { |glyph, words| "${glyph} ${words}" })
  IO.printLine(Outer.shown())
  IO.printLine(Html.text("x").words)
  IO.printLine(passed()(Request { path: "/" }).path)
  IO.printLine(flagged())
  IO.printLine(shapes().length)
  IO.printLine({"a": 1}.get("a"))
  IO.printLine(Web.Route.get("ab", 2))
  IO.printLine(Notes.take("a"))
  with Notes = Notes.Local { prefix: "with:" } do
    IO.printLine(Notes.take("b"))
    IO.printLine(Notes.noted("c"))
  end
  IO.printLine(Mark { text: "m" }.loud)
  IO.printLine("hey".exclaimed)
end
)KEX";

// Whether a line of `text` reports `message`, from the separate check (its
// lines start "separate: ") or from the merged one.
auto reports(const std::string& text, const std::string& message, bool separate) -> bool {
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);)
        if (line.find(message) != std::string::npos &&
            line.starts_with("separate: ") == separate)
            return true;
    return false;
}

} // namespace

int main() {
    describe("the separate check", []() {
        it("agrees with the merged check where it finds nothing", []() {
            Package package;
            package.write("app/main.kex", program);
            const auto text = package.check();
            assertTrue(text.find("units checked, 0 errors") != std::string::npos,
                       "the separate check found errors: " + text);
            assertTrue(text.find(": error: ") == std::string::npos,
                       "a check reported an error: " + text);
        });
        it("finds what the merged check finds", []() {
            Package package;
            package.write("app/main.kex", std::string(program) + R"KEX(
let wrong() -> String = Shapes.Dot

let skipped() -> String = define do |t| t.text(:id, doc: 1) end
)KEX");
            const auto text = package.check();
            for (const std::string expected : {"`wrong` declared to return String but body returns Dot",
                                               "`text` expects argument 5 to be String, but got Integer"}) {
                assertTrue(reports(text, expected, false),
                           "the merged check missed \"" + expected + "\": " + text);
                assertTrue(reports(text, expected, true),
                           "the separate check missed \"" + expected + "\": " + text);
            }
        });
    });
    describe("lowering unit by unit", []() {
        it("runs as the merged build runs", []() {
            Package package;
            package.write("app/main.kex", program);
            const auto [merged, separate] = package.run();
            assertTrue(merged.find("error") == std::string::npos && !merged.empty(),
                       "the merged build did not run: " + merged);
            assertEqual(separate, merged);
        });
    });
    return runAll();
}
