// Two Kex programs as two connected BEAM nodes.
//
// Node `a` runs a `serving` counter and a registered request loop; node `b`
// connects to it (`--sname`/`--cookie`, `Node.connect`) and checks what has
// to work across the wire:
//
//  - Node.list / Node.whereis / Node.send to a registered name;
//  - a typed record in a message, and a reply built from it;
//  - a typed `Server` handle sent from a to b, whose slots b then calls —
//    the call runs on a, against a's state;
//  - Node.spawn of a block from b's program, which a has never loaded — the
//    runtime ships the module's code over first.
//
// Two VMs are the point, so this drives the real binary rather than living
// in spec/*.kex. A machine that cannot start distributed Erlang (no epmd, a
// sandbox without networking) skips rather than fails: `b` reports whether
// it came up as a node before anything else.
#include "test.hxx"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace test;
namespace fs = std::filesystem;

namespace {

auto writeFile(const fs::path& path, const std::string& contents) -> void {
    std::ofstream file(path);
    file << contents;
}

auto readFile(const fs::path& path) -> std::string {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

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

auto replaceAll(std::string text, const std::string& from, const std::string& to)
    -> std::string {
    for (size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

// Declarations both programs share, so each side can build and match the
// other's messages. Identical source compiles to identical tags.
constexpr const char* kShared = R"KEX(using Node

record Counter do
  n : Integer
end

serving Counter do
  slot bump -> Reply<Integer> do
    new.n = @n + 1
    return { new, reply: new.n }
  end
end

record Greeting do
  text : String
  times : Integer
end

type Request = Ping(Pid, Greeting) | GetCounter(Pid) | Stop
)KEX";

constexpr const char* kNodeA = R"KEX(
foul serve(counter: Server<Counter>) do
  receive do
    Ping(from, greeting) => do
      from.send((:pong, Node.self, greeting.text.repeat(greeting.times)))
      serve(counter)
    end
    GetCounter(from) => do
      from.send(counter)
      serve(counter)
    end
    Stop => IO.printLine("a stopped")
  after timeout: 20000
    IO.printLine("a timed out")
  end
end

main do
  let counter = Process.spawn(Counter { n: 0 })
  Process.register(Process.self, :requests)
  serve(counter)
end
)KEX";

constexpr const char* kNodeB = R"KEX(
foul pause(ms: Integer) do
  receive do
  after timeout: ms
  end
end

# `a` needs a moment to come up and register; ask until it has.
foul awaitNode(a: Atom, tries: Integer) -> Bool do
  if tries == 0
    return false
  end
  if Node.connect(a) && Node.whereis(a, :requests).present?
    return true
  end
  pause(100)
  awaitNode(a, tries - 1)
end

main do
  IO.printLine("alive: ${Node.alive?}")
  let host = Node.self.string.split("@").last.or("")
  let a = Atom.from("NODE_A@" + host)
  IO.printLine("reached a: ${awaitNode(a, 100)}")
  IO.printLine("a listed: ${Node.list.contains?(a)}")

  Node.send(a, :requests, Ping(Process.self, Greeting { text: "hi", times: 3 }))
  receive do
    (:pong, from, text) => IO.printLine("pong from a: ${from == a} ${text}")
  after timeout: 5000
    IO.printLine("no pong")
  end

  Node.send(a, :requests, GetCounter(Process.self))
  receive do
    handle => do
      let counter: Server<Counter> = handle
      IO.printLine("remote bump: ${counter.bump()}")
      IO.printLine("remote bump: ${counter.bump()}")
    end
  after timeout: 5000
    IO.printLine("no counter")
  end

  let me = Process.self
  Node.spawn(a) do me.send((:ran_on, Node.self)) end
  receive do
    (:ran_on, where) => IO.printLine("spawned on a: ${where == a}")
  after timeout: 5000
    IO.printLine("remote spawn: no reply")
  end

  Node.send(a, :requests, Stop)
end
)KEX";

} // namespace

int main() {
    describe("two Kex nodes", []() {
        it("message, call and spawn across a connection", []() {
            char tmp[] = "/tmp/kex_cluster_XXXXXX";
            fs::path root = mkdtemp(tmp);
            const std::string suffix = std::to_string(getpid());
            const std::string nodeA = "kexa" + suffix;
            const std::string nodeB = "kexb" + suffix;
            const std::string cookie = "kexcluster" + suffix;
            writeFile(root / "node_a.kex", std::string(kShared) + kNodeA);
            writeFile(root / "node_b.kex",
                      std::string(kShared) + replaceAll(kNodeB, "NODE_A", nodeA));

            const std::string kex = KEX_BINARY_PATH;
            const auto logA = root / "a.log";
            std::system((kex + " --no-colors --sname " + nodeA + " --cookie " + cookie +
                         " " + (root / "node_a.kex").string() + " > " + logA.string() +
                         " 2>&1 &").c_str());
            auto outputB = popenCapture(kex + " --no-colors --sname " + nodeB +
                                        " --cookie " + cookie + " " +
                                        (root / "node_b.kex").string() + " 2>&1");

            if (outputB.rfind("alive: true", 0) != 0) {
                std::cout << "    (skipped: this machine cannot start a distributed "
                             "Erlang node)\n" << outputB;
                std::error_code ec;
                fs::remove_all(root, ec);
                return;
            }

            // `a` exits once it gets Stop; give it a moment to say so.
            std::string outputA;
            for (int i = 0; i < 100; i++) {
                outputA = readFile(logA);
                if (outputA.find('\n') != std::string::npos) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            assertEqual(outputB,
                        std::string("alive: true\n"
                                    "reached a: true\n"
                                    "a listed: true\n"
                                    "pong from a: true hihihi\n"
                                    "remote bump: 1\n"
                                    "remote bump: 2\n"
                                    "spawned on a: true\n"),
                        "node b's view of the cluster");
            assertEqual(outputA, std::string("a stopped\n"), "node a's own output");

            std::error_code ec;
            fs::remove_all(root, ec);
        });
    });

    describe("examples/cluster", []() {
        // The example as its header tells a reader to run it: two terminals,
        // fixed node names `store` and `client`. Kept under test so the
        // example cannot quietly stop working.
        it("runs the store and client nodes together", []() {
            const std::string kex = KEX_BINARY_PATH;
            const fs::path dir = fs::absolute("examples/cluster");
            char tmp[] = "/tmp/kex_cluster_example_XXXXXX";
            fs::path root = mkdtemp(tmp);
            const auto logStore = root / "store.log";
            std::system(("cd " + dir.string() + " && " + kex +
                         " --no-colors --sname store --cookie kexexample store_node.kex > " +
                         logStore.string() + " 2>&1 &").c_str());
            auto client = popenCapture("cd " + dir.string() + " && " + kex +
                                       " --no-colors --sname client --cookie kexexample"
                                       " client_node.kex 2>&1");
            std::string store;
            for (int i = 0; i < 100; i++) {
                store = readFile(logStore);
                if (store.find("stopping") != std::string::npos) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::error_code ec;
            fs::remove_all(root, ec);
            if (client.find("Start this node with a name") != std::string::npos ||
                client.find("No store at") != std::string::npos) {
                std::cout << "    (skipped: no distributed Erlang here)\n" << client;
                return;
            }
            assertTrue(client.find("greeting: hello from :store@") != std::string::npos &&
                           client.find("keys now: [client@") != std::string::npos &&
                           client.find(", greeting]") != std::string::npos &&
                           client.find("spawned block ran on :store@") != std::string::npos,
                       "client_node.kex output: " + client);
            assertTrue(store.find("store: handing the store to a client") != std::string::npos &&
                           store.find("store: stopping") != std::string::npos,
                       "store_node.kex output: " + store);
        });
    });

    return runAll();
}
