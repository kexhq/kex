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

        it("reports one make function name on two same-named types", []() {
            const Files files = {
                {"metro/connection.kex", R"KEX(module Metro.Connection

record Connection do
  id : Integer = 0
end

make Connection do
  let send(message: String) -> String = "metro ${@id} got ${message}"
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
  IO.printLine(Metro.Connection.Connection { id: 3 }.send("x"))
  IO.printLine(Metro.Other.Connection { name: "ws" }.send("y"))
end
)KEX"}};
            for (const auto* mode : {"--run", "--run-walker"}) {
                const auto output = runProgram(files, mode);
                // Which module is merged first is not this test's business.
                assertTrue(
                    output.find("`send` is declared for both") != std::string::npos &&
                    output.find("`Metro.Connection.Connection`") != std::string::npos &&
                    output.find("`Metro.Other.Connection`") != std::string::npos,
                    std::string(mode) + " must name both types, got:\n" + output);
            }
        });
    });

    describe("a make function's name beside other modules' names (kexhq/kex#476)", []() {
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

        it("does not turn another record's field read into a call", []() {
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

    describe("a free function and a make function of one name (kexhq/kex#476)", []() {
        it("passes a foul make function its context", []() {
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

        it("keeps a foul make function apart from a pure one elsewhere (kexhq/kex#451)", []() {
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

        it("does not lend a make function's defaults to another receiver (kexhq/kex#475)", []() {
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
