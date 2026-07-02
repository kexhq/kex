#include "scheduler.hxx"
#include "environment.hxx"
#include "evaluator.hxx"

// std::this_thread::sleep_until — a plain blocking sleep on the single OS
// thread this scheduler already owns, not thread *management* (no new
// thread is created). See driveUntilFinished.
#include <thread>

#ifndef __EMSCRIPTEN__
#include <boost/context/detail/exception.hpp>
#endif

namespace kex::interpreter {

namespace {
// A process's fiber can be destroyed while still suspended mid-execution —
// e.g. a `loop { receive ... }` server process that's still blocked in
// `receive` when its Evaluator goes out of scope (matches
// examples/proc_ping.kex: nothing ever explicitly kills pingServer). On the
// native (Boost.Context) backend, destroying a not-yet-finished fiber makes
// Boost.Context resume it one last time specifically to throw
// forced_unwind at its suspension point, letting it unwind its C++ stack
// normally (running destructors) before freeing the stack. That exception
// MUST propagate all the way back out of the entry function uncaught —
// swallowing it in a `catch (...)` (as both fiber entry points below
// otherwise would) corrupts Boost.Context's internal state and crashes
// later, unrelated fiber operations. See docs/fiber-process-plan.md.
#ifndef __EMSCRIPTEN__
auto rethrowIfForcedUnwind() -> void {
    try {
        throw;
    } catch (const boost::context::detail::forced_unwind&) {
        throw;
    } catch (...) {
        // fall through — caller's own catch(...) handles everything else
    }
}
#else
auto rethrowIfForcedUnwind() -> void {}
#endif
}

Scheduler::Scheduler(Evaluator& evaluator) : m_evaluator(evaluator) {}

auto Scheduler::resumeProcess(ProcessId id) -> void {
    auto it = m_processes.find(id);
    if (it == m_processes.end() || it->second->finished) return;
    auto& proc = *it->second;

    ProcessId prevCurrent = m_current;
    m_current = id;
    // Restore this process's own m_env cursor (not necessarily its root
    // scope — see Process::savedEnv) before handing it the C++ stack back.
    m_evaluator.m_env = proc.savedEnv;

    proc.fiber->resume();

    // Wherever this process's execution left m_env (its root scope if it
    // finished, or some nested scope if it yielded mid-expression) is
    // exactly what the next resume() of THIS process needs to see again.
    proc.savedEnv = m_evaluator.m_env;
    m_current = prevCurrent;
}

auto Scheduler::fireExpiredTimeouts() -> void {
    auto now = std::chrono::steady_clock::now();
    while (!m_timeouts.empty() && m_timeouts.begin()->first <= now) {
        auto entry = m_timeouts.begin()->second;
        m_timeouts.erase(m_timeouts.begin());

        auto pit = m_processes.find(entry.pid);
        if (pit == m_processes.end() || pit->second->finished) continue;
        auto& proc = *pit->second;
        // Stale entry from a receive call this process has since moved past
        // (e.g. woken by a message, then entered a fresh receive before
        // this old deadline got here) — discard silently, matches BEAM
        // never firing a superseded `after`.
        if (!proc.blockedOnReceive || proc.waitGeneration != entry.generation) continue;

        proc.blockedOnReceive = false;
        proc.wokeByTimeout = true;
        m_ready.push_back(entry.pid);
    }
}

auto Scheduler::driveUntilFinished(ProcessId target) -> ValuePtr {
    while (true) {
        auto it = m_processes.find(target);
        if (it == m_processes.end() || it->second->finished) break;

        fireExpiredTimeouts();

        if (m_ready.empty()) {
            if (m_timeouts.empty()) {
                // Nothing runnable, nothing pending — a real deadlock: some
                // process in the chain is blocked on a receive nothing will
                // ever satisfy.
                throw RuntimeError("deadlock: no process is runnable", SourceLocation{});
            }
            // Nothing runnable *right now*, but some process's `receive
            // timeout:` deadline hasn't arrived yet. This is a plain
            // blocking sleep on the one OS thread we already have — not
            // "managing threads" in the sense this design deliberately
            // avoids (no new thread, no lock) — acceptable for native
            // script/REPL execution; a wasm/browser embed drives via
            // step() instead (see docs/fiber-process-plan.md §2) and would
            // never reach this branch that way.
            std::this_thread::sleep_until(m_timeouts.begin()->first);
            continue;
        }

        ProcessId next = m_ready.front();
        m_ready.pop_front();
        resumeProcess(next);
    }
    auto it = m_processes.find(target);
    return it != m_processes.end() ? it->second->exitValue : Value::none();
}

auto Scheduler::runToCompletion(std::function<ValuePtr()> body, std::shared_ptr<Environment> env) -> ValuePtr {
    ProcessId id = m_nextId++;
    auto proc = std::make_unique<Process>();
    proc->id = id;
    proc->env = env;
    proc->savedEnv = env;
    Process* procPtr = proc.get();

    proc->fiber = std::make_unique<Fiber>([procPtr, body = std::move(body)]() mutable {
        try {
            procPtr->exitValue = body();
        } catch (const ReturnException& ret) {
            procPtr->exitValue = ret.value();
        } catch (...) {
            rethrowIfForcedUnwind();
            // Unlike spawn()'s child processes (see below), the root
            // process's error must reach Evaluator::execute()'s caller —
            // otherwise every runtime error in top-level code (not just
            // process-related code) would be silently swallowed instead of
            // reported. Can't just rethrow here — an exception can't safely
            // cross the fiber stack-switch boundary — so stash it and let
            // runToCompletion rethrow it below, back on the caller's own
            // (non-fiber) stack.
            procPtr->error = std::current_exception();
        }
        procPtr->finished = true;
    });

    m_processes.emplace(id, std::move(proc));
    m_ready.push_back(id);

    auto result = driveUntilFinished(id);
    if (auto it = m_processes.find(id); it != m_processes.end() && it->second->error) {
        std::rethrow_exception(it->second->error);
    }
    return result;
}

auto Scheduler::spawn(const std::vector<ast::ExprPtr>& body, std::shared_ptr<Environment> closureEnv) -> ProcessId {
    ProcessId id = m_nextId++;
    auto proc = std::make_unique<Process>();
    proc->id = id;
    // A genuine child scope, not a copy — bindings made inside the new
    // process must never leak into the parent or siblings (same
    // closure-capture convention LambdaValue already uses).
    proc->env = std::make_shared<Environment>(closureEnv);
    proc->savedEnv = proc->env;
    Process* procPtr = proc.get();
    Evaluator* evalPtr = &m_evaluator;
    const std::vector<ast::ExprPtr>* bodyPtr = &body;

    proc->fiber = std::make_unique<Fiber>([evalPtr, procPtr, bodyPtr]() {
        try {
            procPtr->exitValue = evalPtr->evalBody(*bodyPtr);
        } catch (const ReturnException& ret) {
            procPtr->exitValue = ret.value();
        } catch (...) {
            rethrowIfForcedUnwind();
            procPtr->exitValue = Value::none();
        }
        procPtr->finished = true;
    });

    m_processes.emplace(id, std::move(proc));
    m_ready.push_back(id);
    return id;
}

auto Scheduler::send(ProcessId target, ValuePtr payload) -> bool {
    auto it = m_processes.find(target);
    if (it == m_processes.end() || it->second->finished) return false;
    auto& proc = *it->second;

    auto sender = Value::process(m_current, this);
    auto wrapped = Value::tuple({std::move(payload), std::move(sender)});
    proc.mailbox.push_back(std::move(wrapped));

    if (proc.blockedOnReceive) {
        proc.blockedOnReceive = false;
        m_ready.push_back(target);
    }
    return true;
}

auto Scheduler::blockingReceive(const ast::ReceiveExpr& expr) -> ValuePtr {
    auto it = m_processes.find(m_current);
    // Only reachable from inside a running process's fiber, so this should
    // always exist — defensive fallback just in case.
    if (it == m_processes.end()) return Value::none();
    auto& proc = *it->second;

    // Evaluated once, up front — matches BEAM's `after` semantics, where
    // the timeout is measured from entering receive, not restarted each
    // time a non-matching message arrives. A local, not Process state: it
    // only needs to survive across this call's own yields, which plain
    // local variables already do (the whole C++ stack, including locals,
    // is preserved across a fiber yield/resume — that's the entire point
    // of using real fibers instead of a rewritten/CPS'd evaluator).
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (expr.timeout) {
        auto timeoutVal = m_evaluator.eval(**expr.timeout);
        if (auto* iv = std::get_if<IntValue>(&timeoutVal->data)) {
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(iv->value);
        }
        // Anything else (e.g. an `:infinity` atom) — no deadline, blocks
        // forever, same as `after` being absent entirely.
    }
    uint64_t myGeneration = ++proc.waitGeneration;
    bool timeoutRegistered = false;

    while (true) {
        for (size_t i = 0; i < proc.mailbox.size(); i++) {
            auto* tup = std::get_if<TupleValue>(&proc.mailbox[i]->data);
            ValuePtr payload = (tup && tup->elements.size() == 2) ? tup->elements[0] : proc.mailbox[i];
            ValuePtr sender = (tup && tup->elements.size() == 2) ? tup->elements[1] : Value::none();

            for (const auto& clause : expr.clauses) {
                m_evaluator.pushEnv();
                try {
                    if (expr.senderBinding) {
                        m_evaluator.m_env->define(*expr.senderBinding, sender);
                    }
                    bool matched = false;
                    for (const auto& pat : clause.patterns) {
                        if (m_evaluator.matchPattern(*pat, payload)) { matched = true; break; }
                    }
                    if (matched && clause.guard && *clause.guard) {
                        auto guardVal = m_evaluator.eval(**clause.guard);
                        if (!guardVal->isTrue()) matched = false;
                    }
                    if (matched) {
                        proc.mailbox.erase(proc.mailbox.begin() + static_cast<long>(i));
                        auto result = clause.body ? m_evaluator.eval(*clause.body) : Value::none();
                        m_evaluator.popEnv();
                        return result;
                    }
                } catch (...) {
                    m_evaluator.popEnv();
                    throw;
                }
                m_evaluator.popEnv();
            }
        }

        // Nothing in the mailbox matched any clause — block until send()
        // wakes this process (or, if a deadline is set, until it passes),
        // then re-scan from scratch (a new message may not be the only
        // unmatched one).
        if (deadline && !timeoutRegistered) {
            m_timeouts.emplace(*deadline, TimeoutEntry{m_current, myGeneration});
            timeoutRegistered = true;
        }
        proc.blockedOnReceive = true;
        proc.wokeByTimeout = false;
        Fiber::yieldToScheduler();

        if (proc.wokeByTimeout) {
            proc.wokeByTimeout = false;
            return expr.afterBody ? m_evaluator.eval(**expr.afterBody) : Value::none();
        }
    }
}

auto Scheduler::isAlive(ProcessId id) const -> bool {
    auto it = m_processes.find(id);
    return it != m_processes.end() && !it->second->finished;
}

auto Scheduler::tickReduction() -> void {
    auto it = m_processes.find(m_current);
    if (it == m_processes.end()) return;
    auto& proc = *it->second;

    if (++proc.reductions < kReductionBudget) return;
    proc.reductions = 0;

    // Still runnable — just giving up this turn, unlike blockingReceive's
    // yield, so re-enqueue before yielding (nothing else will wake this
    // process the way send() wakes a receive-blocked one).
    m_ready.push_back(m_current);
    Fiber::yieldToScheduler();
}

} // namespace kex::interpreter
