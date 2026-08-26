#!/usr/bin/env bash
# Regenerate the docs site: every released tag plus the current (unreleased)
# checkout, for the prelude/stdlib and for tey.
#
#   tools/build-docs.sh [output-dir]
#
# output-dir defaults to ../kdocs relative to the repo root — the checkout
# that is published to docs.kex.run. Each released tag is built from a
# temporary git worktree; the unreleased build uses the working tree as-is.
# Builds run through the CURRENT toolchain — `tey docs` is a built-in of
# this checkout's Tey, and docgen parses old sources with the current
# compiler, so files that no longer parse are skipped with a warning rather
# than failing the release.
#
# Environment:
#   SKIP_TAGS=1   build only the unreleased checkout (used on main pushes,
#                 where the released tags have not changed)
#   BASE_URL      the site base URL for sitemap/robots (docgen --base-url)
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel)"
OUT="${1:-$(cd "$ROOT/.." && pwd)/kdocs}"
TEY_RUN="$ROOT/tey-run"
WORKTREES="$ROOT/.cache/docgen-tags"

if [ ! -x "$TEY_RUN" ]; then
  echo "build-all: no ./tey-run — run: make build-tey" >&2
  exit 1
fi

mkdir -p "$OUT" "$WORKTREES"

cleanup() {
  git -C "$ROOT" worktree list --porcelain | grep "^worktree $WORKTREES/" | cut -d' ' -f2- | while read -r wt; do
    git -C "$ROOT" worktree remove --force "$wt" 2>/dev/null || true
  done
}
trap cleanup EXIT

# build_docs <source-dir> <package> <label> [version]
#
# The version passed is the PACKAGE's own version story: prelude versions are
# Kex releases (the stdlib ships with the compiler), tey versions are read
# from that tag's tey/package.kex. The unreleased tey build passes no version
# and lets docgen default it from tey/package.kex in the CWD.
build_docs() {
  local source="$1" package="$2" label="$3" version="${4:-}"
  if [ ! -d "$source" ]; then
    echo "build-docs: skip $package (no $source)"
    return 0
  fi
  local args=(docs build --source "$source" --out "$OUT" --package "$package" --label "$label")
  if [ -n "${BASE_URL:-}" ]; then
    args+=(--base-url "$BASE_URL")
  fi
  if [ -n "$version" ]; then
    args+=(--release "$version")
  fi
  echo "build-docs: $package $version"
  "$TEY_RUN" "${args[@]}" || \
    echo "build-docs: WARNING $package $version failed — keeping previous output" >&2
}

# tey's own version as declared by a checkout's tey/package.kex.
tey_version() {
  sed -n 's/^ *version("\([^"]*\)").*/\1/p' "$1/tey/package.kex" | head -1
}

if [ -z "${SKIP_TAGS:-}" ]; then
  for tag in $(git -C "$ROOT" tag --sort=v:refname); do
    version="${tag#v}"
    wt="$WORKTREES/$version"
    git -C "$ROOT" worktree add --detach --force "$wt" "$tag" >/dev/null 2>&1 || {
      echo "build-docs: WARNING cannot worktree $tag — skipped" >&2
      continue
    }
    build_docs "$wt/src/stdlib" prelude "Standard Library" "$version"
    build_docs "$wt/tey/src" tey "Tey" "$(tey_version "$wt")"
    git -C "$ROOT" worktree remove --force "$wt" >/dev/null 2>&1 || true
  done
fi

# The unreleased builds. Prelude's version is the compiler's (the working
# tree's package.kex tracks it). Tey's comes from its own manifest via
# docgen's default, so that build runs from inside tey/ — but as a distinct
# "-dev" release, so an unreleased build cannot overwrite the released docs
# of the same version number.
build_docs "$ROOT/src/stdlib" prelude "Standard Library"
(cd "$ROOT/tey" && "$TEY_RUN" docs build --out "$OUT" \
  ${BASE_URL:+--base-url "$BASE_URL"} \
  --release "$(tey_version "$ROOT")-dev") || \
  echo "build-docs: WARNING tey unreleased failed — keeping previous output" >&2

# Static site assets docgen does not own — the favicon, the kexhq GitHub org
# icon. Copied here rather than generated, so docgen stays a documentation
# generator and the branding lives in the output.
if [ -f "$ROOT/tools/docs-assets/icon.png" ]; then
  cp "$ROOT/tools/docs-assets/icon.png" "$OUT/icon.png"
fi

echo "build-docs: done -> $OUT"
