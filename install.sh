#!/bin/sh
# Install Tey — Kex's package, compiler, runtime and standard-library
# manager — without Homebrew.
#
#   curl -fsSL https://raw.githubusercontent.com/kexhq/kex/main/install.sh | sh
#   wget -qO- https://raw.githubusercontent.com/kexhq/kex/main/install.sh | sh
#
# Options, what it needs, and what it changes: see `usage` below, or pass
# `--help` (which works through the pipe too: `... | sh -s -- --help`).
#
# Piped into `sh`, this script IS the shell's stdin, and the shell reads it
# a block (8 KB for dash) at a time. Anything run from here that reads stdin
# — `erl` does, and so do `tey` and `kex`, which start it — swallows the
# rest of the script, and the shell then fails on a half-read line
# ("Unterminated quoted string"). Every such command gets </dev/null.
set -eu

REPO="${TEY_REPO:-kexhq/kex}"
ORIGINAL_PATH="$PATH"
KEX_VERSION="${KEX_VERSION:-latest}"
PREFIX="${PREFIX:-$HOME/.local/share/tey/dist}"
NO_KEX=0

# A heredoc rather than the header comment read back from "$0": piped from
# curl, "$0" is `sh`, and there is no file to read.
usage() {
  cat <<'EOF'
Install Tey — Kex's package, compiler, runtime and standard-library
manager — without Homebrew.

  curl -fsSL https://raw.githubusercontent.com/kexhq/kex/main/install.sh | sh

With options (also settable as environment variables):

  curl -fsSL .../install.sh | sh -s -- --kex 0.4.0

  --prefix DIR     where to install Tey (default: ~/.local/share/tey/dist)
  --kex VERSION    Kex toolchain to fetch: a version, or `latest` (default)
  --no-kex         install Tey only, skip the Kex toolchain
  --repo OWNER/REPO
                   releases to install from, for forks and mirrors
                   (default: kexhq/kex)
  -h, --help       print this text and stop

Needs Erlang/OTP 27 or newer (`erl`), `curl` or `wget`, `tar`, and
`sha256sum` or `shasum`. On Linux the Kex binaries need glibc 2.38 or newer
(Ubuntu 24.04, Debian 13, Fedora 39 and later). Every download is verified
against the `.sha256` published beside it.

Running it again upgrades Tey and installs the requested Kex beside the ones
you already have; nothing outside the prefix and ~/.local/share/tey changes,
and no shell startup file is edited.
EOF
}

die() {
  echo "install.sh: $*" >&2
  exit 1
}

have() {
  command -v "$1" >/dev/null 2>&1
}

# Fetch a URL to stdout.
fetch() {
  if have curl; then
    curl -fsSL ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} "$1"
  else
    wget -q ${GITHUB_TOKEN:+--header="Authorization: Bearer $GITHUB_TOKEN"} -O- "$1"
  fi
}

# Fetch a URL into a file.
fetch_to() {
  if have curl; then
    curl -fsSL ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} -o "$2" "$1"
  else
    wget -q ${GITHUB_TOKEN:+--header="Authorization: Bearer $GITHUB_TOKEN"} -O "$2" "$1"
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
    --prefix=*) PREFIX="${1#--prefix=}"; shift ;;
    --kex) KEX_VERSION="${2:?--kex needs a version}"; shift 2 ;;
    --kex=*) KEX_VERSION="${1#--kex=}"; shift ;;
    --no-kex) NO_KEX=1; shift ;;
    --repo) REPO="${2:?--repo needs OWNER/REPO}"; shift 2 ;;
    --repo=*) REPO="${1#--repo=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option '$1'; see --help" ;;
  esac
done

case "$REPO" in
  */*) ;;
  *) die "--repo needs OWNER/REPO, got '$REPO'" ;;
esac

# Which system this is, as `<id> <major version>`, for the per-system
# commands below: "ubuntu 24", "debian 13", "fedora 44", "macos", or just
# the ID when there is no version to read.
system_id() {
  if [ "$(uname -s)" = Darwin ]; then
    echo macos
  elif [ -r /etc/os-release ]; then
    (. /etc/os-release; echo "${ID:-unknown} ${VERSION_ID%%.*}")
  else
    echo unknown
  fi
}
SYSTEM="$(system_id)"
SYSTEM_MAJOR="$(echo "$SYSTEM" | cut -s -d' ' -f2)"
# The package family, for commands whose package names a whole family shares
# (Linux Mint and Pop!_OS take Ubuntu's): the ID and everything ID_LIKE names.
SYSTEM_FAMILY="$(echo "$SYSTEM" | cut -d' ' -f1)"
if [ -r /etc/os-release ]; then
  SYSTEM_FAMILY="$SYSTEM_FAMILY $(. /etc/os-release; echo "${ID_LIKE:-}")"
fi

# Erlang runs Tey itself and every Kex it manages, so without it nothing
# below can even start. Installing it is one privileged, per-OS command too
# many for a script piped from the network — check, and say exactly what to
# run instead.
#
# Kex needs OTP 27 or newer, which rules out several distros' own packages
# (Ubuntu 24.04 ships 25, Fedora 44 ships 26), so a present-but-old Erlang
# gets the same advice as a missing one. Each command here was checked
# against that distro's repositories.
erlang_hint() {
  case "$SYSTEM" in
    macos) echo "  brew install erlang" ;;
    debian*)
      if [ "${SYSTEM_MAJOR:-0}" -ge 13 ] 2>/dev/null; then
        echo "  sudo apt install erlang-nox"
      else
        echo "  Debian ${SYSTEM_MAJOR:-?} ships an older Erlang; upgrade to Debian 13, or build OTP 27+ with asdf/mise"
      fi ;;
    ubuntu*)
      if [ "${SYSTEM_MAJOR:-0}" -ge 26 ] 2>/dev/null; then
        echo "  sudo apt install erlang-nox"
      else
        echo "  sudo add-apt-repository ppa:rabbitmq/rabbitmq-erlang"
        echo "  sudo apt install erlang-nox"
      fi ;;
    arch*|manjaro*|endeavouros*) echo "  sudo pacman -S erlang" ;;
    *) echo "  Erlang/OTP 27 or newer for your system: https://www.erlang.org/downloads (or asdf/mise)" ;;
  esac
}
if ! have erl; then
  die "Erlang is not installed (no \`erl\` on PATH). Kex runs on Erlang/OTP 27 or newer.
Install it with:
$(erlang_hint)
then run this installer again."
fi
otp_release="$(erl -noshell -eval 'io:format("~s",[erlang:system_info(otp_release)]),halt().' </dev/null 2>/dev/null || echo 0)"
if [ "$otp_release" -lt 27 ] 2>/dev/null; then
  die "the Erlang on PATH is OTP $otp_release ($(command -v erl)); Kex needs OTP 27 or newer.
Install a newer one with:
$(erlang_hint)
then run this installer again."
fi

# The Linux Kex binaries are built on Ubuntu 24.04 against glibc, so a musl
# system cannot load them at all and an older glibc fails with "GLIBC_2.38
# not found" on the first `kex`. Both are known before anything is
# downloaded, so say so now rather than after installing something unusable.
if [ "$(uname -s)" = Linux ]; then
  glibc="$(getconf GNU_LIBC_VERSION 2>/dev/null | cut -d' ' -f2)"
  if [ -z "$glibc" ]; then
    die "the Kex release binaries need glibc, and this system uses another C library (Alpine and other musl systems).
Use the container image instead (see docker/ in the repository), or build Kex from source."
  fi
  glibc_major="${glibc%%.*}"
  glibc_minor="$(echo "$glibc" | cut -d. -f2)"
  if [ "$glibc_major" -lt 2 ] || { [ "$glibc_major" -eq 2 ] && [ "$glibc_minor" -lt 38 ]; }; then
    die "this system has glibc $glibc; the Kex release binaries need glibc 2.38 or newer
(Ubuntu 24.04, Debian 13, Fedora 39 or later). Upgrade the OS, or build Kex from source."
  fi
fi

# The shared libraries the Kex binary loads. None of them is Tey's to
# install, so after installing Kex it is run once, and if the loader refuses,
# this is the one command to fix it.
libraries_hint() {
  case " $SYSTEM_FAMILY " in
    *" macos "*) echo "  brew install gmp pcre2 readline" ;;
    *" debian "*|*" ubuntu "*) echo "  sudo apt install libreadline8 libgmp10 libpcre2-8-0 libssl3" ;;
    *" fedora "*|*" rhel "*) echo "  sudo dnf install readline gmp pcre2 openssl-libs" ;;
    *" arch "*) echo "  sudo pacman -S readline gmp pcre2 openssl" ;;
    *) echo "  your system's packages for readline 8, GMP, PCRE2 (8-bit) and OpenSSL 3" ;;
  esac
}

have curl || have wget || die "need \`curl\` or \`wget\` to download the release"
have tar || die "need \`tar\` to unpack the release"
# Tey lists Kex releases and fetches every package with Git, so without it
# the first `tey kex install` fails — after Tey is already installed.
if ! have git; then
  case " $SYSTEM_FAMILY " in
    *" macos "*) git_hint="  xcode-select --install" ;;
    *" debian "*|*" ubuntu "*) git_hint="  sudo apt install git" ;;
    *" fedora "*|*" rhel "*) git_hint="  sudo dnf install git" ;;
    *" arch "*) git_hint="  sudo pacman -S git" ;;
    *) git_hint="  Git for your system: https://git-scm.com/downloads" ;;
  esac
  die "Git is not installed. Tey uses it to find Kex releases and fetch packages.
Install it with:
$git_hint
then run this installer again."
fi
if have sha256sum; then
  checksum_of() { sha256sum "$1" | cut -d' ' -f1; }
elif have shasum; then
  checksum_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
  die "need \`sha256sum\` or \`shasum\` to verify the release"
fi

tmp="$(mktemp -d /tmp/tey-install.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT INT TERM HUP

# Which release: `latest` answers the newest stable one, a version names its
# tag (`0.4.0` or `v0.4.0` — tags use both spellings). Either way the
# release's own asset list says which Tey archive to fetch, since Tey is
# versioned separately from Kex.
api="https://api.github.com/repos/$REPO/releases"
if [ "$KEX_VERSION" = "latest" ]; then
  fetch "$api/latest" > "$tmp/release.json" || die "could not read the latest release for $REPO"
else
  tag_short="$(echo "$KEX_VERSION" | sed 's/^v//')"
  if fetch "$api/tags/v$tag_short" > "$tmp/release.json" 2>/dev/null; then
    :
  elif fetch "$api/tags/$tag_short" > "$tmp/release.json" 2>/dev/null; then
    :
  else
    die "no Kex release '$KEX_VERSION' in $REPO"
  fi
fi

tag="$(grep -o '"tag_name": *"[^"]*"' "$tmp/release.json" | head -1 | sed 's/^"tag_name": *"//; s/"$//')"
tey_asset="$(grep -o '"name": *"tey-[^"]*\.tar\.gz"' "$tmp/release.json" | head -1 | sed 's/^"name": *"//; s/"$//')"
[ -n "$tag" ] || die "could not read the release tag for $REPO"
[ -n "$tey_asset" ] || die "$tag publishes no tey-*.tar.gz"
tey_version="$(echo "$tey_asset" | sed 's/^tey-//; s/\.tar\.gz$//')"
kex_version="$(echo "$tag" | sed 's/^v//')"
echo "Installing Tey $tey_version with Kex $kex_version to $PREFIX"

base="https://github.com/$REPO/releases/download/$tag"
fetch_to "$base/$tey_asset" "$tmp/tey.tar.gz" || die "could not download $base/$tey_asset"
fetch_to "$base/$tey_asset.sha256" "$tmp/tey.tar.gz.sha256" || die "could not download $base/$tey_asset.sha256"

expected="$(cut -d' ' -f1 < "$tmp/tey.tar.gz.sha256")"
actual="$(checksum_of "$tmp/tey.tar.gz")"
[ -n "$expected" ] && [ "$expected" = "$actual" ] || die "checksum mismatch for $tey_asset (release may still be uploading; try again shortly)"

tar -xzf "$tmp/tey.tar.gz" -C "$tmp" || die "could not unpack $tey_asset"
[ -x "$tmp/tey-$tey_version/bin/tey" ] || die "$tey_asset does not contain a Tey installation"
[ -x "$tmp/tey-$tey_version/bin/kex" ] || die "$tey_asset does not contain a Tey installation"
[ -d "$tmp/tey-$tey_version/lib/kex/tey/ebin" ] || die "$tey_asset does not contain a Tey installation"

mkdir -p "$PREFIX" || die "could not create $PREFIX"
# Replace Tey's runtime directory outright: copying over it would keep the
# modules an older Tey had and this one dropped. The directory is Tey's own;
# `bin/` is not, so there only the two launchers are overwritten.
rm -rf "$PREFIX/lib/kex/tey/ebin"
cp -R "$tmp/tey-$tey_version/." "$PREFIX/" || die "could not copy Tey into $PREFIX"

# Both directories a user needs on PATH: the prefix's `bin` (tey, and the kex
# dispatcher) and the Tey home's `bin` (programs `tey install` puts there).
# They go on this script's own PATH too, so the `tey` run below sees a
# working `kex` and does not print PATH advice of its own — the summary at
# the end says it once, in full.
TEY_HOME="${TEY_HOME:-$HOME/.local/share/tey}"
PATH="$PREFIX/bin:$TEY_HOME/bin:$PATH"
export PATH
# Tells Tey that this script checks whether the installed
# Kex can start, and reports it, so Tey should not say the same thing first.
TEY_INSTALL_SH=1
export TEY_INSTALL_SH

if [ "$NO_KEX" -eq 0 ]; then
  # Tey's own installer from here: it verifies the published checksum,
  # stages into `<version>.partial` and renames only a complete tree, and
  # records the selection. Reproducing that in shell is how the two drift.
  if [ "$KEX_VERSION" = "latest" ]; then
    "$PREFIX/bin/tey" kex install "$kex_version" </dev/null || die "\`tey kex install $kex_version\` failed"
  else
    "$PREFIX/bin/tey" kex install "$KEX_VERSION" </dev/null || die "\`tey kex install $KEX_VERSION\` failed"
  fi
fi

"$PREFIX/bin/tey" --version </dev/null >/dev/null || die "the installed Tey does not run"

# Installed is not the same as runnable: the Kex binary loads readline, GMP,
# PCRE2 and OpenSSL from the system, and "Done" followed by a loader error on
# the first `kex` is the worst way to find that out.
kex_error=""
if [ "$NO_KEX" -eq 0 ]; then
  if ! kex_error="$("$PREFIX/bin/kex" --version </dev/null 2>&1 >/dev/null)"; then
    [ -n "$kex_error" ] || kex_error="\`kex --version\` failed"
  else
    kex_error=""
  fi
  case "$kex_error" in
    *"GLIBC_"*|*"GLIBCXX_"*)
      # Nothing the user can install fixes this one; no steps would help.
      die "Kex is installed but cannot start on this system:
$(echo "$kex_error" | sed 's/^/  /')
The release binaries need glibc 2.38 or newer (Ubuntu 24.04, Debian 13,
Fedora 39 or later). Upgrade the OS, or build Kex from source."
      ;;
  esac
fi

# A login shell that cannot find these directories cannot run anything just
# installed. Say the exact line, for the file this user's shell reads, rather
# than editing dotfiles from a piped script — that edit is the user's to make.
# The `~` in `rc` is shown to the user, never expanded.
# shellcheck disable=SC2088
case "${SHELL:-}" in
  */zsh) rc="~/.zshrc"; path_line="export PATH=\"$PREFIX/bin:$TEY_HOME/bin:\$PATH\"" ;;
  */fish) rc="~/.config/fish/config.fish"; path_line="fish_add_path \"$PREFIX/bin\" \"$TEY_HOME/bin\"" ;;
  */bash) rc="~/.bashrc"; path_line="export PATH=\"$PREFIX/bin:$TEY_HOME/bin:\$PATH\"" ;;
  *) rc="your shell's startup file"; path_line="export PATH=\"$PREFIX/bin:$TEY_HOME/bin:\$PATH\"" ;;
esac

echo ""
if [ "$NO_KEX" -eq 1 ]; then
  echo "Installed Tey $tey_version (no Kex toolchain; add one with \`tey kex install\`)."
elif [ "$KEX_VERSION" = "latest" ]; then
  echo "Installed Tey $tey_version and Kex $kex_version."
else
  echo "Installed Tey $tey_version and Kex $(echo "$KEX_VERSION" | sed 's/^v//')."
fi

if [ -n "$kex_error" ]; then
  echo ""
  echo "Kex cannot start yet — a library it needs is missing:"
  echo "$kex_error" | sed 's/^/  /'
fi

# What the user's own shell will find, judged from the PATH they started
# with — not the one extended above.
found_tey="$(PATH="$ORIGINAL_PATH" command -v tey 2>/dev/null || true)"
if [ -n "$found_tey" ] && [ "$found_tey" != "$PREFIX/bin/tey" ]; then
  echo ""
  echo "Note: your shell currently finds another tey first: $found_tey"
  case "$found_tey" in
    */Cellar/*|/opt/homebrew/*|/home/linuxbrew/*) echo "That one is Homebrew's; remove it with \`brew uninstall tey\`, or put Tey's PATH line AFTER Homebrew's in $rc." ;;
    *) echo "Remove it, or make sure Tey's PATH line comes after whatever puts it on PATH in $rc." ;;
  esac
fi

# Everything left for the user to do, in order, numbered — so nobody fixes
# the first problem and never learns about the second.
check="kex --version"
[ "$NO_KEX" -eq 0 ] || check="tey --version"
on_path=1
for dir in "$PREFIX/bin" "$TEY_HOME/bin"; do
  case ":$ORIGINAL_PATH:" in
    *":$dir:"*) ;;
    *) on_path=0 ;;
  esac
done
step=0
echo ""
echo "Next steps:"
if [ -n "$kex_error" ]; then
  step=$((step + 1))
  echo "  $step. Install the libraries Kex needs:"
  libraries_hint | sed 's/^ */       /'
fi
if [ "$on_path" -eq 0 ]; then
  step=$((step + 1))
  echo "  $step. Add Tey to your PATH — put this line at the end of $rc:"
  echo "       $path_line"
  step=$((step + 1))
  echo "  $step. Open a new terminal and check:"
else
  step=$((step + 1))
  echo "  $step. Check:"
fi
echo "       $check"
echo ""
# Tey before 0.2.1 has no `upgrade` command; re-running this script is the
# way forward from those.
if "$PREFIX/bin/tey" help </dev/null 2>/dev/null | grep -qw upgrade; then
  echo "To upgrade later: \`tey upgrade\` for Tey, \`tey kex install latest\` for Kex."
else
  echo "To upgrade later, run this installer again."
fi

# Installed, but not usable yet: say so to scripts as well as people.
[ -z "$kex_error" ] || exit 1
