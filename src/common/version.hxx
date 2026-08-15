#pragma once

#include <string>

namespace kex {

// The version lives in the repository's `VERSION` file — one line, e.g.
// `0.4.0` or `0.4.0-rc.1` — and CMake bakes it in here. Keeping it out of the
// source means bumping a release touches no C++, and everything else that
// needs the number (the release workflow's tag check, a container build, a
// download page) can read one plain file instead of parsing a header.
//
// Everything that reports a version derives it from these: `kex --version`,
// the REPL banner, and `Kex.Kernel.VERSION` inside Kex programs — which must
// agree, since the whole point of the last one is answering "what am I
// actually running on?".
//
// The fallbacks are deliberately 0.0.0: a build that did not go through our
// CMake has no version to claim, and an obviously-wrong number is better than
// a plausible one.
#ifdef KEX_VERSION_MAJOR
constexpr int kVersionMajor = KEX_VERSION_MAJOR;
#else
constexpr int kVersionMajor = 0;
#endif
#ifdef KEX_VERSION_MINOR
constexpr int kVersionMinor = KEX_VERSION_MINOR;
#else
constexpr int kVersionMinor = 0;
#endif
#ifdef KEX_VERSION_PATCH
constexpr int kVersionPatch = KEX_VERSION_PATCH;
#else
constexpr int kVersionPatch = 0;
#endif

// The release channel this build belongs to: "" for a stable release, or a
// pre-release identifier such as "prealpha.1", "beta.2", "rc.1" — the part of
// `VERSION` after the dash.
#ifdef KEX_VERSION_PRERELEASE
constexpr auto kVersionPreRelease = KEX_VERSION_PRERELEASE;
#else
constexpr auto kVersionPreRelease = "";
#endif

// The git commit this build came from, injected by CMake at configure time.
// Empty when built from a source archive rather than a checkout — which is a
// normal state, not an error, and is why the Kex-facing form is an Optional.
#ifdef KEX_GIT_REVISION
constexpr auto kGitRevision = KEX_GIT_REVISION;
#else
constexpr auto kGitRevision = "";
#endif

// "0.3.0", or "0.4.0-rc.1" on a pre-release build.
inline auto versionNumber() -> const std::string& {
    static const std::string value = [] {
        auto number = std::to_string(kVersionMajor) + "." +
                      std::to_string(kVersionMinor) + "." +
                      std::to_string(kVersionPatch);
        if (*kVersionPreRelease) number += std::string("-") + kVersionPreRelease;
        return number;
    }();
    return value;
}

// The day this binary was built, as YYYY-MM-DD (UTC), injected by CMake.
// Empty when the build system did not supply one.
//
// A date rather than a timestamp, and overridable with -DKEX_BUILD_DATE (the
// release workflow pins it from the tag's commit): two builds of the same
// source on the same day stay byte-identical, which a clock-precise stamp
// would break for no gain — "when was this built" is a question about days.
#ifdef KEX_BUILD_DATE
constexpr auto kBuildDate = KEX_BUILD_DATE;
#else
constexpr auto kBuildDate = "";
#endif

// "0.3.0 (a1b2c3d)", or just "0.3.0" when the revision is unknown. This is
// what a human sees.
inline auto versionString() -> const std::string& {
    static const std::string value =
        *kGitRevision ? versionNumber() + " (" + kGitRevision + ")"
                      : versionNumber();
    return value;
}

// "0.3.0 (a1b2c3d, built 2026-08-15)" — everything a bug report needs about
// which binary is running. The REPL banner and `--version` share it; the
// shorter versionString() stays for places with no room for a date.
inline auto versionStringWithBuild() -> const std::string& {
    static const std::string value = [] {
        if (!*kBuildDate) return versionString();
        if (!*kGitRevision) return versionNumber() + " (built " + kBuildDate + ")";
        return versionNumber() + " (" + kGitRevision + ", built " + kBuildDate + ")";
    }();
    return value;
}

} // namespace kex
