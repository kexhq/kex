#include "../evaluator.hxx"
#include <chrono>
#include <ctime>

namespace kex::interpreter {

namespace {

auto toSeconds(const ValuePtr& val) -> std::time_t {
    if (auto* i = std::get_if<IntValue>(&val->data))
        return static_cast<std::time_t>(i->value);
    if (auto* f = std::get_if<FloatValue>(&val->data))
        return static_cast<std::time_t>(f->value);
    return 0;
}

// The clock is int64 nanoseconds. A Kex Integer is not: it promotes to
// arbitrary precision, so an instant outside 1677..2262 arrives here as a
// BigInt. Saturating rather than returning 0 keeps the failure ordered —
// clamping to the end of the representable range, not to 1970. Time.settable?
// rejects these before they reach here; this is the backstop.
auto toNanos(const ValuePtr& val) -> int64_t {
    if (auto* i = std::get_if<IntValue>(&val->data)) return i->value;
    if (auto* f = std::get_if<FloatValue>(&val->data))
        return static_cast<int64_t>(f->value);
    if (auto* bi = std::get_if<BigIntValue>(&val->data))
        return bi->value < 0 ? INT64_MIN : INT64_MAX;
    return 0;
}

auto hostNanos() -> int64_t {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// The test clock. Mirrors kex_intrinsic_time's persistent_term state.
//
//   Real       the host clock, the only state a normal program is ever in
//   Frozen     every reading returns exactly `value`
//   Travelling `value` nanoseconds are added to the host clock, so time
//              still advances but from a different starting point
enum class ClockMode { Real = 0, Frozen = 1, Travelling = 2 };

ClockMode g_clockMode = ClockMode::Real;
int64_t g_clockValue = 0;

} // namespace

// The clock boundary. Deliberately few primitives: everything else the
// Time/Date/DateTime stdlib does — civil conversion, formatting, parsing,
// arithmetic — is pure Kex, so the interpreter and BEAM cannot drift on
// calendar behavior. Only "what time is it", "what is this machine's UTC
// offset", and the test clock that overrides the first genuinely need the
// host.
auto Evaluator::registerTimeBuiltins() -> void {
    // Nanoseconds since the Unix epoch, UTC. Nanosecond resolution is what
    // the record carries; the host clock may be coarser.
    //
    // Every clock reading in the language funnels through here — Time.now,
    // Date.today, DateTime.utcNow — so controlling this one function controls
    // all of them.
    defineIntrinsic("Time::nowNanos", [](std::vector<ValuePtr>) -> ValuePtr {
        switch (g_clockMode) {
            case ClockMode::Frozen: return Value::integer(g_clockValue);
            case ClockMode::Travelling:
                return Value::integer(hostNanos() + g_clockValue);
            case ClockMode::Real: break;
        }
        return Value::integer(hostNanos());
    });

    // Pin the clock to an instant. Repeated readings return the same value.
    defineIntrinsic("Time::freezeAt", [](std::vector<ValuePtr> args) -> ValuePtr {
        g_clockMode = ClockMode::Frozen;
        g_clockValue = args.empty() ? 0 : toNanos(args[0]);
        return Value::unit();
    });

    // Move the clock to an instant and let it run from there.
    defineIntrinsic("Time::travelTo", [](std::vector<ValuePtr> args) -> ValuePtr {
        g_clockMode = ClockMode::Travelling;
        g_clockValue = (args.empty() ? 0 : toNanos(args[0])) - hostNanos();
        return Value::unit();
    });

    // Back to the host clock.
    defineIntrinsic("Time::release", [](std::vector<ValuePtr>) -> ValuePtr {
        g_clockMode = ClockMode::Real;
        g_clockValue = 0;
        return Value::unit();
    });

    defineIntrinsic("Time::clockMode", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::integer(static_cast<int64_t>(g_clockMode));
    });

    // Seconds east of UTC for the system zone AT the given instant, so a
    // date in July and one in January get their own DST answer rather than
    // today's offset being applied to every timestamp.
    defineIntrinsic("Time::localOffset",
                    [](std::vector<ValuePtr> args) -> ValuePtr {
        const std::time_t instant = args.empty() ? 0 : toSeconds(args[0]);
        std::tm localParts{};
        std::tm utcParts{};
        if (!localtime_r(&instant, &localParts) ||
            !gmtime_r(&instant, &utcParts))
            return Value::integer(0);
        // Compare the two civil renderings of the same instant rather than
        // reading tm_gmtoff, which is not portable.
        localParts.tm_isdst = 0;
        utcParts.tm_isdst = 0;
        const auto local = timegm(&localParts);
        const auto utc = timegm(&utcParts);
        return Value::integer(static_cast<int64_t>(local - utc));
    });
}

} // namespace kex::interpreter
