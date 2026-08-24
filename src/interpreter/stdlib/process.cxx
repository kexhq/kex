#include "../evaluator.hxx"

#ifndef __EMSCRIPTEN__
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>

namespace {

#ifndef __EMSCRIPTEN__
// Reads both pipes to EOF, closing each as it ends. Interleaved rather than
// sequential on purpose: a child that writes a lot to the stream we are not
// reading yet would block forever once its pipe buffer filled.
auto drainPipes(int outFd, std::string& out, int errFd, std::string& err)
    -> void {
    struct pollfd fds[2] = {{outFd, POLLIN, 0}, {errFd, POLLIN, 0}};
    std::string* targets[2] = {&out, &err};
    char buffer[4096];
    while (fds[0].fd >= 0 || fds[1].fd >= 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !fds[i].revents) continue;
            const auto got = read(fds[i].fd, buffer, sizeof(buffer));
            if (got > 0) {
                targets[i]->append(buffer, static_cast<std::size_t>(got));
                continue;
            }
            if (got < 0 && errno == EINTR) continue;
            // EOF, or an error there is no way to report per-stream.
            close(fds[i].fd);
            fds[i].fd = -1;
        }
    }
    for (auto& descriptor : fds)
        if (descriptor.fd >= 0) close(descriptor.fd);
}
#endif

#ifndef __EMSCRIPTEN__
// Whether the command names a program this process could actually run —
// PATH lookup included, the same question `os:find_executable/1` answers on
// BEAM. Asked BEFORE forking so both backends report a missing program the
// same way: an Error, not an Ok carrying the shell's 127.
auto executableExists(const std::string& command) -> bool {
    auto runnable = [](const std::string& path) {
        return !path.empty() && access(path.c_str(), X_OK) == 0;
    };
    if (command.find('/') != std::string::npos) return runnable(command);
    const char* search = std::getenv("PATH");
    if (!search) return false;
    const std::string paths = search;
    for (std::size_t start = 0; start <= paths.size();) {
        const auto end = paths.find(':', start);
        const auto directory = paths.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (runnable((directory.empty() ? std::string(".") : directory) +
                     "/" + command))
            return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}
#endif

} // namespace

namespace kex::interpreter {

// spawn/receive themselves are handled directly in Evaluator::eval (they
// need direct access to the Scheduler, not just argument values — see the
// SpawnExpr/ReceiveExpr branches). This file only covers the ordinary
// builtins: Process.self and pid.send(msg).
auto Evaluator::registerProcessBuiltins() -> void {

    // Pre-register the namespace placeholder so `Process.self` resolves via
    // the ModuleValue namespace-dispatch branch in eval() (ast::MethodCall),
    // the same convention used by the remaining public-native namespaces.
    defineModule("Process");

    defineIntrinsic("Process::run", [](std::vector<ValuePtr> args) -> ValuePtr {
#ifdef __EMSCRIPTEN__
        return Value::error(Value::string("process execution is unavailable in wasm"));
#else
        if (args.size() != 2)
            return Value::error(Value::string("Process.run expects a command and argument list"));
        const auto* command = std::get_if<StringValue>(&args[0]->data);
        const auto* list = std::get_if<ListValue>(&args[1]->data);
        if (!command || !list)
            return Value::error(Value::string("Process.run expects (String, [String])"));

        std::vector<std::string> strings;
        strings.reserve(list->elements.size() + 1);
        strings.push_back(command->value);
        for (const auto& value : list->elements) {
            const auto* string = std::get_if<StringValue>(&value->data);
            if (!string)
                return Value::error(Value::string("Process.run arguments must be strings"));
            strings.push_back(string->value);
        }

        if (!executableExists(strings.front()))
            return Value::error(Value::string("executable not found"));

        // Both streams come back over PIPES rather than through temporary
        // files: nothing the child prints touches the disk, and there is no
        // path to collide over or clean up. The parent must drain both ends
        // as they fill — reading one to EOF first would deadlock as soon as
        // the child filled the other pipe's buffer (64K on Linux) — so poll
        // sits on the pair until each side reports EOF.
        int outPipe[2];
        int errPipe[2];
        if (pipe(outPipe) < 0)
            return Value::error(Value::string("could not create process pipes"));
        if (pipe(errPipe) < 0) {
            close(outPipe[0]);
            close(outPipe[1]);
            return Value::error(Value::string("could not create process pipes"));
        }

        const pid_t child = fork();
        if (child == 0) {
            dup2(outPipe[1], STDOUT_FILENO);
            dup2(errPipe[1], STDERR_FILENO);
            close(outPipe[0]);
            close(outPipe[1]);
            close(errPipe[0]);
            close(errPipe[1]);
            std::vector<char*> argv;
            argv.reserve(strings.size() + 1);
            for (auto& string : strings) argv.push_back(string.data());
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        close(outPipe[1]);
        close(errPipe[1]);
        if (child < 0) {
            close(outPipe[0]);
            close(errPipe[0]);
            return Value::error(Value::string("could not start process"));
        }

        std::string stdoutText;
        std::string stderrText;
        drainPipes(outPipe[0], stdoutText, errPipe[0], stderrText);

        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                                              : 128 + WTERMSIG(status);
        return Value::ok(Value::record("ProcessResult", {
            {"exitCode", Value::integer(exitCode)},
            {"stdout", Value::string(stdoutText)},
            {"stderr", Value::string(stderrText)},
        }));
#endif
    });


    // Process.stream — run a child on THIS process's stdout and stderr, and
    // answer only its exit code.
    //
    // `Process.run` captures both streams and can only hand them back once
    // the child has exited, so anything long-running shows a blank terminal
    // and then dumps everything at the end: `tey test` and a package's own
    // commands looked stalled for their whole run (kexhq/kex#187). Here the
    // child simply inherits the descriptors, so its output is the parent's
    // output, in real time and with its own buffering and colour decisions
    // intact — a child that checks for a terminal still sees one.
    //
    // Nothing is captured, by definition. Callers that need the text keep
    // using `run`; callers that need a person to watch progress use this.
    defineIntrinsic("Process::stream", [](std::vector<ValuePtr> args) -> ValuePtr {
#ifdef __EMSCRIPTEN__
        return Value::error(Value::string("process execution is unavailable in wasm"));
#else
        if (args.size() != 2)
            return Value::error(Value::string("Process.stream expects a command and argument list"));
        const auto* command = std::get_if<StringValue>(&args[0]->data);
        const auto* list = std::get_if<ListValue>(&args[1]->data);
        if (!command || !list)
            return Value::error(Value::string("Process.stream expects (String, [String])"));

        std::vector<std::string> strings;
        strings.reserve(list->elements.size() + 1);
        strings.push_back(command->value);
        for (const auto& value : list->elements) {
            const auto* string = std::get_if<StringValue>(&value->data);
            if (!string)
                return Value::error(Value::string("Process.stream arguments must be strings"));
            strings.push_back(string->value);
        }

        if (!executableExists(strings.front()))
            return Value::error(Value::string("executable not found"));

        // The parent's own buffers are flushed first: anything printed
        // before this call is still sitting in them, and the child writes
        // straight to the descriptor — without this the child's first line
        // can appear above a banner that was printed before it.
        std::fflush(stdout);
        std::fflush(stderr);

        const pid_t child = fork();
        if (child == 0) {
            std::vector<char*> argv;
            argv.reserve(strings.size() + 1);
            for (auto& string : strings) argv.push_back(string.data());
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        if (child < 0)
            return Value::error(Value::string("could not start process"));

        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                                              : 128 + WTERMSIG(status);
        return Value::ok(Value::integer(exitCode));
#endif
    });
    // Walker-native scheduler fallback. This can be called from concurrently
    // scheduled processes, where entering a Kex wrapper would mutate the
    // evaluator's shared environment frame.
    defineDual("Process::self", [this](std::vector<ValuePtr>) -> ValuePtr {
        return Value::process(m_scheduler->currentProcessId(), m_scheduler.get());
    });

    defineDual("Process::spawn", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        return Value::server(m_scheduler->startServer(args[0]), m_scheduler.get());
    });

    defineIntrinsic("Process::reply", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::record("Reply", {{"reply", args.empty() ? Value::unit() : args[0]}});
    });

    defineIntrinsic("Process::cast", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::unit();
    });

    defineIntrinsic("Process::replyFrom", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::unit();
        auto* from = std::get_if<RecordValue>(&args[0]->data);
        if (!from || from->typeName != "From") return Value::unit();
        auto pidIt = from->fields.find("pid");
        auto refIt = from->fields.find("ref");
        if (pidIt == from->fields.end() || refIt == from->fields.end()) return Value::unit();
        auto* pid = std::get_if<ProcessValue>(&pidIt->second->data);
        auto* ref = std::get_if<IntValue>(&refIt->second->data);
        if (!pid || !ref) return Value::unit();
        pid->scheduler->send(pid->pid, Value::tuple({Value::atom("server_reply"),
            Value::integer(ref->value), Value::ok(args[1])}));
        return Value::unit();
    });

    defineIntrinsic("Process::fromPid", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* from = std::get_if<RecordValue>(&args[0]->data);
        if (!from) return Value::none();
        auto found = from->fields.find("pid");
        return found == from->fields.end() ? Value::none() : found->second;
    });

    // Private receiver primitives. The public methods are defined by
    // process.kex and call these category-qualified identities.
    defineIntrinsic("Process::send", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::unit();
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::unit();
        p->scheduler->send(p->pid, args[1]);
        return Value::unit();
    });

    defineIntrinsic("Process::sendFrom", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::unit();
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::unit();
        auto sender = Value::process(m_scheduler->currentProcessId(), m_scheduler.get());
        p->scheduler->send(p->pid, Value::tuple({std::move(sender), args[1]}));
        return Value::unit();
    });

    // pid.link()/pid.unlink() — links the CALLING process (whichever
    // process is currently running when this is invoked, not necessarily
    // the receiver's spawner) to the receiver pid. Passive bookkeeping
    // only — see Scheduler::link's doc comment for why this deliberately
    // doesn't carry BEAM's signal-propagation semantics.
    defineIntrinsic("Process::link", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::unit();
        p->scheduler->link(p->pid);
        return Value::unit();
    });
    defineIntrinsic("Process::unlink", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::unit();
        p->scheduler->unlink(p->pid);
        return Value::unit();
    });

    // pid.alive?() — true until that process's fiber has finished (whether
    // by a normal return or an uncaught exception caught by its own
    // fiber's outer handler — see Scheduler::spawn/runToCompletion).
    defineIntrinsic("Process::alive?", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::boolean(false);
        return Value::boolean(p->scheduler->isAlive(p->pid));
    });

    defineModule("Task");

    // Task.start { block } — `block` arrives here as an already-evaluated
    // zero-arg FunctionValue (the `{ ... }` block, per MethodCall's "block
    // as last positional arg" handling in eval()).
    // Walker-native scheduler fallback. Starting a child while a Kex wrapper's
    // shared evaluator frame is active can corrupt later process execution.
    defineDual("Task::start", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto pid = m_scheduler->startTask(args[0]);
        return Value::task(pid, m_scheduler.get());
    });

    // Walker-only public fallback: awaiting yields the scheduler while the
    // evaluator's environment stack is active, so a Kex wrapper cannot safely
    // own this call until evaluator environments are process-local. Returns
    // Ok(result)/Error(reason) — Error(:timeout) specifically on timeout,
    // matching docs/concurrency.md's documented `Result<T, TaskError>`
    // shape (TaskError isn't a distinct type here, just whatever reason
    // atom/value ends up in Error(...) — no separate error ADT to keep
    // this from growing beyond what the interpreter needs).
    NativeFunc await = [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(Value::string("not a task"));
        auto* t = std::get_if<TaskValue>(&args[0]->data);
        if (!t) return Value::error(Value::string("not a task"));

        std::optional<int64_t> timeoutMs;
        if (args.size() > 1) {
            if (auto* iv = std::get_if<IntValue>(&args[1]->data)) timeoutMs = iv->value;
        }

        auto msg = t->scheduler->awaitTaskMessage(t->pid, timeoutMs);
        if (!msg) return Value::error(Value::atom("timeout"));

        auto* tup = std::get_if<TupleValue>(&(*msg)->data);
        if (!tup || tup->elements.size() != 2) {
            return Value::error(Value::string("malformed task result"));
        }
        auto* tag = std::get_if<AtomValue>(&tup->elements[0]->data);
        if (tag && tag->name == "task_done") {
            return Value::ok(tup->elements[1]);
        }
        return Value::error(tup->elements[1]);
    };
    definePublic("await", await);

    // The source declaration still calls the qualified identity (used by
    // direct intrinsic calls and kept ABI-aligned with BEAM), while ordinary
    // walker receiver dispatch intentionally selects the bare fallback above.
    defineIntrinsic("Process::await", std::move(await));

    // Task.awaitAll([tasks]) — awaits each task in order, no timeout
    // (matches Task::start's counterpart having no bulk-timeout variant in
    // docs/concurrency.md; call .await(timeout: N) per-task first via
    // Task.awaitAll([...]) is not a thing — this is the simple sequential
    // form). Returns a list of Ok/Error results, same shape as `await`.
    // Matches the Kex-facing name in the BEAM backend —
    // both dispatch to the same surface syntax, camelCase like every other
    // multi-word Kex builtin (was briefly registered as "await_all" here,
    // the one snake_case outlier — fixed to match).
    defineDual("Task::awaitAll", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* lst = std::get_if<ListValue>(&args[0]->data);
        if (!lst) return Value::list({});

        std::vector<ValuePtr> results;
        for (const auto& taskVal : lst->elements) {
            auto* t = std::get_if<TaskValue>(&taskVal->data);
            if (!t) {
                results.push_back(Value::error(Value::string("not a task")));
                continue;
            }
            auto msg = t->scheduler->awaitTaskMessage(t->pid, std::nullopt);
            if (!msg) {
                results.push_back(Value::error(Value::atom("timeout")));
                continue;
            }
            auto* tup = std::get_if<TupleValue>(&(*msg)->data);
            if (!tup || tup->elements.size() != 2) {
                results.push_back(Value::error(Value::string("malformed task result")));
                continue;
            }
            auto* tag = std::get_if<AtomValue>(&tup->elements[0]->data);
            if (tag && tag->name == "task_done") {
                results.push_back(Value::ok(tup->elements[1]));
            } else {
                results.push_back(Value::error(tup->elements[1]));
            }
        }
        return Value::list(std::move(results));
    });

    defineModule("Supervisor");

    // worker { startFn() } — wraps a zero-arg block (expected to call
    // `spawn` and return the child's pid, matching
    // examples/beam/proc_supervisor.kex's `worker { startCounter("A") }`)
    // into a spec Supervisor.start can both call now (to start it) and
    // recall later (to restart it, from the exact same start function).
    definePublic("worker", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        return Value::tuple({Value::atom("worker"), args[0]});
    });

    // Supervisor.start(restart: :only_crashed) do [worker { ... }, ...] end
    // — see Scheduler::startSupervisor for the actual poll/restart loop.
    // args[0] is the do-block (a deferred zero-arg FunctionValue evaluating
    // to the list of worker specs); args[1], if present, is the `restart:`
    // atom (named args land positionally-appended here — see `await`'s
    // comment on why). Only :only_crashed is supported — anything else is
    // a clear Error(...) pointing at the BEAM backend instead of a silent
    // wrong behavior.
    definePublic("Supervisor::start", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) {
            return Value::error(Value::string("Supervisor.start requires a do...end block"));
        }
        auto* specsBlockFn = std::get_if<FunctionValue>(&args[0]->data);
        if (!specsBlockFn || !specsBlockFn->native) {
            return Value::error(Value::string("Supervisor.start requires a do...end block"));
        }

        std::string strategy = "only_crashed";
        if (args.size() > 1) {
            if (auto* av = std::get_if<AtomValue>(&args[1]->data)) strategy = av->name;
        }
        if (strategy != "only_crashed") {
            return Value::error(Value::string(
                "Supervisor restart strategy :" + strategy + " isn't supported by the interpreter — "
                "only :only_crashed is; use the BEAM backend (kex -R) for :all/:crashed_and_newer."));
        }

        auto specsVal = specsBlockFn->native({});
        auto* specsList = std::get_if<ListValue>(&specsVal->data);
        if (!specsList) {
            return Value::error(Value::string("Supervisor.start's block must evaluate to a list of worker specs"));
        }

        std::vector<ValuePtr> childBlocks;
        for (const auto& spec : specsList->elements) {
            auto* tup = std::get_if<TupleValue>(&spec->data);
            if (tup && tup->elements.size() == 2) {
                childBlocks.push_back(tup->elements[1]);
            }
        }

        auto supPid = m_scheduler->startSupervisor(std::move(childBlocks));
        return Value::ok(Value::process(supPid, m_scheduler.get()));
    });
}

} // namespace kex::interpreter
