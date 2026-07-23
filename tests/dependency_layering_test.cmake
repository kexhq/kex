if(NOT DEFINED KEX_SOURCE_DIR)
    message(FATAL_ERROR "KEX_SOURCE_DIR is required")
endif()

# Compiler targets (lexer, parser, semantic, ir) must not include stdlib source
# files or native public builtin registries directly. The only allowed path
# into stdlib is through the generic package and intrinsic interfaces in
# src/common/ (prelude_loader, etc.).
#
# src/interpreter/stdlib/ is excluded from this check — the interpreter IS the
# native builtin registry, so it necessarily includes evaluator internals.
# src/main.cxx and src/wasm_repl.cxx are entry points, not compiler targets.

set(compiler_dirs
    "${KEX_SOURCE_DIR}/src/lexer"
    "${KEX_SOURCE_DIR}/src/parser"
    "${KEX_SOURCE_DIR}/src/ast"
    "${KEX_SOURCE_DIR}/src/semantic"
    "${KEX_SOURCE_DIR}/src/ir"
    "${KEX_SOURCE_DIR}/src/beam"
    "${KEX_SOURCE_DIR}/src/module")

set(violations)

foreach(dir IN LISTS compiler_dirs)
    if(NOT EXISTS "${dir}")
        continue()
    endif()
    file(GLOB_RECURSE sources "${dir}/*.cxx" "${dir}/*.hxx")
    foreach(src IN LISTS sources)
        file(READ "${src}" content)
        file(RELATIVE_PATH relpath "${KEX_SOURCE_DIR}" "${src}")

        # No compiler target should #include prelude .kex sources or read them
        # at compile time (runtime loading through prelude_loader is fine).
        string(REGEX MATCHALL "#include[^\n]*prelude/[^\n]*\\.kex" prelude_hits "${content}")
        foreach(hit IN LISTS prelude_hits)
            list(APPEND violations "${relpath}: ${hit}")
        endforeach()

        # No compiler target should #include runtime Erlang sources.
        string(REGEX MATCHALL "#include[^\n]*runtime/[^\n]*\\.erl" runtime_hits "${content}")
        foreach(hit IN LISTS runtime_hits)
            list(APPEND violations "${relpath}: ${hit}")
        endforeach()

        # No compiler target should #include interpreter stdlib registrations.
        string(REGEX MATCHALL "#include[^\n]*interpreter/stdlib/[^\n]*" stdlib_hits "${content}")
        foreach(hit IN LISTS stdlib_hits)
            list(APPEND violations "${relpath}: ${hit}")
        endforeach()
    endforeach()
endforeach()

if(violations)
    string(REPLACE ";" "\n  " formatted "${violations}")
    message(FATAL_ERROR
        "dependency layering violation — compiler targets must not include "
        "stdlib sources or native builtin registries:\n  ${formatted}")
endif()
