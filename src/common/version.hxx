#pragma once

#include <string>

namespace kex {

// The one place the toolchain's version is written down. Everything that
// reports a version derives it from here: `kex --version`, the REPL banner,
// and `Kex.Kernel.VERSION` inside Kex programs — which must agree, since the
// whole point of the last one is answering "what am I actually running on?".
constexpr int kVersionMajor = 0;
constexpr int kVersionMinor = 3;
constexpr int kVersionPatch = 0;

// The git commit this build came from, injected by CMake at configure time.
// Empty when built from a source archive rather than a checkout — which is a
// normal state, not an error, and is why the Kex-facing form is an Optional.
#ifdef KEX_GIT_REVISION
constexpr auto kGitRevision = KEX_GIT_REVISION;
#else
constexpr auto kGitRevision = "";
#endif

// "0.3.0" — the version alone.
inline auto versionNumber() -> const std::string& {
    static const std::string value = std::to_string(kVersionMajor) + "." +
                                     std::to_string(kVersionMinor) + "." +
                                     std::to_string(kVersionPatch);
    return value;
}

// "0.3.0 (a1b2c3d)", or just "0.3.0" when the revision is unknown. This is
// what a human sees.
inline auto versionString() -> const std::string& {
    static const std::string value =
        *kGitRevision ? versionNumber() + " (" + kGitRevision + ")"
                      : versionNumber();
    return value;
}

} // namespace kex
