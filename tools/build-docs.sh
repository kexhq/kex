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
# Absolute, so later steps can change directory (the docsite assembler runs
# from its own package) without re-resolving a caller-relative path.
case "$OUT" in
  /*) ;;
  *) OUT="$ROOT/$OUT" ;;
esac
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

# Failures of the UNRELEASED builds are fatal (see the end of this script).
# Old tags are not: they are parsed by the CURRENT compiler, so a release
# whose sources no longer parse is expected and only skipped.
unreleased_failed=0

# build_docs <source-dir> <package> <label> <version>
#
# The version is the PACKAGE's own version story, and is always passed: prelude
# versions are Kex releases (the stdlib ships with the compiler), tey versions
# come from that checkout's tey/package.kex. Docgen can read a name and version
# out of a package.kex in the CWD, but this script never leans on that — the
# stdlib is not a package, and a driver that states the identity cannot file
# pages under the wrong one when it is run from somewhere unexpected.
build_docs() {
  local source="$1" package="$2" label="$3" version="$4"
  if [ ! -d "$source" ]; then
    echo "build-docs: skip $package (no $source)"
    return 0
  fi
  local args=(docs build --source "$source" --out "$OUT" --package "$package" \
              --label "$label" --release "$version")
  if [ -n "${BASE_URL:-}" ]; then
    args+=(--base-url "$BASE_URL")
  fi
  echo "build-docs: $package $version"
  "$TEY_RUN" "${args[@]}"
}

# A released tag that no longer parses is skipped with a warning; the site
# keeps whatever that version's pages already were.
build_tag_docs() {
  build_docs "$@" || \
    echo "build-docs: WARNING $2 $4 failed — keeping previous output" >&2
}

# Static book content lives in kexhq/docs, versioned per release as
# guide/<version>/. CI checks that repo out and points GUIDE_SOURCE at the
# version being built; locally it defaults to this checkout's scratch tree.
# Whatever it names must be a flat tree of .md files — skipped when absent.
GUIDE_SOURCE="${GUIDE_SOURCE:-$ROOT/docs-src/guide}"

# build_prose <md-dir> <package> <label> <version>
#
# Hand-written Markdown rendered into the same chrome as the reference (see
# `tey docs prose`). A tree that is not there is skipped.
build_prose() {
  local source="$1" package="$2" label="$3" version="$4"
  if [ ! -d "$source" ]; then
    echo "build-docs: skip $package prose (no $source)"
    return 0
  fi
  local args=(docs prose --source "$source" --out "$OUT" --package "$package" \
              --label "$label" --release "$version")
  if [ -n "${BASE_URL:-}" ]; then
    args+=(--base-url "$BASE_URL")
  fi
  echo "build-docs: $package prose $version"
  "$TEY_RUN" "${args[@]}"
}

# tey's own version as declared by a checkout's tey/package.kex.
tey_version() {
  sed -n 's/^ *version("\([^"]*\)").*/\1/p' "$1/tey/package.kex" | head -1
}

# The compiler's version, which is what a prelude/stdlib "release" means.
kex_version() {
  sed -n 's/^ *version("\([^"]*\)").*/\1/p' "$1/package.kex" | head -1
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
    build_tag_docs "$wt/tey/src" tey "Tey" "$(tey_version "$wt")"
    # No guide for old tags: the book is versioned per release in kexhq/docs,
    # and a tag's guide is whatever that repo held when the tag shipped — a
    # backfill loop over its version dirs, not this script's worktree walk.
    git -C "$ROOT" worktree remove --force "$wt" >/dev/null 2>&1 || true
  done
fi

# The unreleased builds, from the working tree as it stands. Both carry a
# "-dev" release so an unreleased build can never overwrite the published
# pages of the same version number.
#
# These are FATAL. They are the ones that break when docgen or the
# sources they read regress, and the previous version of this script masked
# every failure behind a warning — which is how a crash on startup, a
# swallowed --package and a broken HTML emitter all shipped unnoticed
# (kexhq/kex#287, #288, #289). A tag that no longer parses is still tolerated
# above; this is not.
build_docs "$ROOT/src/stdlib" prelude "Standard Library" \
  "$(kex_version "$ROOT")-dev" || unreleased_failed=1
build_docs "$ROOT/tey/src" tey "Tey" "$(tey_version "$ROOT")-dev" || \
  unreleased_failed=1
build_prose "$GUIDE_SOURCE" guide "Guide" \
  "$(kex_version "$ROOT")-dev" || unreleased_failed=1

# Static site assets docgen does not own — the favicon, the kexhq GitHub org
# icon. Copied here rather than generated, so docgen stays a documentation
# generator and the branding lives in the output.
if [ -f "$ROOT/tools/docs-assets/icon.png" ]; then
  cp "$ROOT/tools/docs-assets/icon.png" "$OUT/icon.png"
fi

# The final step: assemble the site-wide files (landing page, aggregated
# llms.txt/llms-full.txt/sitemap.xml, CNAME) out of every unit docgen wrote.
# Docgen documents one unit per run and stays free of site concepts; this
# package owns the site. Fatal like the unreleased builds above.
if [ -n "${BASE_URL:-}" ]; then
  (cd "$ROOT/tools/docsite" && "$TEY_RUN" run -- "$OUT" "$BASE_URL") || \
    unreleased_failed=1
else
  (cd "$ROOT/tools/docsite" && "$TEY_RUN" run -- "$OUT") || \
    unreleased_failed=1
fi

if [ "$unreleased_failed" -ne 0 ]; then
  echo "build-docs: FAILED — the unreleased build did not complete" >&2
  exit 1
fi

echo "build-docs: done -> $OUT"
