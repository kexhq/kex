#!/bin/sh
# Install Tey — Kex's package, compiler, runtime and standard-library
# manager — without Homebrew.
#
#   curl -fsSL https://raw.githubusercontent.com/kexhq/kex/main/install.sh | sh
#   wget -qO- https://raw.githubusercontent.com/kexhq/kex/main/install.sh | sh
#
# With options (also settable as environment variables):
#
#   curl -fsSL .../install.sh | sh -s -- --kex 0.4.0 --prefix ~/.local/share/tey/dist
#
#   --prefix DIR     where to install (default: ~/.local/share/tey/dist)
#   --kex VERSION    Kex toolchain to fetch: a version, or `latest` (default)
#   --no-kex         install Tey only, skip the Kex toolchain
#   --repo OWNER/REPO
#                    releases to install from, for forks and mirrors
#                    (default: kexhq/kex)
#   -h, --help       print this text and stop
#
# What it needs on PATH: `erl` (Erlang/OTP — this script only checks for it
# and tells you how to get it), `curl` or `wget`, `tar`, and `sha256sum` or
# `shasum`. Everything is verified against the `.sha256` published beside
# each release before anything is installed.
set -eu

REPO="${TEY_REPO:-kexhq/kex}"
KEX_VERSION="${KEX_VERSION:-latest}"
PREFIX="${PREFIX:-$HOME/.local/share/tey/dist}"
NO_KEX=0

usage() {
  sed -n '2,/^set -eu$/p' "$0" | sed -e '/^set -eu$/d' -e 's/^# //' -e 's/^#$//'
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

# Erlang runs Tey itself and every Kex it manages, so without it nothing
# below can even start. Installing it is one privileged, per-OS command too
# many for a script piped from the network — check, and say exactly what to
# run instead.
if ! have erl; then
  case "$(uname -s)" in
    Darwin) hint="brew install erlang" ;;
    Linux) hint="sudo apt-get install -y erlang-base  (or your distro's Erlang/OTP package)" ;;
    *) hint="install Erlang/OTP for your system (https://www.erlang.org/downloads)" ;;
  esac
  die "no \`erl\` on PATH — install Erlang/OTP first, then run this again:
  $hint"
fi

have curl || have wget || die "need \`curl\` or \`wget\` to download the release"
have tar || die "need \`tar\` to unpack the release"
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
cp -R "$tmp/tey-$tey_version/." "$PREFIX/" || die "could not copy Tey into $PREFIX"

# A login shell that cannot find `$PREFIX/bin` cannot run anything just
# installed. Say the exact line rather than editing dotfiles from a piped
# script — that edit is the user's to make, once, in the file they use.
case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *)
    echo ""
    echo "Add Tey to PATH (once):"
    echo "  export PATH=\"$PREFIX/bin:\$PATH\""
    ;;
esac

if [ "$NO_KEX" -eq 0 ]; then
  # Tey's own installer from here: it verifies the published checksum,
  # stages into `<version>.partial` and renames only a complete tree, and
  # records the selection. Reproducing that in shell is how the two drift.
  if [ "$KEX_VERSION" = "latest" ]; then
    "$PREFIX/bin/tey" kex install "$kex_version" || die "\`tey kex install $kex_version\` failed"
  else
    "$PREFIX/bin/tey" kex install "$KEX_VERSION" || die "\`tey kex install $KEX_VERSION\` failed"
  fi
fi

"$PREFIX/bin/tey" --version || die "installed Tey does not run"
echo "Done. Try: tey --help"
