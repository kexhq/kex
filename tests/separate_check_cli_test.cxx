// The build checks and lowers each unit of a program against the interfaces
// of the units below it, and must find what the program's code says and run
// what it means. Each module here is shaped like one whose interface once
// left something out: constructors called through their module, defaulted
// parameters, named block arguments, structural type aliases, `serving`
// slots and `within:` on them, a module in two parts, a capability nested in
// a module, a constant, a nested module compiled on its own, and a module
// function another module's `make` function shares a name with, or one that
// takes no receiver its signature line does not name.
//
// Built, compiled and run, the program must print what each unit's code
// means, and the Core kept with KEX_SEPARATE_CORE must run the same.
// For that the program also has a capability beside a module of its name,
// whose default and replacement and helper each run, a slot called with
// `within:` on another unit's server, a `make` block outside its module,
// a trait default a type of another unit inherits,
// a function of another unit called by UFCS, an operator one unit
// overloads for two of its types, called on each from another unit, and a
// method only one type of another unit owns, called on it from this one,
// and a `make` naming a nested module's record from its module.
#include "test.hxx"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
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
type Wrap = Step -> Step
type Reached = String | Web.Plugs.Request
type Reach = Request -> Reached
type Relay = Reach -> Reach

step : Step -> Step
let step(body) = body

around : Wrap -> Step
let around(wrap) = wrap(do |request| request end)

relayed : Relay -> String
let relayed(relay) = match relay(do |request| request end)(Request { path: "/" }) do
  Request { path } => path
  text => text
end
)KEX");
        write("lib/tally.kex", R"KEX(module Tally

record Counter do
  count : Integer = 0
end

serving Counter do
  get ::> Reply<Integer>
  slot get -> Reply<Integer> = Kex.Intrinsic.Process.reply(@count)
  reset :> Void
  slot reset -> Void = Kex.Intrinsic.Process.cast
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
routeSummary : String -> String
let routeSummary(path) = "route ${path}"
)KEX");
        write("lib/uses_route.kex", R"KEX(module UsesRoute

using Web.Route, only: [routeSummary]

let summarized() = routeSummary("inside")
)KEX");
        write("lib/kinds.kex", R"KEX(module Kinds

type Kind = Red | Blue
)KEX");
        write("lib/one/widget.kex", R"KEX(module One.Widget

type OneKind = First | Second
)KEX");
        write("lib/two/widget.kex", R"KEX(module Two.Widget

type TwoKind = Other | Another
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
        write("lib/ids.kex", R"KEX(module Ids

distinct type UserId = String
)KEX");
        write("lib/boxes.kex", R"KEX(module Boxes

record Box do
  label : String
end

make Box, implement: Showable do
  let named(label: String) -> Box = Box { label: label }
  let showValue -> String = "box ${@label}"
end
)KEX");
        // A module named like the record it declares, whose `make` functions
        // another unit calls through the module (`Team.handle?(text)`): they
        // are the interface's receiver functions, not its exports. Its
        // constant gives it exports, so the alias is read as the module. Its
        // `make` for another type, with a function of the same name, is not
        // what the alias reaches.
        write("lib/teams/team.kex", R"KEX(module Teams.Team

using Boxes, only: [Box]

record Team do
  handle : String
end

let LONGEST_HANDLE = 30

# Another type's function of the same name, first in the interface: the
# alias still names Team.
make Box do
  handled : String -> String
  let handled(name) = "box ${name}"
end

make Team do
  handle? : String -> Bool
  let handle?(text) = text.length >= 2

  handled : String -> String
  let handled(name) = name.lowerCase
end
)KEX");
        // ...beside a constructor of the same name from another unit.
        write("lib/team_events.kex", R"KEX(module TeamEvents

type TeamEvent = Joined | Team
)KEX");
        write("lib/team_users.kex", R"KEX(module TeamUsers

using Teams.Team, only: [Team]
using Teams.Team, as: Team
using TeamEvents, only: [TeamEvent]

let checked() -> String = "handle ${Team.handle?("ab")} ${Team.handled("AB")}"
)KEX");
        write("lib/crates.kex", R"KEX(module Crates

record Crate do
  count : Integer
end

make Crate, implement: Showable do
  let showValue -> String = "crate of ${@count}"
end
)KEX");
        write("lib/bags.kex", R"KEX(module Bags

record Bag do
  items : [Integer]
end

make Bag, implement: Foldable do
  let reduce(acc: A, f: A -> Integer -> A) -> A = @items.reduce(acc, f)
end

bag : [Integer] -> Bag
let bag(items) = Bag { items: items }
)KEX");
        write("lib/fields.kex", R"KEX(module Fields

record IntField do
  label : String
end

record TextField do
  label : String
end

make IntField do
  let ==(val: Integer) -> String = "intfield ${@label} == ${val}"
end

make TextField do
  let ==(val: String) -> String = "textfield ${@label} == ${val}"
end
)KEX");
        write("lib/tokens.kex", R"KEX(module Tokens

distinct type Token = String

make Token do
  let blank? -> Bool = this.as(String).empty?
end
)KEX");
        write("lib/paths.kex", R"KEX(module Paths

join : String -> String -> String
let join(first, second) = "${first}/${second}"
)KEX");
        write("lib/path_user.kex", R"KEX(module PathUser

using Paths

shown : String
let shown = Paths.join("x", "y")
)KEX");
        write("lib/pages.kex", R"KEX(module Pages

let page = { |text| "page ${text}" }
)KEX");
        write("lib/thing_ids.kex", R"KEX(module ThingIds

distinct type ThingId = String
make ThingId do
  let empty? -> Bool = true
end
)KEX");
        write("lib/conditions.kex", R"KEX(module Conditions

type Cond = One(Integer) | Any([Cond]) | All([Cond])

make Cond do
  let ||(other: Cond) -> Cond = Any([this, other])
  let &&(other: Cond) -> Cond = All([this, other])
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
  let loud -> String = Marks.exclaimed(@text)
end
)KEX");
        // A `make` naming a nested module's record from its module, and a
        // function the prelude has for another type (`FileHandle.getLine`).
        write("lib/wire.kex", R"KEX(module Wire

module Tcp do
  record Conn do
    id : Integer = 0
  end

  foul open() -> Conn = Conn {}
end

make Tcp.Conn do
  foul getLine -> String? = Just("wire line")
end
)KEX");
        write("lib/web/routes.kex", R"KEX(module Web.Routes

record Context do
  path : String
end

type Reply = String | Web.Routes.Context
type Outcome = Reply | Result<Reply, Any> | Reply?
type Action = Context -> Outcome

get : String -> Action -> String
let get(path, action) = "route ${path}"
get : String -> Block<Outcome> -> String
let get(path, block) = "block ${path}"
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

    // app/main.kex built (`kex -c`, unit by unit): its output, the check's
    // diagnostics.
    auto check() -> std::string {
        const auto output = root / "output";
        std::filesystem::create_directories(root / "checked");
        const auto command =
            "KEX_CACHE=" + (root / "cache").string() + " " + std::string(KEX_BINARY_PATH) +
            " --no-colors -c -o " + (root / "checked").string() + " --source-root " +
            (root / "app").string() + " --source-root " + (root / "lib").string() + " " +
            (root / "app" / "main.kex").string() + " > " + output.string() + " 2>&1";
        runShell(command);
        return read(output);
    }

    // The program built unit by unit, its Core also kept with
    // KEX_SEPARATE_CORE, each compiled and run: what the build ran, then what
    // the kept Core runs.
    auto run() -> std::pair<std::string, std::string> {
        const auto roots = " --source-root " + (root / "app").string() + " --source-root " +
                           (root / "lib").string() + " ";
        const auto runAs = [&](const std::string& kind) {
            const auto cores = root / kind;
            const auto beams = root / (kind + "-beam");
            std::filesystem::create_directories(beams);
            const auto output = root / (kind + ".out");
            // erlc's warnings are no output of the program; its errors are
            // read back when nothing ran.
            const auto compiled = root / (kind + "-erlc.out");
            const auto command = "erlc -o " + beams.string() + " " + cores.string() + "/*.core > " +
                                 compiled.string() + " 2>&1 && " + std::string(KEX_BINARY_PATH) +
                                 " --no-colors --run " + (beams / "kex_main.beam").string() +
                                 " > " + output.string() + " 2>&1 || cat " + compiled.string() +
                                 " >> " + output.string();
            runShell(command);
            return read(output);
        };
        std::filesystem::create_directories(root / "actual");
        std::filesystem::create_directories(root / "separate");
        const auto actualBuild = "KEX_SEPARATE_CORE=" + (root / "separate").string() +
                                 " KEX_CACHE=" + (root / "cache").string() + " " +
                                 std::string(KEX_BINARY_PATH) + " --no-colors -E -o " +
                                 (root / "actual").string() + roots +
                                 (root / "app" / "main.kex").string() + " > " +
                                 (root / "actual-build.out").string() + " 2>&1";
        runShell(actualBuild);
        auto separate = runAs("separate");
        auto actual = runAs("actual");
        // What went wrong is in the build's own output when it emitted nothing.
        if (actual.find("open error") != std::string::npos) {
            actual += read(root / "actual-build.out");
        }
        runShell("KEX_TIMINGS=1 " + actualBuild);
        const auto warmBuild = read(root / "actual-build.out");
        assertTrue(
            warmBuild.find("unit cache hit:") != std::string::npos,
            "the warm separate build did not reuse a unit: " + warmBuild);
        assertEqual(runAs("actual"), actual);
        write("lib/paths.kex", R"KEX(module Paths

join : String -> String -> String
let join(first, second) = "${first}:${second}"
)KEX");
        runShell("KEX_TIMINGS=1 " + actualBuild);
        const auto changedBuild = read(root / "actual-build.out");
        assertTrue(
            changedBuild.find(
                "unit cache hit: " +
                std::filesystem::weakly_canonical(root / "lib" / "path_user.kex").string()) !=
                std::string::npos,
            "a dependent unit with unchanged interfaces was rebuilt: " + changedBuild);
        const auto changedRun = runAs("actual");
        assertTrue(
            changedRun.find("\na:b\n") != std::string::npos,
            "the changed unit's body was not rebuilt: " + changedRun + "\n---\n" + changedBuild);
        assertTrue(
            runAs("actual").find("\nx:y\n") != std::string::npos,
            "a cached dependent called the old implementation: " + changedBuild);
        return {std::move(actual), std::move(separate)};
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
using Web.Plugs, only: [Request, Step, Pair, Wrap, step, around, relayed]
using Web.Html, as: Html
using Tally, only: [Counter]
using Store, only: [Blob]
using Tickers
using Statuses, only: [STATUSES]
using Outer
using Web.Route, only: [get]
using UsesRoute
using Kinds
using One.Widget
using Two.Widget
using Notes
using Marks
using Boxes, only: [Box]
using TeamUsers
using Crates, only: [Crate]
using Bags
using Ids, only: [UserId]
using Wire
using Fields, only: [IntField, TextField]
using Tokens, only: [Token]
using FS
using ThingIds, only: [ThingId]
using Conditions, only: [Cond]
using Web.Routes

foul read(server: Server<Counter>) -> Result<Integer, CallError> = server.get()
foul readSoon(server: Server<Counter>) -> Result<Integer, CallError> = server.get(within: 100)

let shapes() -> [(String, Shapes.Shape)] = [("ring", Shapes.Circle(2)), ("dot", Shapes.Dot)]

let passed() -> Step = step do |request|
  request
end

let flagged() -> Pair = (1, true)

# An alias of another unit whose body names another of its aliases: the
# closure is read as a `Wrap`, and `around` takes it as one.
prefixed : String -> Wrap
let prefixed(tag) = do |inner|
  do |request| inner(Request { path: "${tag}${request.path}" }) end
end

# The same, through a union naming a qualified type, which is an alias and
# not an ADT whose constructors are `String` and `Request`.
relaying : String -> Web.Plugs.Relay
let relaying(tag) = do |inner|
  do |request| inner(Request { path: "${tag}${request.path}" }) end
end

# Which `showValue` runs is known only from the value: each unit's for its
# own type, the prelude's for an Integer.
let presented(value: Showable) -> String = "${value}"

# A literal of what it backs is a distinct type of another unit too.
let owner(id: UserId) -> String = "owner ${id}"

let tokenBlank(token: Token) -> Bool = token.blank?
let joined(value: Any) -> String = value.join(", ")
let either(left: Cond, right: Cond) -> Cond = left || right
let both(left: Cond, right: Cond) -> Cond = left && right

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
  IO.printLine(around(prefixed("/wrapped"))(Request { path: "/" }).path)
  IO.printLine(relayed(relaying("/relayed")))
  IO.printLine(shapes().length)
  IO.printLine({"a": 1}.get("a"))
  IO.printLine(Web.Route.get("ab", 2))
  IO.printLine(UsesRoute.summarized())
  IO.printLine(Type.of(Kinds.Red).constructors.map(&.name))
  IO.printLine(Type.of(One.Widget.First).constructors.map(&.name))
  IO.printLine(Type.of(Two.Widget.Other).constructors.map(&.name))
  IO.printLine(Notes.take("a"))
  with Notes = Notes.Local { prefix: "with:" } do
    IO.printLine(Notes.take("b"))
    IO.printLine(Notes.noted("c"))
  end
  IO.printLine(Mark { text: "m" }.loud)
  IO.printLine("hey".exclaimed)
  IO.printLine(presented(Box { label: "b" }))
  IO.printLine(Box.named("named").label)
  IO.printLine(TeamUsers.checked())
  IO.printLine(presented(Crate { count: 3 }))
  IO.printLine("bag ${Bags.bag([1, 2, 3]).any? { |x| x > 2 }}")
  IO.printLine(presented(7))
  IO.printLine(owner("u1"))
  IO.printLine(IntField { label: "n" } == 1)
  IO.printLine(TextField { label: "name" } == "x")
  IO.printLine(tokenBlank("abc"))
  IO.printLine(tokenBlank(""))
  IO.printLine(["?"].join(", "))
  IO.printLine(joined(["?"]))
  IO.printLine(Paths.join("a", "b"))
  IO.printLine(PathUser.shown())
  IO.printLine(Pages.page("x"))
  IO.printLine("x".as(ThingId).empty?)
  IO.printLine(either(Conditions.One(1), Conditions.One(2)))
  IO.printLine(both(Conditions.One(1), Conditions.One(2)))
  # A block ending in a `.try` on an Optional is an `Outcome`'s `Reply?`,
  # through the bare `get` the prelude's own overloads share a name with.
  IO.printLine(get("/maybe", do |_| "${[1].first.try}" end))
  IO.printLine(Wire.Tcp.open().getLine.or("no line"))
end
)KEX";

} // namespace

int main() {
    describe("the separate check", []() {
        it("finds nothing in a program without errors", []() {
            Package package;
            package.write("app/main.kex", program);
            const auto text = package.check();
            assertTrue(text.find("Aborted") == std::string::npos, "the build was aborted: " + text);
            assertTrue(
                text.find(": error: ") == std::string::npos, "a check reported an error: " + text);
        });
        it("finds each error a unit has", []() {
            Package package;
            package.write("app/main.kex", std::string(program) + R"KEX(
let wrong() -> String = Shapes.Dot

let skipped() -> String = define do |t| t.text(:id, doc: 1) end

foul timed(server: Server<Counter>) = server.reset(within: 10)
)KEX");
            const auto text = package.check();
            for (const std::string expected :
                 {"`wrong` declared to return String but body returns Shapes.Shape.Dot",
                  "`text` expects argument 5 to be String, but got Integer",
                  "`within:` is not valid on an asynchronous cast slot"}) {
                assertTrue(
                    text.find(expected) != std::string::npos,
                    "the check missed \"" + expected + "\": " + text);
            }
        });
    });
    describe("lowering unit by unit", []() {
        it("runs each unit's behaviour", []() {
            Package package;
            package.write("app/main.kex", program);
            const auto [ran, separate] = package.run();
            assertTrue(
                ran.find("error") == std::string::npos && !ran.empty(),
                "the build did not run: " + ran);
            assertEqual(separate, ran);
            assertTrue(
                ran.find("\nhandle true ab\n") != std::string::npos,
                "a make function of a module named like its type did not run: " + ran);
            assertTrue(
                ran.find("\nroute /maybe\n") != std::string::npos,
                "a block giving an Optional did not reach the union naming it: " + ran);
            assertTrue(
                ran.find("\n/wrapped/\n/relayed/\n") != std::string::npos,
                "a closure declared as another unit's nested alias did not run: " + ran);
            assertTrue(
                ran.find("\nbag true\n") != std::string::npos,
                "a trait default another unit's type inherits did not run: " + ran);
            assertTrue(
                ran.find("\n?\n?\n") != std::string::npos, "list.join called a path join: " + ran);
            assertTrue(
                ran.find("\npage x\ntrue\n") != std::string::npos,
                "cross-unit callable or distinct method did not run: " + ran);
            assertTrue(
                ran.find("Any([One(1), One(2)])") != std::string::npos &&
                    ran.find("All([One(1), One(2)])") != std::string::npos,
                "external union boolean operators did not run: " + ran);
            assertTrue(
                ran.find("\nwire line\n") != std::string::npos,
                "a make naming a nested module's record from its module did not run: " + ran);
        });
    });
    return runAll();
}
