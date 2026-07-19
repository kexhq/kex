if(NOT DEFINED KEX_BINARY OR NOT DEFINED KEX_ARTIFACT_TEST_DIR)
    message(FATAL_ERROR "KEX_BINARY and KEX_ARTIFACT_TEST_DIR are required")
endif()

file(REMOVE_RECURSE "${KEX_ARTIFACT_TEST_DIR}")
file(MAKE_DIRECTORY "${KEX_ARTIFACT_TEST_DIR}")
file(WRITE "${KEX_ARTIFACT_TEST_DIR}/Kex.Obsolete.beam" "stale")
file(WRITE "${KEX_ARTIFACT_TEST_DIR}/Kex.Obsolete.core" "stale")

execute_process(
    COMMAND "${KEX_BINARY}" --build-prelude "${KEX_ARTIFACT_TEST_DIR}"
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
    if(EXISTS "${KEX_ARTIFACT_TEST_DIR}/Kex.Obsolete.${extension}")
        message(FATAL_ERROR "stale Kex.Obsolete.${extension} survived prelude build")
    endif()
endforeach()
if(NOT EXISTS "${KEX_ARTIFACT_TEST_DIR}/kex_prelude.beam")
    message(FATAL_ERROR "prelude entry beam was not produced")
endif()

file(GLOB companion_beams "${KEX_ARTIFACT_TEST_DIR}/Kex.*.beam")
if(NOT companion_beams)
    message(FATAL_ERROR "prelude companion beams were not produced")
endif()

file(REMOVE_RECURSE "${KEX_ARTIFACT_TEST_DIR}")
