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

auto hostNanos() -> int64_t {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // namespace

// The clock boundary. Deliberately few primitives: everything else the
// Time/Date/DateTime stdlib does — civil conversion, formatting, parsing,
// arithmetic — is pure Kex, so the interpreter and BEAM cannot drift on
// calendar behavior. Only "what time is it" and "what is this machine's UTC
// offset" genuinely need the host. Replacing the clock in a test is not done
// here: `Time.Clock` is a capability, substituted with `with`.
auto Evaluator::registerTimeBuiltins() -> void {
    // Nanoseconds since the Unix epoch, UTC. Nanosecond resolution is what
    // the record carries; the host clock may be coarser.
    //
    // The host clock. `Time.Clock`'s default body reads it, so every reading
    // in the language (Time.now, Date.today, DateTime.utcNow) comes through
    // here unless that capability is replaced.
    defineIntrinsic("Time::nowNanos", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::integer(hostNanos());
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
