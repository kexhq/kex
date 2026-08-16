cmake_minimum_required(VERSION 3.20)
project(kex LANGUAGES CXX)

include(GNUInstallDirs)

# ── Build revision ──────────────────────────────────────────────────────
# The commit a build came from, so `kex --version`, the REPL banner and
# `Kex.Kernel.VERSION` can all report it. Resolved at CONFIGURE time, with
# .git/HEAD as a dependency so switching branches or committing re-runs this
# rather than leaving a stale hash baked in. A source archive has no .git at
# all; that is a normal state and leaves the revision empty (`None` in Kex).
# The version NUMBERS are not here at all — they come from the VERSION file
# below.
#
# -DKEX_GIT_REVISION=<hash> supplies it when there is no .git to ask: a
# container build copies the source without the repository, and a release
# image that cannot name its commit is harder to trace than it needs to be.
if(NOT DEFINED KEX_GIT_REVISION)
    set(KEX_GIT_REVISION "")
endif()
if(NOT KEX_GIT_REVISION AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE KEX_GIT_REVISION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/.git/HEAD")
    endif()
endif()

# The version comes from the repository's VERSION file: one line,
# `MAJOR.MINOR.PATCH` with an optional `-prerelease`. It is baked into the
# binary below rather than written in a header, so a release bump touches one
# plain file that the release workflow and any packaging can read too.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" KEX_VERSION_RAW)
string(STRIP "${KEX_VERSION_RAW}" KEX_VERSION_FULL)
if(NOT KEX_VERSION_FULL MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9A-Za-z.-]+))?$")
    message(FATAL_ERROR
        "VERSION must read MAJOR.MINOR.PATCH[-prerelease], got '${KEX_VERSION_FULL}'")
endif()
set(KEX_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(KEX_VERSION_MINOR "${CMAKE_MATCH_2}")
set(KEX_VERSION_PATCH "${CMAKE_MATCH_3}")
set(KEX_VERSION_PRERELEASE "${CMAKE_MATCH_5}")

# Re-configure when the version changes, the same way the git HEAD is watched.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/VERSION")
if(KEX_GIT_REVISION)
    message(STATUS "kex ${KEX_VERSION_FULL} (${KEX_GIT_REVISION})")
else()
    message(STATUS "kex ${KEX_VERSION_MAJOR}.${KEX_VERSION_MINOR}.${KEX_VERSION_PATCH} (no git revision)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# An unoptimized (default/empty CMAKE_BUILD_TYPE) Asyncify build is not
# just slower than Release — it's practically unusable. Confirmed
# empirically: the full interpreter test suite (145 cases, including
# process/fiber-heavy ones) went from timing out past 280s and even
# outright failing a wall-clock-timing-sensitive test at the default build
# type, to completing entirely in well under a second at -O3. Asyncify's
# instrumentation overhead is apparently dominated by what optimization
# removes, not added by it, for this code — so default to Release for the
# wasm target specifically rather than requiring everyone building it to
# know to pass -DCMAKE_BUILD_TYPE=Release themselves. Native is unaffected
# (still whatever CMAKE_BUILD_TYPE the caller passes, empty by default,
# matching prior behavior).
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" --version
        OUTPUT_VARIABLE KEX_EMSCRIPTEN_VERSION_OUTPUT
        ERROR_VARIABLE KEX_EMSCRIPTEN_VERSION_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
    if(NOT KEX_EMSCRIPTEN_VERSION_OUTPUT MATCHES
            "Emscripten.* 5\\.0\\.7([ -]|$)")
        string(REGEX MATCH "^[^\r\n]*" KEX_EMSCRIPTEN_VERSION_LINE
            "${KEX_EMSCRIPTEN_VERSION_OUTPUT}")
        message(FATAL_ERROR
            "Kex's WASM fiber backend requires Emscripten 5.0.7; detected: "
            "${KEX_EMSCRIPTEN_VERSION_LINE}\n"
            "Activate emsdk 5.0.7 and configure a fresh WASM build directory. "
            "See third_party/gmp-wasm/README.md.")
    endif()
endif()

# Must come before add_library(kex_lib ...) below, not just before the
# executables that link it — add_compile_options()/add_link_options() only
# affect targets declared AFTER they're called. Placing these after
# add_library(kex_lib ...) (where they lived originally) meant kex_lib's
# own object files never got -sNO_DISABLE_EXCEPTION_CATCHING at compile
# time — only the final executables got it, at link time, which doesn't
# retroactively fix exception codegen already baked into kex_lib's already-
# compiled .o files. Confirmed the hard way: every try/catch inside
# kex_lib's sources (i.e. everywhere that matters — this is where the
# entire interpreter lives) silently failed to catch anything on wasm,
# while a hand-written reproduction of the same fiber+exception pattern in
# a single translation unit (compiled with the flag correctly applied)
# worked fine. Cost real time to find because the failure mode (a specific
# try/catch just doesn't fire) looks identical to a genuine Asyncify/EH
# interaction bug, which is what an actual Emscripten 6.0.2 regression
# (see third_party/gmp-wasm/README.md) had already trained us to suspect.
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    # Default wasm heap (16MB) is far smaller than what a native process
    # gets for free, and every process's fiber (see src/interpreter/fiber.cxx)
    # allocates its own 8MB C stack + 8MB asyncify stack — a single fiber
    # alone can exceed the default budget. -sALLOW_MEMORY_GROWTH lets the
    # heap grow on demand instead of hard-capping and aborting.
    add_link_options(-sALLOW_MEMORY_GROWTH=1)

    # emscripten_fiber_swap (src/interpreter/fiber.cxx's wasm backend)
    # requires Asyncify at link time — confirmed empirically, matching
    # Emscripten's own docs: fibers are built ON TOP OF Asyncify, not an
    # alternative to it, so this cost is unavoidable for the wasm target regardless of
    # which fiber API is used.
    add_link_options(-sASYNCIFY)
    # Default ASYNCIFY_STACK_SIZE (4KB) is nowhere near enough for this
    # tree-walking interpreter's recursion depth — eval()/callFunction()
    # nest many C++ frames deep per one level of Kex-level recursion, and
    # Asyncify needs scratch space to unwind/rewind through all of them on
    # every emscripten_fiber_swap. Confirmed empirically ("unreachable"
    # trap with a direct "may be due to ASYNCIFY_STACK_SIZE" hint).
    add_link_options(-sASYNCIFY_STACK_SIZE=16777216)

    # The interpreter uses C++ exceptions as its core control-flow/error
    # mechanism (RuntimeError, ReturnException, ...) — Emscripten disables
    # exception catching by default (smaller/faster output for code that
    # doesn't need it). Needed at both compile and link time (see the
    # comment above this whole block for why compile-time matters here).
    add_compile_options(-sNO_DISABLE_EXCEPTION_CATCHING)
    add_link_options(-sNO_DISABLE_EXCEPTION_CATCHING)
endif()

add_library(kex_lib
    src/ir/lower.cxx
    src/ir/emit_core.cxx
    src/lexer/token.cxx
    src/lexer/lexer.cxx
    src/parser/parser.cxx
    src/semantic/symbol.cxx
    src/semantic/analyzer.cxx
    src/semantic/receiver_conflicts.cxx
    src/semantic/types.cxx
    src/semantic/traits.cxx
    src/semantic/typechecker.cxx
    src/semantic/db.cxx
    src/semantic/collect_pass.cxx
    src/semantic/resolve_pass.cxx
    src/ast/clone.cxx
    src/compiled/expand.cxx
    src/compiled/reify.cxx
    src/module/resolver.cxx
    src/module/package_metadata.cxx
    src/validation/tag_validator.cxx
    src/interpreter/value.cxx
    src/interpreter/environment.cxx
    src/interpreter/fiber.cxx
    src/interpreter/scheduler.cxx
    src/interpreter/evaluator.cxx
    src/interpreter/stdlib/adt.cxx
    src/interpreter/stdlib/io.cxx
    src/interpreter/stdlib/file.cxx
    src/interpreter/stdlib/list.cxx
    src/interpreter/stdlib/string.cxx
    src/interpreter/stdlib/number.cxx
    src/interpreter/stdlib/regex.cxx
    src/interpreter/stdlib/stream.cxx
    src/interpreter/stdlib/map.cxx
    src/interpreter/stdlib/process.cxx
    src/interpreter/stdlib/digest.cxx
    src/interpreter/stdlib/env.cxx
    src/interpreter/stdlib/math.cxx
    src/interpreter/stdlib/time.cxx
    src/interpreter/stdlib/type.cxx
    src/interpreter/stdlib/bits.cxx
    src/interpreter/stdlib/console.cxx
    src/interpreter/stdlib/test.cxx
    src/interpreter/stdlib/parser.cxx
    src/interpreter/stdlib/eval.cxx
    src/interpreter/stdlib/http.cxx
    src/interpreter/stdlib/web.cxx
    src/interpreter/stdlib/kex.cxx
    src/beam/beam_file.cxx
    src/beam/etf.cxx
    src/beam/kexi.cxx
    src/beam/collect_metadata.cxx
    src/beam/kexi_registry.cxx
)

target_include_directories(kex_lib PUBLIC src)

target_compile_definitions(kex_lib PUBLIC
    KEX_VERSION_MAJOR=${KEX_VERSION_MAJOR}
    KEX_VERSION_MINOR=${KEX_VERSION_MINOR}
    KEX_VERSION_PATCH=${KEX_VERSION_PATCH}
    KEX_VERSION_PRERELEASE="${KEX_VERSION_PRERELEASE}")

if(KEX_GIT_REVISION)
    target_compile_definitions(kex_lib PUBLIC
        KEX_GIT_REVISION="${KEX_GIT_REVISION}")
endif()

# The build date, shown by `kex --version`. A DATE, not a timestamp, so two
# builds of the same source on the same day still match; pass
# -DKEX_BUILD_DATE=YYYY-MM-DD (the release workflow uses the tagged commit's
# date) to pin it exactly.
if(NOT DEFINED KEX_BUILD_DATE)
    string(TIMESTAMP KEX_BUILD_DATE "%Y-%m-%d" UTC)
endif()
target_compile_definitions(kex_lib PUBLIC KEX_BUILD_DATE="${KEX_BUILD_DATE}")

# Wasm can't discover the prelude via executableDirectory() (no /proc/self/exe,
# no _NSGetExecutablePath) so bake the source path in at compile time.
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    target_compile_definitions(kex_lib PUBLIC
        KEX_STDLIB_DIR="${CMAKE_SOURCE_DIR}/src/stdlib")

    # The COMPILED standard-library unit (kex_prelude.beam plus its Kex.*
    # companions). Without it the checker falls back to extracting interfaces
    # from .kex source, which cannot recover inferred result types or the
    # trait bounds behind `make T, implement: ...` — so the same program
    # type-checks differently under wasm than natively. The unit is produced
    # by running the NATIVE kex (`--build-prelude`, which needs erlc), so a
    # wasm-only build cannot make it: point this at a native build tree.
    # Loading it is pure parsing — no Erlang is involved at runtime, and wasm
    # never executes the code half of these files.
    set(KEX_PREBUILT_RUNTIME_DIR "" CACHE PATH
        "Native build's runtime/beam directory, holding the compiled stdlib unit")
    if(KEX_PREBUILT_RUNTIME_DIR AND
       EXISTS "${KEX_PREBUILT_RUNTIME_DIR}/kex_prelude.beam")
        target_compile_definitions(kex_lib PUBLIC
            KEX_RUNTIME_BEAM_DIR="${KEX_PREBUILT_RUNTIME_DIR}")
        set(KEX_WASM_RUNTIME_UNIT "${KEX_PREBUILT_RUNTIME_DIR}")
        message(STATUS "wasm: using compiled stdlib unit from ${KEX_PREBUILT_RUNTIME_DIR}")
    else()
        message(STATUS
            "wasm: no compiled stdlib unit — the checker will derive interfaces "
            "from source (set KEX_PREBUILT_RUNTIME_DIR to a native build's "
            "runtime/beam to match native behaviour)")
    endif()
endif()

# GMP backs the arbitrary-precision `Integer` default (see
# docs/types.md's Runtime representation section). Required,
# not optional like readline below.
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    # Wasm has no meaningful dynamic-linking story here, so this is a
    # deliberate, scoped exception to the native build's dynamic-only
    # policy below: statically linking GMP (dual-licensed LGPLv3/GPLv2)
    # into the wasm output is only permissible under LGPLv3's static-
    # linking allowance (relinkability — this repo, including this
    # third_party/gmp-wasm build recipe, provides everything needed to
    # rebuild and relink a modified GMP). No prebuilt wasm GMP exists via
    # Emscripten's port system or Homebrew — see third_party/gmp-wasm/
    # README.md for how this was built from source.
    set(GMP_WASM_DIR "${CMAKE_SOURCE_DIR}/third_party/gmp-wasm")
    set(GMP_LIB "${GMP_WASM_DIR}/lib/libgmp.a")
    set(GMPXX_LIB "${GMP_WASM_DIR}/lib/libgmpxx.a")
    set(GMP_INCLUDE "${GMP_WASM_DIR}/include")

    if(NOT EXISTS "${GMP_LIB}" OR NOT EXISTS "${GMPXX_LIB}" OR NOT EXISTS "${GMP_INCLUDE}/gmpxx.h")
        message(FATAL_ERROR
            "Prebuilt wasm GMP not found under ${GMP_WASM_DIR} — see "
            "third_party/gmp-wasm/README.md to rebuild it.")
    endif()
else()
    # Dynamic linking only: GMP is dual-licensed LGPLv3/GPLv2, fine to
    # depend on under LGPL terms when dynamically linked, but statically
    # linking it into the `kex` binary would pull the GPL's stricter terms
    # onto that binary. Force shared-only resolution rather than letting
    # find_library silently pick up the .a Homebrew also installs
    # alongside the .dylib.
    set(_kex_saved_lib_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    if(APPLE)
        set(CMAKE_FIND_LIBRARY_SUFFIXES .dylib)
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES .so)
    endif()
    find_library(GMP_LIB gmp HINTS /opt/homebrew/lib /usr/local/lib)
    find_library(GMPXX_LIB gmpxx HINTS /opt/homebrew/lib /usr/local/lib)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_kex_saved_lib_suffixes})
    find_path(GMP_INCLUDE gmpxx.h HINTS /opt/homebrew/include /usr/local/include)

    if(NOT GMP_LIB OR NOT GMPXX_LIB OR NOT GMP_INCLUDE)
        message(FATAL_ERROR
            "GMP (libgmp + libgmpxx, shared) not found — required for the "
            "arbitrary-precision Integer runtime. Install it (e.g. `brew "
            "install gmp`) and re-run cmake.")
    endif()
endif()

target_include_directories(kex_lib SYSTEM PUBLIC ${GMP_INCLUDE})
target_link_libraries(kex_lib PUBLIC ${GMPXX_LIB} ${GMP_LIB})

# PCRE2 backs the `Regex` module (src/stdlib/regex.kex). PCRE2 specifically,
# not RE2 or std::regex: Erlang's `re` is built on PCRE, so this is the only
# engine that gives the BEAM backend the same pattern *language* — see
# the same pattern language. BSD-licensed, so unlike GMP above
# there is no static/dynamic linking constraint here.
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    # Static build from third_party/pcre2-wasm (gitignored, not vendored —
    # same arrangement as gmp-wasm above; that directory's README.md has the
    # recipe). No Emscripten port exists for PCRE2, and unlike GMP its BSD
    # licence puts no constraint on linking it statically.
    set(PCRE2_WASM_DIR "${CMAKE_SOURCE_DIR}/third_party/pcre2-wasm")
    set(PCRE2_LIB "${PCRE2_WASM_DIR}/lib/libpcre2-8.a")
    set(PCRE2_INCLUDE "${PCRE2_WASM_DIR}/include")

    if(NOT EXISTS "${PCRE2_LIB}" OR NOT EXISTS "${PCRE2_INCLUDE}/pcre2.h")
        message(FATAL_ERROR
            "Prebuilt wasm PCRE2 not found under ${PCRE2_WASM_DIR} — see "
            "third_party/pcre2-wasm/README.md to rebuild it.")
    endif()
else()
    find_library(PCRE2_LIB pcre2-8 HINTS /opt/homebrew/lib /usr/local/lib)
    find_path(PCRE2_INCLUDE pcre2.h HINTS /opt/homebrew/include /usr/local/include)

    if(NOT PCRE2_LIB OR NOT PCRE2_INCLUDE)
        message(FATAL_ERROR
            "PCRE2 (libpcre2-8) not found — required for the Regex module. "
            "Install it (e.g. `brew install pcre2`) and re-run cmake.")
    endif()
endif()

# PCRE2 requires the code-unit width to be chosen by the including
# translation unit; 8 means UTF-8 subjects, matching Kex's String
# representation and Erlang `re`'s binary subjects. No KEX_HAVE_PCRE2 guard
# any more: every platform, wasm included, now fails to configure without
# PCRE2 rather than building a Regex-less interpreter.
target_compile_definitions(kex_lib PRIVATE PCRE2_CODE_UNIT_WIDTH=8)
target_include_directories(kex_lib SYSTEM PUBLIC ${PCRE2_INCLUDE})
target_link_libraries(kex_lib PUBLIC ${PCRE2_LIB})

# SHA-256 for KexI interface hashing: CommonCrypto on macOS, OpenSSL on Linux
if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    find_package(OpenSSL REQUIRED)
    target_link_libraries(kex_lib PUBLIC OpenSSL::Crypto)
endif()

# Boost.Context backs the native (non-Emscripten) fiber/coroutine primitive
# used for Kex's process model (see src/interpreter/fiber.cxx). Plain
# ucontext was tried first and rejected —
# on Apple Silicon macOS, swapcontext doesn't reliably save/restore FP/NEON
# register state across a context switch, causing heap corruption. Boost's
# fcontext backend is the well-tested primitive purpose-built for this.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    # Released archives link it STATICALLY (-DKEX_BOOST_STATIC=ON in
    # release.yml). Boost.Context is the one dependency whose Linux soname
    # carries the upstream version — libboost_context.so.1.83.0 for the
    # Ubuntu runner that builds the archive — so it can never be satisfied by
    # the copy a package manager installed: Homebrew's boost is a different
    # 1.x and its soname says so. That is what broke `brew install tey` on
    # Linux for 0.3.1, with the formula's `depends_on "boost"` satisfied and
    # useless. Every other dependency (gmp, pcre2, readline, crypto) has a
    # stable ABI-versioned soname that any of them provides, so only this one
    # has to come along in the binary. It is a few tens of KB of assembly.
    option(KEX_BOOST_STATIC "Link Boost.Context statically" OFF)
    if(KEX_BOOST_STATIC)
        set(Boost_USE_STATIC_LIBS ON)
    endif()
    find_package(Boost REQUIRED COMPONENTS context)
    target_link_libraries(kex_lib PUBLIC Boost::context)

    # The language server is part of the kex executable. Keep its protocol
    # implementation vendored so configuring a source checkout stays offline.
    set(LSP_INSTALL OFF CACHE BOOL "" FORCE)
    set(LSP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/lsp-framework EXCLUDE_FROM_ALL)
endif()

# Optional readline support
find_library(READLINE_LIB readline HINTS /opt/homebrew/opt/readline/lib /usr/local/opt/readline/lib)
find_path(READLINE_INCLUDE readline/readline.h HINTS /opt/homebrew/opt/readline/include /usr/local/opt/readline/include)

# ── Erlang runtime beams ────────────────────────────────────────────────
# Pre-compile runtime/src/*.erl into the build tree so that BEAM runs don't
# pay an erlc spawn (~0.15s per module) on every invocation.
# Missing artifacts are diagnosed; user operations never compile runtime
# source implicitly.
find_program(ERLC_EXECUTABLE erlc)
set(KEX_RUNTIME_BEAM_DIR "${CMAKE_BINARY_DIR}/runtime/beam")
file(GLOB KEX_RUNTIME_ERL CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/src/*.erl")
set(KEX_RUNTIME_BEAMS "")
foreach(erl IN LISTS KEX_RUNTIME_ERL)
    cmake_path(GET erl STEM stem)
    list(APPEND KEX_RUNTIME_BEAMS "${KEX_RUNTIME_BEAM_DIR}/${stem}.beam")
endforeach()

# The OTP release of the erlc above, baked into the binary. A .beam cannot be
# loaded by a runtime older than the one that produced it (OTP 28 changed the
# atom table encoding: older loaders report "corrupt atom table" and the run
# dies as an anonymous `undef`). kex checks this floor against the `erl` it is
# about to spawn and diagnoses the mismatch instead. 0 disables the check —
# set when erlc is missing (BEAM modes then need prebuilt artifacts anyway)
# or when the release cannot be determined.
set(KEX_RUNTIME_OTP_FLOOR 0)
if(ERLC_EXECUTABLE)
    # Prefer the erl sitting beside the discovered erlc — same install — so
    # the floor describes the beams actually being compiled, not whatever
    # erl happens to be first on PATH.
    cmake_path(GET ERLC_EXECUTABLE PARENT_PATH _erlc_dir)
    find_program(ERL_EXECUTABLE erl HINTS "${_erlc_dir}")
    if(ERL_EXECUTABLE)
        execute_process(
            COMMAND ${ERL_EXECUTABLE} -noshell -eval
                "io:format(\"~s\",[erlang:system_info(otp_release)]),halt()."
            OUTPUT_VARIABLE _otp_release
            RESULT_VARIABLE _otp_result
            TIMEOUT 30
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_otp_result EQUAL 0 AND _otp_release MATCHES "^[0-9]+$")
            set(KEX_RUNTIME_OTP_FLOOR "${_otp_release}")
        else()
            message(WARNING "Runtime beams: could not determine the OTP release of ${ERL_EXECUTABLE}; the BEAM runtime-version check will be disabled")
        endif()
    endif()
endif()

if(ERLC_EXECUTABLE AND KEX_RUNTIME_BEAMS)
    add_custom_command(
        OUTPUT ${KEX_RUNTIME_BEAMS}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${KEX_RUNTIME_BEAM_DIR}
        # The version reaches the BEAM side as erlc defines rather than a
        # second copy of the numbers: kex_intrinsic_kex:version/0 has to
        # answer exactly what the native binary reports.
        COMMAND ${ERLC_EXECUTABLE}
            -DKEX_VERSION_MAJOR=${KEX_VERSION_MAJOR}
            -DKEX_VERSION_MINOR=${KEX_VERSION_MINOR}
            -DKEX_VERSION_PATCH=${KEX_VERSION_PATCH}
            "-DKEX_VERSION_PRERELEASE=\"${KEX_VERSION_PRERELEASE}\""
            "-DKEX_GIT_REVISION=\"${KEX_GIT_REVISION}\""
            -o ${KEX_RUNTIME_BEAM_DIR} ${KEX_RUNTIME_ERL}
        DEPENDS ${KEX_RUNTIME_ERL}
        COMMENT "Compiling Kex runtime Erlang modules"
        VERBATIM
    )
    add_custom_target(kex_runtime_beams DEPENDS ${KEX_RUNTIME_BEAMS})
    set(KEX_HAS_RUNTIME_BEAMS TRUE)
    message(STATUS "Runtime beams: compiled by OTP ${KEX_RUNTIME_OTP_FLOOR} (loadable by OTP ${KEX_RUNTIME_OTP_FLOOR}+ only)")
else()
    message(STATUS "Runtime beams: skipped (erlc not found); BEAM modes require prebuilt runtime artifacts")
endif()

add_executable(kex src/main.cxx)
target_link_libraries(kex PRIVATE kex_lib)
target_compile_definitions(kex PRIVATE KEX_RUNTIME_OTP_FLOOR=${KEX_RUNTIME_OTP_FLOOR})
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    target_sources(kex PRIVATE src/lsp/server.cxx)
    target_compile_definitions(kex PRIVATE KEX_HAS_LSP=1)
    target_link_libraries(kex PRIVATE lsp)
endif()
if(KEX_HAS_RUNTIME_BEAMS)
    add_dependencies(kex kex_runtime_beams)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    install(TARGETS kex RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY src/stdlib/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/kex/stdlib
        FILES_MATCHING PATTERN "*.kex")
    # The package.kex vocabulary ships beside the stdlib but NOT inside it:
    # anything under stdlib/ is compiled into the prelude artifact, and these
    # names (`version`, `target`, `kex`) must reach a manifest only.
    install(DIRECTORY src/manifest/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/kex/manifest
        FILES_MATCHING PATTERN "*.kex")
endif()

# ── Kex stdlib artifact unit ─────────────────────────────────────────────
# The prelude manifest selects standard-library sources from src/stdlib and is
# compiled into a
# kex_prelude.beam entry plus Kex.* companion modules. They are placed beside
# the runtime beams so BEAM runs put the unit on the -pa path and lazy-load it
# on first use. Built by the freshly-built kex binary (Kex → Core Erlang →
# beam), so it depends on `kex` and the intrinsic beams it calls.
if(KEX_HAS_RUNTIME_BEAMS AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(KEX_PRELUDE_BEAM "${KEX_RUNTIME_BEAM_DIR}/kex_prelude.beam")
    file(GLOB_RECURSE KEX_STDLIB_SRC CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/stdlib/*.kex")
    # KEX_STDLIB_DIR is passed explicitly rather than left to the binary's
    # own discovery: that walks up from the EXECUTABLE (../src/stdlib), which
    # only finds the sources when the build tree sits inside the source tree.
    # An out-of-tree build — what `tey kex install` does, building into its
    # cache — failed with "no prelude source root is available".
    add_custom_command(
        OUTPUT ${KEX_PRELUDE_BEAM}
        COMMAND ${CMAKE_COMMAND} -E env
            "KEX_STDLIB_DIR=${CMAKE_SOURCE_DIR}/src/stdlib"
            $<TARGET_FILE:kex> --build-prelude ${KEX_RUNTIME_BEAM_DIR}
        DEPENDS kex ${KEX_STDLIB_SRC} ${KEX_RUNTIME_BEAMS}
        COMMENT "Prebuilding Kex prelude (kex_prelude.beam)"
        VERBATIM
    )
    add_custom_target(kex_prelude_beam ALL DEPENDS ${KEX_PRELUDE_BEAM})
    # --build-prelude also emits Kex.* companion modules whose filenames are
    # discovered only at build time, so install the complete beam directory.
    install(DIRECTORY "${KEX_RUNTIME_BEAM_DIR}/"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/kex/runtime
        FILES_MATCHING PATTERN "*.beam")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    # NODERAWFS maps every fs call straight through to Node's real host
    # filesystem (no MEMFS sandbox, no --preload-file needed) — this is
    # what lets `node build-wasm/kex.js some/real/path.kex` work at all,
    # Deliberately not applied to kex_repl_wasm (below),
    # which needs to run in an actual browser, where NODERAWFS doesn't
    # apply — that target uses --preload-file instead. This is what makes
    # `make spec-wasm` possible at all.
    target_link_options(kex PRIVATE -sNODERAWFS=1)
endif()

if(READLINE_LIB AND READLINE_INCLUDE)
    target_compile_definitions(kex PRIVATE HAS_READLINE=1)
    target_include_directories(kex PRIVATE ${READLINE_INCLUDE})
    target_link_libraries(kex PRIVATE ${READLINE_LIB})
    message(STATUS "Readline support: enabled")
else()
    message(STATUS "Readline support: disabled (readline not found)")
endif()

# Wasm-only: a small C API (src/wasm_repl.cxx) for driving the interpreter
# from JS — deliberately not the `kex` CLI binary above, whose interactive
# REPL loop assumes a real blocking stdin (readline or a plain-stdin
# fallback), neither of which exists in a browser. See
# web/index.html for the test page that loads this.
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    add_executable(kex_repl_wasm src/wasm_repl.cxx)
    target_link_libraries(kex_repl_wasm PRIVATE kex_lib)
    target_link_options(kex_repl_wasm PRIVATE
        -sMODULARIZE=1
        # A real `export default`, not just Emscripten's default UMD-style
        # module.exports/AMD-only wrapper — that form works when imported
        # from Node (whose ESM loader treats a CommonJS module.exports as
        # the default export automatically) but silently has no export at
        # all when loaded via a browser <script type="module">, since
        # neither `module` nor `exports` exist there. Confirmed the hard
        # way via an actual headless-browser run of web/index.html, not
        # just the Node-based smoke tests used earlier in this file's
        # history — testing only through Node wasn't sufficient here.
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createKexReplModule
        -sEXPORTED_FUNCTIONS=_kex_version,_kex_repl_banner,_kex_repl_create,_kex_repl_destroy,_kex_repl_eval,_kex_repl_last_result,_kex_repl_complete,_malloc,_free
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8
        # Embeds the unified stdlib into MEMFS. `prelude.kex` selects the
        # automatic subset; opt-in modules resolve from the same root.
        # Keep each flag and its mapped path together. CMake otherwise
        # de-duplicates the repeated --preload-file flag, leaving the second
        # path as a bogus linker input.
        "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/src/stdlib@/stdlib"
    )
    # The compiled stdlib unit is deliberately NOT preloaded here. The browser
    # REPL never sets ImportedInterfaces on its SemanticDB, so it would embed
    # ~330 KB that nothing reads. prebuiltRuntimeBeamDir() already looks in
    # /runtime, so mapping it here is the only change needed if that REPL
    # grows a type-checking path.
endif()

enable_testing()

# Parse, type-check, and lower the exact source set selected by prelude.kex.
# Run with: cmake --build build --target check-prelude
set(KEX_PRELUDE_MANIFEST "${CMAKE_SOURCE_DIR}/src/stdlib/prelude.kex")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${KEX_PRELUDE_MANIFEST}")
file(GLOB_RECURSE KEX_PRELUDE_CHECK_DEPS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/stdlib/*.kex")
set(KEX_PRELUDE_CHECK_DIR
    "${CMAKE_BINARY_DIR}/prelude-check")
add_custom_target(check-prelude
    COMMAND ${CMAKE_COMMAND} -E echo "Checking prelude..."
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${KEX_PRELUDE_CHECK_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env
        "KEX_STDLIB_DIR=${CMAKE_SOURCE_DIR}/src/stdlib"
        $<TARGET_FILE:kex> --build-prelude
        "${KEX_PRELUDE_CHECK_DIR}"
    DEPENDS kex ${KEX_PRELUDE_CHECK_DEPS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM
)

add_executable(beam_test tests/beam_test.cxx)
target_link_libraries(beam_test PRIVATE kex_lib)
target_include_directories(beam_test PRIVATE tests)
add_test(NAME beam_test COMMAND beam_test)

add_executable(codegen_test tests/codegen_test.cxx)
target_link_libraries(codegen_test PRIVATE kex_lib)
target_include_directories(codegen_test PRIVATE tests)
if(TARGET kex_prelude_beam)
    target_compile_definitions(codegen_test PRIVATE
        KEX_RUNTIME_BEAM_DIR="${KEX_RUNTIME_BEAM_DIR}")
    add_dependencies(codegen_test kex_prelude_beam)
endif()
add_test(NAME codegen_test COMMAND codegen_test)

add_executable(lexer_test tests/lexer_test.cxx)
target_link_libraries(lexer_test PRIVATE kex_lib)
target_include_directories(lexer_test PRIVATE tests)
add_test(NAME lexer_test COMMAND lexer_test)

add_executable(parser_test tests/parser_test.cxx)
target_link_libraries(parser_test PRIVATE kex_lib)
target_include_directories(parser_test PRIVATE tests)
add_test(NAME parser_test COMMAND parser_test)

add_executable(module_resolver_test tests/module_resolver_test.cxx)
target_link_libraries(module_resolver_test PRIVATE kex_lib)
target_include_directories(module_resolver_test PRIVATE tests)
add_test(NAME module_resolver_test COMMAND module_resolver_test)

add_executable(semantic_test tests/semantic_test.cxx)
target_link_libraries(semantic_test PRIVATE kex_lib)
target_include_directories(semantic_test PRIVATE tests)
if(TARGET kex_prelude_beam)
    target_compile_definitions(semantic_test PRIVATE
        KEX_RUNTIME_BEAM_DIR="${KEX_RUNTIME_BEAM_DIR}")
    add_dependencies(semantic_test kex_prelude_beam)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    target_compile_definitions(semantic_test PRIVATE KEX_RUNTIME_BEAM_DIR="")
    target_link_options(semantic_test PRIVATE -sNODERAWFS=1)
endif()
add_test(NAME semantic_test COMMAND semantic_test)

add_executable(validation_test tests/validation_test.cxx)
target_link_libraries(validation_test PRIVATE kex_lib)
target_include_directories(validation_test PRIVATE tests)
add_test(NAME validation_test COMMAND validation_test)

add_executable(interpreter_test tests/interpreter_test.cxx)
target_link_libraries(interpreter_test PRIVATE kex_lib)
target_include_directories(interpreter_test PRIVATE tests)
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    target_link_options(interpreter_test PRIVATE -sNODERAWFS=1)
endif()
add_test(NAME interpreter_test COMMAND interpreter_test)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    # Compile the browser REPL entry point natively as a lightweight contract
    # test. This catches session-configuration drift without requiring a
    # browser or Asyncify-capable JS harness in the native CI job.
    add_executable(wasm_repl_test
        tests/wasm_repl_test.cxx
        src/wasm_repl.cxx)
    target_link_libraries(wasm_repl_test PRIVATE kex_lib)
    target_include_directories(wasm_repl_test PRIVATE tests)
    add_test(NAME wasm_repl_test COMMAND wasm_repl_test)

    add_executable(lsp_test
        tests/lsp_test.cxx
        src/lsp/server.cxx)
    target_link_libraries(lsp_test PRIVATE kex_lib lsp)
    target_include_directories(lsp_test PRIVATE tests)
    add_test(NAME lsp_test COMMAND lsp_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

add_executable(examples_test tests/examples_test.cxx)
target_link_libraries(examples_test PRIVATE kex_lib)
target_include_directories(examples_test PRIVATE tests)
add_test(NAME examples_test COMMAND examples_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(completion_test tests/completion_test.cxx)
target_link_libraries(completion_test PRIVATE kex_lib)
target_include_directories(completion_test PRIVATE tests)
add_test(NAME completion_test COMMAND completion_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(repl_test tests/repl_test.cxx)
target_link_libraries(repl_test PRIVATE kex_lib)
target_include_directories(repl_test PRIVATE tests)
add_test(NAME repl_test COMMAND repl_test)

# Drives the real `kex` binary's REPL loop (prompt, multi-line continuation,
# colorValue rendering) over stdin — repl_test.cxx's ReplSession bypasses
# main.cxx entirely, so it can't catch bugs in that loop or in colorValue().
add_executable(repl_cli_test tests/repl_cli_test.cxx)
target_link_libraries(repl_cli_test PRIVATE kex_lib)
target_include_directories(repl_cli_test PRIVATE tests)
target_compile_definitions(repl_cli_test PRIVATE KEX_BINARY_PATH="$<TARGET_FILE:kex>")
add_dependencies(repl_cli_test kex)
add_test(NAME repl_cli_test COMMAND repl_cli_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME installed_layout_test
    COMMAND ${CMAKE_COMMAND}
        -DKEX_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DKEX_INSTALL_TEST_PREFIX=${CMAKE_BINARY_DIR}/test-install
        -P ${CMAKE_SOURCE_DIR}/tests/installed_layout_test.cmake)

add_test(NAME dependency_layering_test
    COMMAND ${CMAKE_COMMAND}
        -DKEX_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/tests/dependency_layering_test.cmake)

if(KEX_HAS_RUNTIME_BEAMS AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    add_test(NAME prelude_artifact_test
        COMMAND ${CMAKE_COMMAND}
            -DKEX_BINARY=$<TARGET_FILE:kex>
            -DKEX_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DKEX_ARTIFACT_TEST_DIR=${CMAKE_BINARY_DIR}/test-prelude-artifacts
            -P ${CMAKE_SOURCE_DIR}/tests/prelude_artifact_test.cmake)
endif()

# Verifies ANSI coloring is consistent across the REPL, IO.inspect, and error
# diagnostics by driving the real `kex` binary. colorizeMessage() and the
# diagnostic print loops live in main.cxx, unreachable from the in-process
# test harness, so this asserts on the binary's raw output bytes.
add_executable(color_cli_test tests/color_cli_test.cxx)
target_link_libraries(color_cli_test PRIVATE kex_lib)
target_include_directories(color_cli_test PRIVATE tests)
target_compile_definitions(color_cli_test PRIVATE KEX_BINARY_PATH="$<TARGET_FILE:kex>")
add_dependencies(color_cli_test kex)
add_test(NAME color_cli_test COMMAND color_cli_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
