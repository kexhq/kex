# Plan: Distribute `kex` via Homebrew

Captured 2026-06-28. Not yet started.

## Routes

1. **Personal tap (recommended for now)** — create `akoskovacs/homebrew-tap` on
   GitHub, drop a `Formula/kex.rb` into it. Users install with
   `brew tap akoskovacs/tap && brew install kex`. No approval process, we
   control versioning. Right fit for the project's current maturity.
2. **homebrew-core** — requires notability (stars/usage), tagged stable
   releases, and a non-`head` URL. Premature today (no git tags yet).

## Blockers (must fix before either route works)

### 1. `KEX_PRELUDE_DIR` is broken for installed binaries
`CMakeLists.txt:76` bakes it at compile time to
`${CMAKE_SOURCE_DIR}/src/prelude`, which won't exist on a user's machine
after `brew install`. The prelude `.kex` files must be installed alongside
the binary and the path resolved to the install prefix.

### 2. No `install` rules in CMakeLists
`cmake --install` currently does nothing — there's no
`install(TARGETS kex ...)` and nothing for the prelude.

### 3. No tagged releases
A formula needs a stable `url` + `sha256` (a tag tarball). `head`-only
formulas are discouraged by Homebrew.

### 4. Future dependency: Erlang/BEAM
BEAM codegen is already in flight (`src/codegen/core_erlang.cxx`,
`docs/beam-codegen.md`, `*.beam` artifacts in the tree). Once the compiler
emits `.beam` files that users are expected to *run* (not just produce),
the formula will need `depends_on "erlang"` so the BEAM runtime is
available. Today the interpreter is self-contained and Erlang is only a
build-time/development concern, so this can be deferred — but it should
land in the same formula revision that ships runnable BEAM output, not
after.

## Fix outline

In `CMakeLists.txt`, make the prelude path overridable and add install rules:

```cmake
# Keep dev-build default, allow formula to override.
set(KEX_PRELUDE_DIR "${CMAKE_SOURCE_DIR}/src/prelude"
    CACHE PATH "Runtime location of prelude .kex files")
# ...existing target_compile_definitions(kex ... KEX_PRELUDE_DIR=...) stays.

install(TARGETS kex RUNTIME DESTINATION bin)
install(DIRECTORY src/prelude/ DESTINATION share/kex/prelude)
```

The GMP dynamic-link constraint in the existing CMake
(`CMAKE_FIND_LIBRARY_SUFFIXES` forced to `.dylib`/`.so`) is already what
Homebrew wants — no change needed there.

## The formula

After tagging `v0.1.0` and pushing a GitHub release, place this in
`akoskovacs/homebrew-tap/Formula/kex.rb`:

```ruby
class Kex < Formula
  desc "Functional language with Ruby-like syntax, UFCS, and an Elixir-style process model"
  homepage "https://github.com/akoskovacs/kex"
  url "https://github.com/akoskovacs/kex/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "<sha256 of the tarball>"
  license "MIT"
  head "https://github.com/akoskovacs/kex.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "gmp"      # already dynamic-link-only per CMake comments
  depends_on "readline"
  # depends_on "erlang"  # TODO: add when BEAM output is shippable (blocker #4)

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args,
           "-DKEX_PRELUDE_DIR=#{pkgshare}/kex/prelude"
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    (testpath/"t.kex").write <<~EOS
      IO.puts("hello")
    EOS
    assert_match "hello", shell_output("#{bin}/kex t.kex")
  end
end
```

## Validation

From inside the tap repo once the formula exists:

```
brew audit --strict kex
brew test kex
```
