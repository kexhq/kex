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

} // namespace

// The clock boundary. Deliberately just two primitives: everything else the
// Time/Date/DateTime stdlib does — civil conversion, formatting, parsing,
// arithmetic — is pure Kex, so the interpreter and BEAM cannot drift on
// calendar behavior. Only "what time is it" and "what is this machine's UTC
// offset" genuinely need the host.
auto Evaluator::registerTimeBuiltins() -> void {
    // Nanoseconds since the Unix epoch, UTC. Nanosecond resolution is what
    // the record carries; the host clock may be coarser.
    defineIntrinsic("Time::nowNanos", [](std::vector<ValuePtr>) -> ValuePtr {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto nanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        return Value::integer(static_cast<int64_t>(nanos));
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
