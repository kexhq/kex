#!/usr/bin/env bash
# Generate the reference half of docs.kex.run: every released tag plus the
# current (unreleased) checkout, for the prelude/stdlib and for tey.
#
#   tools/build-docs.sh [output-dir]
#
# The site itself — landing page, guide, navigation, theme — is authored in
# Marqraft in the kexhq/docs repository, which builds and publishes it. This
# script only produces what that site mounts as generated pages: docgen's
# `--format fragments` output, one <package>/<version>/ per unit, plus the
# manifest.json Marqraft reads (tools/docsite). output-dir defaults to
# ../docs/generated — the `generated/` directory of a kexhq/docs checkout
# beside this one — so `marq dev ../docs` shows this checkout's reference.
#
# Each released tag is built from a temporary git worktree; the unreleased
# build uses the working tree as-is. Builds run through the CURRENT toolchain
# — `tey docs` is a built-in of this checkout's Tey, and docgen parses old
# sources with the current compiler, so files that no longer parse are
# skipped with a warning rather than failing the release.
#
# Environment:
#   SKIP_TAGS=1   build only the unreleased checkout (used on main pushes,
#                 where the released tags have not changed)
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel)"
OUT="${1:-$(cd "$ROOT/.." && pwd)/docs/generated}"
# Absolute, so later steps can change directory (the docsite tool runs from
# its own package) without re-resolving a caller-relative path.
case "$OUT" in
  /*) ;;
  *) OUT="$ROOT/$OUT" ;;
esac
TEY_RUN="$ROOT/tey-run"
WORKTREES="$ROOT/.cache/docgen-tags"

if [ ! -x "$TEY_RUN" ]; then
  echo "build-docs: no ./tey-run — run: make build-tey" >&2
  exit 1
fi

mkdir -p "$OUT" "$WORKTREES"

cleanup() {
  git -C "$ROOT" worktree list --porcelain | grep "^worktree $WORKTREES/" | cut -d' ' -f2- | while read -r wt; do
    git -C "$ROOT" worktree remove --force "$wt" 2>/dev/null || true
  done
}
trap cleanup EXIT

# Failures of the UNRELEASED builds are fatal (see the end of this script).
# Old tags are not: they are parsed by the CURRENT compiler, so a release
# whose sources no longer parse is expected and only skipped.
unreleased_failed=0

# build_docs <source-dir> <package> <label> <version> [extra docgen args...]
#
# The version is the PACKAGE's own version story, and is always passed: prelude
# versions are Kex releases (the stdlib ships with the compiler), tey versions
# come from that checkout's tey/package.kex. Docgen can read a name and version
# out of a package.kex in the CWD, but this script never leans on that — the
# stdlib is not a package, and a driver that states the identity cannot file
# pages under the wrong one when it is run from somewhere unexpected.
build_docs() {
  local source="$1" package="$2" label="$3" version="$4"
  shift 4
  if [ ! -d "$source" ]; then
    echo "build-docs: skip $package (no $source)"
    return 0
  fi
  local args=(docs build --source "$source" --out "$OUT" --package "$package" \
              --label "$label" --release "$version" --format fragments "$@")
  echo "build-docs: $package $version"
  "$TEY_RUN" "${args[@]}"
}

# A released tag that no longer parses is skipped with a warning; the site
# keeps whatever that version's pages already were.
build_tag_docs() {
  build_docs "$@" || \
    echo "build-docs: WARNING $2 $4 failed — keeping previous output" >&2
}

# Tey's reference links the names it shares with the stdlib (String,
# FS.Path, Result) into the stdlib reference of the same Kex.
prelude_links() {
  local model="$OUT/prelude/$1/model.json"
  [ -f "$model" ] && printf -- '--link-model\n%s\n' "$model"
}

# tey's own version as declared by a checkout's tey/package.kex.
tey_version() {
  sed -n 's/^ *version("\([^"]*\)").*/\1/p' "$1/tey/package.kex" | head -1
}

# The compiler's version, which is what a prelude/stdlib "release" means.
# The root VERSION file, not package.kex's own version(...) field: VERSION
# is what CMake and release.yml actually cut releases from (release.yml
# reads it, tags v$VERSION, and never touches package.kex), so it is the
# only one that has agreed with every released tag so far — package.kex's
# field drifts (kexhq/kex#305: it named 0.3.0 at the v0.3.4 tag, and
# 0.4.0-alpha at both v0.4.0-alpha.2 and v0.4.0-beta).
kex_version() {
  tr -d "[:space:]" < "$1/VERSION"
}

if [ -z "${SKIP_TAGS:-}" ]; then
  for tag in $(git -C "$ROOT" tag --sort=v:refname); do
    version="${tag#v}"
    wt="$WORKTREES/$version"
    git -C "$ROOT" worktree add --detach --force "$wt" "$tag" >/dev/null 2>&1 || {
      echo "build-docs: WARNING cannot worktree $tag — skipped" >&2
      continue
    }
    build_tag_docs "$wt/src/stdlib" prelude "Standard Library" "$version"
    mapfile -t links < <(prelude_links "$version")
    build_tag_docs "$wt/tey/src" tey "Tey" "$(tey_version "$wt")" "${links[@]}"
    git -C "$ROOT" worktree remove --force "$wt" >/dev/null 2>&1 || true
  done
fi

# The unreleased builds, from the working tree as it stands. Both carry a
# "-dev" release so an unreleased build can never overwrite the published
# pages of the same version number.
#
# These are FATAL. They are the ones that break when docgen or the
# sources they read regress, and an earlier version of this script masked
# every failure behind a warning — which is how a crash on startup, a
# swallowed --package and a broken HTML emitter all shipped unnoticed
# (kexhq/kex#287, #288, #289). A tag that no longer parses is still tolerated
# above; this is not.
dev_version="$(kex_version "$ROOT")-dev"
build_docs "$ROOT/src/stdlib" prelude "Standard Library" "$dev_version" || unreleased_failed=1
mapfile -t links < <(prelude_links "$dev_version")
build_docs "$ROOT/tey/src" tey "Tey" "$(tey_version "$ROOT")-dev" "${links[@]}" || \
  unreleased_failed=1

# The handover: manifest.json (which mounts Marqraft loads, which one each
# package's collection link opens) and a versions page per package. Fatal
# like the unreleased builds above.
(cd "$ROOT/tools/docsite" && "$TEY_RUN" run -- manifest "$OUT") || \
  unreleased_failed=1

if [ "$unreleased_failed" -ne 0 ]; then
  echo "build-docs: FAILED — the unreleased build did not complete" >&2
  exit 1
fi

echo "build-docs: done -> $OUT"
