# Kex build dependencies for macOS and Linux (Homebrew / linuxbrew).
# A convenience for setting up a machine to hack on Kex itself:
#
#   brew bundle --no-upgrade
#
# --no-upgrade installs only what is missing; without it brew also upgrades
# every formula below that you already have.
#
# Nothing but a local dev setup consumes this file. People who just want to
# run Kex install it through tey (`brew install tey`, then `tey kex install`)
# and never see this list; ci.yml, release.yml and .github/actions/setup-tey
# keep their own explicit install lists, so this one is not machine-checked —
# CMakeLists.txt is the source of truth, and adding a dependency there means
# updating this list, the two workflows, and the Homebrew formula by hand.
#
# The wasm target's extra deps (emsdk 5.0.7, prebuilt GMP/PCRE2 under
# third_party/*-wasm) are not installable through brew — see those READMEs.

brew "cmake"
brew "gmp"
brew "pcre2"
brew "boost"
brew "erlang"
brew "readline"
brew "openssl" if OS.linux?
