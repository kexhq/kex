#include "../evaluator.hxx"

#ifndef __EMSCRIPTEN__
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <random>
#include <thread>

namespace {

#ifndef __EMSCRIPTEN__
// The child `Process.stream` is currently waiting on, for the signal handler
// to forward to. A handler may only touch a `volatile sig_atomic_t`.
volatile sig_atomic_t g_streamedChild = -1;

extern "C" void forwardSignalToStreamedChild(int sig) {
    if (g_streamedChild > 0) kill(static_cast<pid_t>(g_streamedChild), sig);
}

// Forward SIGINT/SIGTERM/SIGHUP/SIGQUIT to `child` for as long as this object
// lives, restoring the previous dispositions afterwards. Without it a signal
// that takes the interpreter down leaves the grandchild running: `tey run`
// exited on Ctrl+C while the server it started kept serving and kept its port.
//
// With `group`, the target is the child's whole process group (`-child`):
// timed `Process.run` children lead their own group (see below), and a
// terminal signal reaches only the interpreter's group, so the subtree
// would otherwise miss the Ctrl+C it used to receive directly.
class SignalForwarder {
public:
    explicit SignalForwarder(pid_t child, bool group = false) {
        g_streamedChild = group ? -child : child;
        struct sigaction forward {};
        forward.sa_handler = forwardSignalToStreamedChild;
        sigemptyset(&forward.sa_mask);
        forward.sa_flags = 0; // no SA_RESTART: waitpid must return EINTR
        sigaction(SIGINT, &forward, &m_int);
        sigaction(SIGTERM, &forward, &m_term);
        sigaction(SIGHUP, &forward, &m_hup);
        sigaction(SIGQUIT, &forward, &m_quit);
    }
    ~SignalForwarder() {
        sigaction(SIGINT, &m_int, nullptr);
        sigaction(SIGTERM, &m_term, nullptr);
        sigaction(SIGHUP, &m_hup, nullptr);
        sigaction(SIGQUIT, &m_quit, nullptr);
        g_streamedChild = -1;
    }
    SignalForwarder(const SignalForwarder&) = delete;
    auto operator=(const SignalForwarder&) -> SignalForwarder& = delete;

private:
    struct sigaction m_int {}, m_term {}, m_hup {}, m_quit {};
};

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

    defineIntrinsic("Retry::randomUnit", [](std::vector<ValuePtr>) -> ValuePtr {
        std::random_device source;
        std::uniform_real_distribution<double> distribution(0.0, 1.0);
        return Value::floating(distribution(source));
    });

    defineIntrinsic("Task::sleep", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto* duration = std::get_if<RecordValue>(&args[0]->data);
        if (!duration || duration->typeName != "Duration") return Value::unit();
        auto found = duration->fields.find("seconds");
        if (found == duration->fields.end()) return Value::unit();
        double seconds = 0.0;
        if (auto* value = std::get_if<FloatValue>(&found->second->data)) seconds = value->value;
        else if (auto* value = std::get_if<IntValue>(&found->second->data)) seconds = value->value;
        if (seconds > 0.0)
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        return Value::unit();
    });

    // Pre-register the namespace placeholder so `Process.self` resolves via
    // the ModuleValue namespace-dispatch branch in eval() (ast::MethodCall),
    // the same convention used by the remaining public-native namespaces.
    defineModule("Process");

    defineIntrinsic("Process::run", [](std::vector<ValuePtr> args) -> ValuePtr {
#ifdef __EMSCRIPTEN__
        return Value::error(Value::string("process execution is unavailable in wasm"));
#else
        if (args.size() != 2 && args.size() != 3)
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

        // Optional third argument: milliseconds before the child is killed.
        std::optional<int64_t> timeoutMs;
        if (args.size() == 3) {
            const auto* limit = std::get_if<IntValue>(&args[2]->data);
            if (!limit)
                return Value::error(Value::string(
                    "Process.run timeout must be an integer number of milliseconds"));
            timeoutMs = limit->value < 0 ? 0 : limit->value;
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
            if (timeoutMs) {
                // Lead a fresh process group, so the timeout kill below can
                // take the whole subtree: a child that already forked (a
                // shell running `sleep`) would otherwise leave the
                // grandchild holding the pipes open, and the EOF the drain
                // waits for would never arrive. Untimed runs keep the
                // historical behaviour of sharing the interpreter's group.
                setpgid(0, 0);
            }
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
        if (timeoutMs) {
            // The parent-side half of the setpgid above: whichever runs
            // first wins, and a group kill later can only name this child's
            // own group — never the interpreter's. Ignored on failure; the
            // child's own pre-exec call is the one that counts.
            setpgid(child, child);
        }

        std::string stdoutText;
        std::string stderrText;
        int status = 0;
        {
            // Installed BEFORE the drain: a Ctrl+C while the child is still
            // writing has to reach it too, not only one that arrives during
            // the wait.
            SignalForwarder forwarding(child, timeoutMs.has_value());
            if (!timeoutMs) {
                drainPipes(outPipe[0], stdoutText, errPipe[0], stderrText);
                while (waitpid(child, &status, 0) < 0) {
                    if (errno == EINTR) continue;
                    kill(child, SIGTERM);
                    break;
                }
            } else {
                // Bounded wait: the pipes are polled until the deadline while
                // the child is watched with WNOHANG alongside. The deadline
                // bounds the WHOLE call — a child that already exited but
                // left its pipes held open (a grandchild inheriting them)
                // still reports a timeout rather than hanging the caller.
                using Clock = std::chrono::steady_clock;
                const auto deadline = Clock::now() +
                    std::chrono::milliseconds(*timeoutMs);
                struct pollfd fds[2] = {
                    {outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
                std::string* targets[2] = {&stdoutText, &stderrText};
                char buffer[4096];
                bool exited = false;
                bool expired = false;
                while (fds[0].fd >= 0 || fds[1].fd >= 0 || !exited) {
                    if (Clock::now() >= deadline) {
                        expired = true;
                        break;
                    }
                    const auto remain = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        deadline - Clock::now()).count();
                    int waitMs =
                        remain > 5000 ? 5000 : static_cast<int>(remain);
                    if (fds[0].fd < 0 && fds[1].fd < 0) {
                        // Both pipes are EOF but the child is not reaped yet:
                        // the reap races with the EOF by a hair, and a poll
                        // on two dead descriptors can only sleep, never
                        // report. Wait briefly and re-check the child.
                        waitMs = waitMs > 10 ? 10 : waitMs;
                    }
                    const int ready = poll(fds, 2, waitMs);
                    if (ready < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (ready > 0) {
                        for (int i = 0; i < 2; ++i) {
                            if (fds[i].fd < 0 || !fds[i].revents) continue;
                            const auto got =
                                read(fds[i].fd, buffer, sizeof(buffer));
                            if (got > 0) {
                                targets[i]->append(
                                    buffer, static_cast<std::size_t>(got));
                                continue;
                            }
                            if (got < 0 && errno == EINTR) continue;
                            // EOF, or an error there is no way to report
                            // per-stream.
                            close(fds[i].fd);
                            fds[i].fd = -1;
                        }
                    }
                    int statusNow = 0;
                    const pid_t watched = waitpid(child, &statusNow, WNOHANG);
                    if (watched == child) {
                        status = statusNow;
                        exited = true;
                    } else if (watched < 0 && errno != EINTR) {
                        break;
                    }
                }
                if (!exited) {
                    // Expired, or the child could no longer be watched: stop
                    // it either way so no stray process outlives the call.
                    // TERM first, KILL after a short grace for anything that
                    // ignores TERM. The negative pid names the child's own
                    // process group (see the setpgid above), so forked
                    // grandchildren die with it instead of holding the pipes
                    // open. The output gathered so far is discarded: a
                    // timeout answers an Error, never a partial result.
                    kill(-child, SIGTERM);
                    const auto graceEnd = Clock::now() +
                        std::chrono::milliseconds(100);
                    while (!exited && Clock::now() < graceEnd) {
                        const auto graceRemain = std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            graceEnd - Clock::now()).count();
                        struct pollfd graceFds[2] = {
                            {fds[0].fd, POLLIN, 0}, {fds[1].fd, POLLIN, 0}};
                        if (poll(graceFds, 2,
                                 static_cast<int>(graceRemain)) < 0 &&
                            errno != EINTR)
                            break;
                        int graceStatus = 0;
                        const pid_t reaped =
                            waitpid(child, &graceStatus, WNOHANG);
                        if (reaped == child) {
                            status = graceStatus;
                            exited = true;
                        } else if (reaped < 0 && errno != EINTR) {
                            break;
                        }
                    }
                    if (!exited) {
                        kill(-child, SIGKILL);
                        while (waitpid(child, &status, 0) < 0) {
                            if (errno == EINTR) continue;
                            break;
                        }
                    }
                }
                for (auto& descriptor : fds)
                    if (descriptor.fd >= 0) close(descriptor.fd);
                if (expired)
                    return Value::error(Value::string("timed out after " +
                        std::to_string(*timeoutMs) + "ms"));
            }
        }
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
        {
            SignalForwarder forwarding(child);
            while (waitpid(child, &status, 0) < 0) {
                if (errno == EINTR) continue;
                // Waiting failed for a reason retrying cannot fix; never
                // leave the child behind holding its port.
                kill(child, SIGTERM);
                break;
            }
        }
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
