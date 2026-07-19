if(NOT DEFINED KEX_SOURCE_DIR)
    message(FATAL_ERROR "KEX_SOURCE_DIR is required")
endif()

file(READ "${KEX_SOURCE_DIR}/src/ir/lower.cxx" lower_source)

set(guard_start_marker "// Guard-safe inline lowerings.")
set(guard_end_marker "// .to(Type) numeric/string conversion")
string(FIND "${lower_source}" "${guard_start_marker}" guard_start)
string(FIND "${lower_source}" "${guard_end_marker}" guard_end)
if(guard_start EQUAL -1 OR guard_end EQUAL -1 OR guard_end LESS guard_start)
    message(FATAL_ERROR "guard compatibility lowering markers are missing or reordered")
endif()

math(EXPR guard_length "${guard_end} - ${guard_start}")
string(SUBSTRING "${lower_source}" ${guard_start} ${guard_length} guard_source)
string(REGEX MATCHALL "m == \"[A-Za-z?]+\"" guard_matches "${guard_source}")

set(actual_names)
foreach(match IN LISTS guard_matches)
    string(REGEX REPLACE "m == \"([A-Za-z?]+)\"" "\\1" name "${match}")
    list(APPEND actual_names "${name}")
endforeach()
list(SORT actual_names)

# This is intentionally an exact, shrinking allowlist. Public stdlib names may
# remain in lowering only until effect-aware pure guards replace this block.
set(allowed_names
    "abs"
    "alive?"
    "alpha?"
    "digit?"
    "error?"
    "even?"
    "in?"
    "none?"
    "odd?"
    "ok?"
    "space?"
)
list(SORT allowed_names)

if(NOT actual_names STREQUAL allowed_names)
    message(FATAL_ERROR
        "guard compatibility allowlist changed\n"
        "expected: ${allowed_names}\n"
        "actual:   ${actual_names}")
endif()
