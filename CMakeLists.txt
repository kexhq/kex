cmake_minimum_required(VERSION 3.20)
project(kex LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(kex_lib
    src/lexer/token.cxx
    src/lexer/lexer.cxx
    src/parser/parser.cxx
    src/semantic/symbol.cxx
    src/semantic/analyzer.cxx
    src/semantic/types.cxx
    src/semantic/traits.cxx
    src/semantic/stdlib_signatures.cxx
    src/semantic/typechecker.cxx
    src/interpreter/value.cxx
    src/interpreter/environment.cxx
    src/interpreter/evaluator.cxx
    src/interpreter/stdlib/adt.cxx
    src/interpreter/stdlib/io.cxx
    src/interpreter/stdlib/file.cxx
    src/interpreter/stdlib/list.cxx
    src/interpreter/stdlib/string.cxx
    src/interpreter/stdlib/integer.cxx
    src/interpreter/stdlib/stream.cxx
    src/interpreter/stdlib/map.cxx
    src/interpreter/stdlib/env.cxx
    src/interpreter/stdlib/math.cxx
    src/interpreter/stdlib/test.cxx
)

target_include_directories(kex_lib PUBLIC src)

# GMP backs the arbitrary-precision `Integer` default (see
# docs/type-system-plan.md's Runtime representation section). Required,
# not optional like readline below — dynamic linking only: GMP is
# dual-licensed LGPLv3/GPLv2, fine to depend on under LGPL terms when
# dynamically linked, but statically linking it into the `kex` binary
# would pull the GPL's stricter terms onto that binary. Force shared-only
# resolution rather than letting find_library silently pick up the .a
# Homebrew also installs alongside the .dylib.
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

target_include_directories(kex_lib PUBLIC ${GMP_INCLUDE})
target_link_libraries(kex_lib PUBLIC ${GMPXX_LIB} ${GMP_LIB})

# Optional readline support
find_library(READLINE_LIB readline HINTS /opt/homebrew/opt/readline/lib /usr/local/opt/readline/lib)
find_path(READLINE_INCLUDE readline/readline.h HINTS /opt/homebrew/opt/readline/include /usr/local/opt/readline/include)

add_executable(kex src/main.cxx)
target_link_libraries(kex PRIVATE kex_lib)

if(READLINE_LIB AND READLINE_INCLUDE)
    target_compile_definitions(kex PRIVATE HAS_READLINE=1)
    target_include_directories(kex PRIVATE ${READLINE_INCLUDE})
    target_link_libraries(kex PRIVATE ${READLINE_LIB})
    message(STATUS "Readline support: enabled")
else()
    message(STATUS "Readline support: disabled (readline not found)")
endif()

enable_testing()

# Type-check all .kex files under src/prelude/ using `kex --check`.
# Run with: cmake --build build --target check-prelude
file(GLOB PRELUDE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/prelude/*.kex")
add_custom_target(check-prelude
    COMMAND ${CMAKE_COMMAND} -E echo "Checking prelude..."
    DEPENDS kex
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
foreach(src IN LISTS PRELUDE_SOURCES)
    cmake_path(GET src FILENAME fname)
    add_custom_command(TARGET check-prelude POST_BUILD
        COMMAND $<TARGET_FILE:kex> --check ${src}
        COMMENT "  ${fname}"
        VERBATIM
    )
endforeach()

add_executable(lexer_test tests/lexer_test.cxx)
target_link_libraries(lexer_test PRIVATE kex_lib)
target_include_directories(lexer_test PRIVATE tests)
add_test(NAME lexer_test COMMAND lexer_test)

add_executable(parser_test tests/parser_test.cxx)
target_link_libraries(parser_test PRIVATE kex_lib)
target_include_directories(parser_test PRIVATE tests)
add_test(NAME parser_test COMMAND parser_test)

add_executable(semantic_test tests/semantic_test.cxx)
target_link_libraries(semantic_test PRIVATE kex_lib)
target_include_directories(semantic_test PRIVATE tests)
add_test(NAME semantic_test COMMAND semantic_test)

add_executable(interpreter_test tests/interpreter_test.cxx)
target_link_libraries(interpreter_test PRIVATE kex_lib)
target_include_directories(interpreter_test PRIVATE tests)
add_test(NAME interpreter_test COMMAND interpreter_test)

add_executable(examples_test tests/examples_test.cxx)
target_link_libraries(examples_test PRIVATE kex_lib)
target_include_directories(examples_test PRIVATE tests)
add_test(NAME examples_test COMMAND examples_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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
