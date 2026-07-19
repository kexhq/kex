if(NOT DEFINED KEX_BUILD_DIR OR NOT DEFINED KEX_INSTALL_TEST_PREFIX)
    message(FATAL_ERROR "installed layout test requires build and prefix paths")
endif()

file(REMOVE_RECURSE "${KEX_INSTALL_TEST_PREFIX}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${KEX_BUILD_DIR}"
            --prefix "${KEX_INSTALL_TEST_PREFIX}"
    RESULT_VARIABLE install_result
    OUTPUT_QUIET)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "could not install the test prefix")
endif()

set(program "${KEX_INSTALL_TEST_PREFIX}/installed.kex")
file(WRITE "${program}"
    "main do\n"
    "  IO.printLine(Stream.Sequence(from: 1) { |n| n + 1 }.take(2))\n"
    "end\n")

set(ENV{KEX_STDLIB_DIR} "")
set(ENV{KEX_RUNTIME_DIR} "")
set(kex "${KEX_INSTALL_TEST_PREFIX}/bin/kex")
foreach(mode IN ITEMS walker beam)
    if(mode STREQUAL "beam")
        set(backend -R)
    else()
        set(backend)
    endif()
    execute_process(
        COMMAND "${kex}" ${backend} --no-check --no-colors "${program}"
        WORKING_DIRECTORY "${KEX_INSTALL_TEST_PREFIX}"
        RESULT_VARIABLE run_result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT run_result EQUAL 0 OR NOT output STREQUAL "[1, 2]\n")
        message(FATAL_ERROR
            "installed ${mode} failed (${run_result}): ${output}${error}")
    endif()
endforeach()

file(REMOVE_RECURSE "${KEX_INSTALL_TEST_PREFIX}")
