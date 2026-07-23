if(NOT DEFINED KEX_BINARY OR NOT DEFINED KEX_SOURCE_DIR OR
   NOT DEFINED KEX_ARTIFACT_TEST_DIR)
    message(FATAL_ERROR
        "KEX_BINARY, KEX_SOURCE_DIR, and KEX_ARTIFACT_TEST_DIR are required")
endif()

file(REMOVE_RECURSE "${KEX_ARTIFACT_TEST_DIR}")
file(MAKE_DIRECTORY "${KEX_ARTIFACT_TEST_DIR}")
file(COPY "${KEX_SOURCE_DIR}/src/prelude"
    DESTINATION "${KEX_ARTIFACT_TEST_DIR}")
set(test_source_dir "${KEX_ARTIFACT_TEST_DIR}/prelude")
set(test_runtime_dir "${KEX_ARTIFACT_TEST_DIR}/runtime")
file(MAKE_DIRECTORY "${test_runtime_dir}")
file(WRITE "${test_runtime_dir}/Kex.Obsolete.beam" "stale")
file(WRITE "${test_runtime_dir}/Kex.Obsolete.core" "stale")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "KEX_STDLIB_DIR=${test_source_dir}"
        "${KEX_BINARY}" --build-prelude "${test_runtime_dir}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "isolated prelude build failed (${build_result})\n"
        "stdout:\n${build_stdout}\n"
        "stderr:\n${build_stderr}")
endif()

foreach(extension IN ITEMS beam core)
    if(EXISTS "${test_runtime_dir}/Kex.Obsolete.${extension}")
        message(FATAL_ERROR "stale Kex.Obsolete.${extension} survived prelude build")
    endif()
endforeach()
if(NOT EXISTS "${test_runtime_dir}/kex_prelude.beam")
    message(FATAL_ERROR "prelude entry beam was not produced")
endif()

file(GLOB companion_beams "${test_runtime_dir}/Kex.*.beam")
if(NOT companion_beams)
    message(FATAL_ERROR "prelude companion beams were not produced")
endif()

file(APPEND "${test_source_dir}/math.kex"
    "\n# Source-freshness regression: interface intentionally unchanged.\n")
file(WRITE "${KEX_ARTIFACT_TEST_DIR}/check.kex" "main do\n  1\nend\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "KEX_STDLIB_DIR=${test_source_dir}"
        "KEX_RUNTIME_DIR=${test_runtime_dir}"
        "${KEX_BINARY}" --check "${KEX_ARTIFACT_TEST_DIR}/check.kex"
    RESULT_VARIABLE stale_result
    OUTPUT_VARIABLE stale_stdout
    ERROR_VARIABLE stale_stderr)
if(stale_result EQUAL 0 OR
   NOT stale_stderr MATCHES "source digest mismatch")
    message(FATAL_ERROR
        "stale prelude source was not rejected (${stale_result})\n"
        "stdout:\n${stale_stdout}\n"
        "stderr:\n${stale_stderr}")
endif()

file(REMOVE_RECURSE "${KEX_ARTIFACT_TEST_DIR}")
