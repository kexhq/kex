// Regression tests for kexhq/kex#476 and the issues it gathers (#448, #451,
// #475): a name in one module being resolved by its last segment against
// everything linked into the program, rather than against what the module
// declared and imported.
//
// Every case here type-checked (or parsed) before it was fixed and most failed
// only when run, and almost all of them need two or more modules in separate
// files — so, like multi_module_check_cli_test.cxx, this drives the real
// binary with `--source-root` instead of living in `spec/*.kex`. Each program
// runs on both backends, which are expected to agree.
#include "test.hxx"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace test;
namespace fs = std::filesystem;

namespace {

using Files = std::vector<std::pair<std::string, std::string>>;

auto popenCapture(const std::string& cmd) -> std::string {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        std::array<char, 4096> buf;
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
            result.append(buf.data(), n);
        pclose(pipe);
    }
    return result;
}

// Writes `files` (paths relative to a fresh project root, `main.kex` being the
// entry) and runs the program with `mode` — `--run` or `--run-walker`.
auto runProgram(const Files& files, const std::string& mode) -> std::string {
    char tmp[] = "/tmp/kex_name_collision_XXXXXX";
    fs::path root = mkdtemp(tmp);
    for (const auto& [path, contents] : files) {
        fs::create_directories((root / path).parent_path());
        std::ofstream(root / path) << contents;
    }
    auto output = popenCapture(
        std::string(KEX_BINARY_PATH) + " " + mode +
        " --no-colors --source-root " + root.string() + " " +
        (root / "main.kex").string() + " 2>&1");
    std::error_code ec;
    fs::remove_all(root, ec);
    return output;
}

auto expectOnBothBackends(const Files& files, const std::string& expected)
    -> void {
    assertEqual(runProgram(files, "--run"), expected, "BEAM");
    assertEqual(runProgram(files, "--run-walker"), expected, "tree-walker");
}

constexpr const char* kReply = R"KEX(module App.Reply

record Reply do
  body : String = ""
end

make Reply do
  let empty(status: Integer) -> String do
    "empty ${status}"
  end
  let text -> String do
    "text ${@body}"
  end
  let route -> String do
    "route ${@body}"
  end
end
)KEX";

} // namespace

int main() {
    describe("records of the same name in different modules (kexhq/kex#476)", []() {
        it("a qualified annotation names that type, not the module's own record", []() {
            expectOnBothBackends({
                {"metro/request.kex", R"KEX(module Metro.Request

using Net.HTTP, as: Web

record Request do
  incoming : Net.HTTP.Request<Binary>
  viaAlias : Web.Request<Binary>? = None
  code : Integer = 0
end

make Request do
  let verb -> String = @incoming.method
end

let wrap(incoming: Net.HTTP.Request<Binary>) -> Request = Request { incoming: incoming }
)KEX"},
                {"main.kex", R"KEX(using Metro.Request, only: [Request]

let describe(request: Request) -> String = "${request.verb} ${request.code}"

main do
  let incoming = Net.HTTP.Request { method: "GET", target: URI.parse("http://example.com/a").try, headers: Net.HTTP.Headers.empty, body: Binary.empty }
  let request = Metro.Request.wrap(incoming)
  IO.printLine(describe(request))
  IO.printLine(request.verb == "GET")
  IO.printLine(request.incoming.target.string)
end
)KEX"}},
                "GET 0\ntrue\nhttp://example.com/a\n");
        });

        it("an alias is read against its own module's imports", []() {
            expectOnBothBackends({
                {"metro/types.kex", R"KEX(module Metro.Types

using Net.HTTP, only: [Request]

type Incoming = Request<Binary>
)KEX"},
                {"metro/request.kex", R"KEX(module Metro.Request

using Metro.Types, only: [Incoming]

record Request do
  incoming : Incoming
  code : Integer = 0
end

make Request do
  let verb -> String = @incoming.method
end

let wrap(incoming: Incoming) -> Request = Request { incoming: incoming }
)KEX"},
                {"main.kex", R"KEX(main do
  let incoming = Net.HTTP.Request { method: "GET", target: URI.parse("http://example.com/a").try, headers: Net.HTTP.Headers.empty, body: Binary.empty }
  let request = Metro.Request.wrap(incoming)
  IO.printLine("${request.verb} ${request.code}")
end
)KEX"}},
                "GET 0\n");
        });

        it("an imported record outranks the prelude's of the same name", []() {
            // `Reply` is also the prelude's serving record.
            expectOnBothBackends({
                {"app/reply.kex", kReply},
                {"main.kex", R"KEX(using App.Reply, only: [Reply]

main do
  IO.printLine(Reply { body: "x" }.text)
end
)KEX"}},
                "text x\n");
        });

        it("keeps record identities through aliases and re-exports", []() {
            expectOnBothBackends({
                {"app/shapes.kex", R"KEX(module App.Shapes
record Circle do radius : Integer = 2 end
make Circle do
  let diameter -> Integer = @radius * 2
end
)KEX"},
                {"main.kex", R"KEX(using App.Shapes, as: Geo
module Public do
  export App.Shapes
end
main do
  IO.printLine(Geo.Circle { radius: 3 }.diameter)
  IO.printLine(Public.Shapes.Circle {}.diameter)
end
)KEX"}}, "6\n4\n");
        });

        it("dispatches one function name on two same-named records", []() {
            const Files files = {
                {"metro/connection.kex", R"KEX(module Metro.Connection

record Connection do
  id : Integer = 0
end

make Connection do
  let send(message: String = "default") -> String do
    "metro ${@id} got ${message}"
  end
end
)KEX"},
                {"metro/other.kex", R"KEX(module Metro.Other

record Connection do
  name : String = ""
end

make Connection do
  let send(message: String) -> String = "sent ${message} via ${@name}"
end
)KEX"},
                {"main.kex", R"KEX(main do
  IO.printLine(Metro.Connection.Connection { id: 3 }.send(message: "x"))
  IO.printLine(Metro.Other.Connection { name: "ws" }.send("y"))
  IO.printLine(Metro.Connection.Connection { id: 3 }.send())
end
)KEX"}};
            expectOnBothBackends(files,
                "metro 3 got x\nsent y via ws\nmetro 3 got default\n");
        });
    });

    describe("a method name beside other modules' names (kexhq/kex#476)", []() {
        it("leaves `Headers.empty` and Net.HTTP's own calls alone", []() {
            expectOnBothBackends({
                {"app/reply.kex", kReply},
                {"main.kex", R"KEX(using App.Reply, only: [Reply]
using Net.HTTP

foul go(url: String) -> Bool do
  return HTTP.delete(url).ok?
end

main do
  IO.printLine(Reply { body: "x" }.empty(204))
  IO.printLine(Reply { body: "x" }.text)
  IO.printLine(Reply { body: "x" }.route)
end
)KEX"}},
                "empty 204\ntext x\nroute x\n");
        });

        it("does not turn another record's field read into the method", []() {
            expectOnBothBackends({
                {"app/reply.kex", kReply},
                {"main.kex", R"KEX(using App.Reply, only: [Reply]
using Net.HTTP

main do
  IO.printLine(Reply { body: "x" }.route)
  let handler = { |request, context| context.route.parameters.keys.count }
  IO.printLine(handler(1, Context { route: RouteContext {} }))
end
)KEX"}},
                "route x\n0\n");
        });

        it("reads a field although a function shares its name (kexhq/kex#448)", []() {
            expectOnBothBackends({
                {"main.kex", R"KEX(record Target do
  string : String = ""
end

record Incoming do
  target : Target = Target {}
  flag : Bool = false
  id : String = "i"
end

let target(incoming: Incoming) -> String = "fn"
let flag(name: String, short: String, help: String, fallback: Bool) -> String = name
let id(column: Integer, label: String) -> String = label

main do
  let incoming = Incoming { target: Target { string: "/x" } }
  IO.printLine(incoming.target.string)
  IO.printLine([incoming].map { |entry| entry.flag })
  IO.printLine([incoming].map { |opening| opening.id })
end
)KEX"}},
                "/x\n[false]\n[i]\n");
        });
    });

    describe("field reads in unannotated blocks (kexhq/kex#476)", []() {
        it("does not infer a function's arity for an ambiguous field", []() {
            expectOnBothBackends({
                {"main.kex", R"KEX(record First do
  flag : Bool = false
end

record Second do
  flag : Bool = false
end

let flag(name: String, short: String, help: String, fallback: Bool) -> String = name

main do
  let handler = { |entry| entry.flag }
  IO.printLine(handler(First { flag: true }))
  IO.printLine(handler(Second { flag: false }))
end
)KEX"}}, "true\nfalse\n");
        });
        it("reads a field before trying an unrelated function", []() {
            expectOnBothBackends({
                {"main.kex", R"KEX(using Net.HTTP

let parameters(context: Context) -> {String: String} = { "wrong": "x" }

foul source(context: Context) -> Context do
  IO.printLine("once")
  context
end

main do
  let handler = { |context| source(context).route.parameters }
  let context = Context {
    route: RouteContext { parameters: { "id": "7" } }
  }
  let result = handler(context)
  IO.printLine(result["id"].or("wrong"))
  IO.printLine(context.parameters["wrong"].or("missing"))
end
)KEX"}}, "once\n7\nx\n");
        });
    });

    describe("a function and a method of one name (kexhq/kex#476)", []() {
        it("keeps a top-level function separate from dependency receiver dispatch", []() {
            expectOnBothBackends({
                {"app/service.kex", R"KEX(module App.Service
record Service do value : String end
make Service do
  let served(suffix: String) -> String = "record ${@value} ${suffix}"
end
)KEX"},
                {"main.kex", R"KEX(using App.Service, only: [Service]
let served(value: String, suffix: String) -> String = "free ${value} ${suffix}"
main do
  IO.printLine(served("a", "b"))
  IO.printLine(Service { value: "c" }.served("d"))
  let call = ~served
  IO.printLine(call("e", "f"))
  IO.printLine("g".served("h"))
end
)KEX"}}, "free a b\nrecord c d\nfree e f\nfree g h\n");
        });

        it("keeps a module's free function separate from its make function", []() {
            expectOnBothBackends({
                {"app/forms.kex", R"KEX(module App.Forms
record Env do raw : String end
make Env do
  let formFields -> [String] = decodeFields(@raw)
end
private do
  let formFields(text: String) -> [String] = text.split(",")
  let decodeFields(text: String) -> [String] = formFields(text)
end
let fields(text: String) -> [String] = Env { raw: text }.formFields
)KEX"},
                {"main.kex", R"KEX(main do
  IO.printLine(App.Forms.fields("a,b"))
end
)KEX"}}, "[a, b]\n");
        });
        it("passes a foul method its context", []() {
            expectOnBothBackends({
                {"app/repo.kex", R"KEX(module App.Repo

record Repo do
  db : String
end

make Repo do
  foul founded(organization: String, title: String, by: String) -> String do
    return "${@db}/${organization}/${title}/${by}"
  end
end
)KEX"},
                {"app/site.kex", R"KEX(module App.Site

using App.Repo, only: [Repo]

foul founded(database: String, user: String, tab: String, title: String) -> String do
  return Repo { db: database }.founded(user, title, tab)
end

foul viaVariable(database: String) -> String do
  let conversations = Repo { db: database }
  return conversations.founded("o", "t", "b")
end
)KEX"},
                {"main.kex", R"KEX(main do
  IO.printLine(App.Site.founded("db", "u", "t", "T"))
  IO.printLine(App.Site.viaVariable("d2"))
end
)KEX"}},
                "db/u/T/t\nd2/o/t/b\n");
        });

        it("keeps a foul method apart from a pure one elsewhere (kexhq/kex#451)", []() {
            // `Net.HTTP.Headers.remove(name)` is pure at the same arity.
            expectOnBothBackends({
                {"db/holdings.kex", R"KEX(module DB.Holdings

record HoldingRepository do
  owner : String
end

make HoldingRepository do
  foul remove(id: String) -> Bool do
    return id == @owner
  end
end

let of(owner: String) -> HoldingRepository = HoldingRepository { owner: owner }
)KEX"},
                {"config/routes.kex", R"KEX(module Config.Routes

using Net.HTTP

foul home(request: Request<Binary>, context: Context) -> Response<Binary> = Response.text(200, "home")

let routes(label: String) -> Router = Router {}.get("/", ~home)
)KEX"},
                {"main.kex", R"KEX(main do
  IO.printLine(DB.Holdings.of("x").remove("nope"))
  IO.printLine(Config.Routes.routes("a").routes.count)
end
)KEX"}},
                "false\n1\n");
        });

        it("does not lend a method's defaults to another receiver (kexhq/kex#475)", []() {
            expectOnBothBackends({
                {"main.kex", R"KEX(record Builder do
  table : String = ""
end

make Builder do
  let string(label: Atom, primaryKey: Bool = false, nullable: Bool = false) -> String do
    "${label} ${primaryKey} ${nullable}"
  end
end

main do
  IO.printLine(Builder {}.string(:id, primaryKey: true))
  let parsed = URI.parse("https://example.com/a?b=1").try
  IO.printLine(parsed.string)
end
)KEX"}},
                ":id true false\nhttps://example.com/a?b=1\n");
        });
    });

    describe("a field beside another module's function (kexhq/kex#448)", []() {
        it("compiles and reads the field, typed or not, and still calls the function", []() {
            expectOnBothBackends({
                {"lib/kind.kex", R"KEX(module Lib.Kind

record Kind do
  label : String
end

let caption(kind: Kind) -> String = "caption of ${kind.label}"
)KEX"},
                {"app/views.kex", R"KEX(module App.Views

record Entry do
  caption : String = "entry"
end

let shown(entry: Entry) -> String = entry.caption
)KEX"},
                {"main.kex", R"KEX(using Lib.Kind, only: [Kind]
using App.Views, only: [Entry]

record Statement do
  caption : String = "statement"
end

main do
  IO.printLine(Statement {}.caption)
  IO.printLine(App.Views.shown(Entry {}))
  IO.printLine([Entry { caption: "listed" }].map { |entry| entry.caption }.join(","))
  let pick = { |item| item.caption }
  IO.printLine(pick(Statement { caption: "picked" }))
  IO.printLine(Lib.Kind.caption(Kind { label: "kind" }))
end
)KEX"}},
                "statement\nentry\nlisted\npicked\ncaption of kind\n");
        });

        it("reads a field in a module's embedded template (kexhq/kex#533)", []() {
            expectOnBothBackends({
                {"lib/views.kex", R"KEX(module Lib.Views

record Card do
  name : String
  link : String
end

let cardPartial = Template.html(Kex.embed("_card.html.ket"))

shown : Card -> String
let shown(card) = cardPartial(card)
)KEX"},
                {"lib/_card.html.ket", R"KEX(---
params: [card: Card]
---
<p><%= card.name %> <%= card.link %></p>
)KEX"},
                {"main.kex", R"KEX(main do
  IO.print(Lib.Views.shown(Lib.Views.Card { name: "Ada", link: "/u" }))
end
)KEX"}},
                "<p>Ada /u</p>\n");
        });
    });

    describe("an imported name in another module's make signature (kexhq/kex#532)", []() {
        it("types the overloaded || on a value that make function answers", []() {
            expectOnBothBackends({
                {"lib/schema.kex", R"KEX(module Lib.Schema

type Cond = EqCond(String, String) | AnyCond([Cond])

record TextColumn do
  label : String
end

make TextColumn do
  let ==(val: String) -> Lib.Schema.Cond = EqCond(@label, val)
end

make Cond do
  let ||(other: Lib.Schema.Cond) -> Lib.Schema.Cond = AnyCond([this, other])
end

text : String -> TextColumn
let text(label) = TextColumn { label: label }

described : Cond -> String
let described(cond) do
  match cond do
    EqCond(label, value) => "${label}=${value}"
    AnyCond(parts) => parts.map { |part| described(part) }.join(" or ")
  end
end
)KEX"},
                {"lib/table.kex", R"KEX(module Lib.Table

using Lib.Schema, only: [TextColumn]

record Definition do
  label : String
end

make Definition do
  textColumn :> String -> TextColumn
  let textColumn(name) = Lib.Schema.text(name)
end
)KEX"},
                {"main.kex", R"KEX(main do
  let users = Lib.Table.Definition { label: "users" }
  let owner = users.textColumn("owner")
  IO.print(Lib.Schema.described(owner == "a" || owner == "b"))
end
)KEX"}},
                "owner=a or owner=b");
        });
    });

    describe("a make function overloaded by name with a defaulted parameter (kexhq/kex#531)", []() {
        it("calls the overload that takes nothing, and the other without its default", []() {
            expectOnBothBackends({
                {"lib/query.kex", R"KEX(module Lib.Query

record Query do
  n : Integer = 0
end

let start -> Query = Query {}

make Query do
  foul all -> [String] = ["u1", "u2"]

  foul all(as: T, renamed: {String: String} = {}) -> [T] = this.all.map { |id| Type.of(as).construct({"id": id}).or(as) }
end
)KEX"},
                {"main.kex", R"KEX(record User do
  id : String
end

main do
  IO.printLine(Lib.Query.start.all(as: User).map(&.id).join(","))
  IO.printLine(Lib.Query.start.all.join(","))
end
)KEX"}},
                "u1,u2\nu1,u2\n");
        });
    });

    describe("same-named records with a same-named method (kexhq/kex#476)", []() {
        it("does not lend one type's default to the other's call", []() {
            expectOnBothBackends({
                {"first/net.kex", R"KEX(module First.Net

record Connection do
  label : String = "first"
end

make Connection do
  let send(text: String, loud: Bool = false) -> String do
    "${@label} sends ${loud then text.upperCase else text}"
  end

  foul close() -> String do
    return "${@label} closed"
  end
end
)KEX"},
                {"second/net.kex", R"KEX(module Second.Net

record Connection do
  label : String = "second"
  count : Integer = 0
end

make Connection do
  let send(text: String) -> String = "${@label} posts ${text} (${@count})"
  let close() -> String = "${@label} shut"
end

let opened(count: Integer) -> Connection = Connection { count: count }
)KEX"},
                {"main.kex", R"KEX(main do
  let first = First.Net.Connection {}
  let second = Second.Net.opened(3)
  IO.printLine(first.send("hi"))
  IO.printLine(first.send("hi", loud: true))
  IO.printLine(second.send("hi"))
  IO.printLine(first.close())
  IO.printLine(second.close())
end
)KEX"}},
                "first sends hi\nfirst sends HI\nsecond posts hi (3)\n"
                "first closed\nsecond shut\n");
        });
    });

    describe("capability stand-ins (kexhq/kex#476)", []() {
        it("reaches the stand-in's own method when two types share its name", []() {
            expectOnBothBackends({
                {"lib/connection.kex", R"KEX(capability Lib.Connection do
  foul run(statement: String) -> String = "no database: ${statement}"
end
)KEX"},
                {"lib/direct.kex", R"KEX(module Lib.Direct

record Connection do
  label : String = "direct"
end

make Connection, implement: Lib.Connection do
  foul run(statement: String) -> String do
    return "${@label} ran ${statement}"
  end
end

foul sent(statement: String) -> String = Lib.Connection.run(statement)
)KEX"},
                {"app/store.kex", R"KEX(module App.Store

record Connection do
  label : String = "store"
end

make Connection, implement: Lib.Connection do
  foul run(statement: String) -> String do
    return "${@label} ran ${statement}"
  end
end
)KEX"},
                {"main.kex", R"KEX(main do
  IO.printLine(Lib.Direct.sent("SELECT 1"))
  with Lib.Connection = Lib.Direct.Connection {} do
    IO.printLine(Lib.Direct.sent("SELECT 2"))
  end
  with Lib.Connection = App.Store.Connection {} do
    IO.printLine(Lib.Direct.sent("SELECT 3"))
  end
end
)KEX"}},
                "no database: SELECT 1\ndirect ran SELECT 2\nstore ran SELECT 3\n");
        });
    });

    describe("function references (kexhq/kex#476)", []() {
        it("names the function through a qualifier, an alias and an import", []() {
            expectOnBothBackends({
                {"app/controllers/chat.kex", R"KEX(module App.Controllers.Chat

record WebRequest do
  path : String = ""
end

let show(request: WebRequest) -> WebRequest = WebRequest { path: "shown" }
let joined(request: WebRequest, socket: String) -> String = "${request.path} ${socket}"
)KEX"},
                {"main.kex", R"KEX(using App.Controllers.Chat, as: Chat
using App.Controllers.Chat, only: [WebRequest, joined]

record Route do
  handler : WebRequest -> WebRequest
end

record Channel do
  handler : WebRequest -> String -> String
end

let run(route: Route) -> String = route.handler(WebRequest {}).path
let join(channel: Channel) -> String = channel.handler(WebRequest { path: "p" }, "s")

main do
  IO.printLine(run(Route { handler: ~App.Controllers.Chat.show }))
  IO.printLine(run(Route { handler: ~Chat.show }))
  IO.printLine(join(Channel { handler: ~joined }))
end
)KEX"}},
                "shown\nshown\np s\n");
        });
    });

    return runAll();
}
