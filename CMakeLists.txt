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
)

target_include_directories(kex_lib PUBLIC src)

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
