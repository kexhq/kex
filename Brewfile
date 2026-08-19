# Kex build dependencies for macOS and Linux (Homebrew / linuxbrew).
# Install everything a native build needs with a single command:
#
#   brew bundle
#
# CMakeLists.txt is the source of truth; keep this list in lockstep with it.
# The macOS CI job installs via this file, so a missing dependency fails the
# macOS build (cmake's FATAL_ERROR on configure) — that is the drift gate.
# The wasm target's extra deps (emsdk 5.0.7, prebuilt GMP/PCRE2 under
# third_party/*-wasm) are not installable through brew — see those READMEs.

brew "cmake"
brew "gmp"
brew "pcre2"
brew "boost"
brew "erlang"
brew "readline"
brew "openssl" if OS.linux?
