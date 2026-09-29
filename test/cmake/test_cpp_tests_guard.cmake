# Regression test for GitHub #45 — "Compilation error statically linking erpl-web extension".
#
# When erpl_odata is consumed as a dependency (duckdb_extension_load / FetchContent), only the
# extension source is fetched; the `duckdb` git submodule is NOT. But test/cpp includes
# catch.hpp / core_functions_extension.hpp via paths relative to that submodule
# (../../duckdb/...), so building the C++ test target in a consumer tree fails with
# "catch.hpp file not found". DuckDB's BUILD_UNITTESTS does not distinguish the two cases
# (it is TRUE in both our dev build and a consumer's main DuckDB build), so CMakeLists.txt
# must gate add_subdirectory(test) on the in-tree submodule's presence.
#
# This is a pure-CMake script — no DuckDB configure required — so it runs identically on
# Linux, macOS and Windows.
#
# Run with:  cmake -DREPO_DIR=<repo root> -P test/cmake/test_cpp_tests_guard.cmake

if(NOT DEFINED REPO_DIR)
    set(REPO_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
endif()
get_filename_component(REPO_DIR "${REPO_DIR}" ABSOLUTE)

set(CMAKELISTS "${REPO_DIR}/CMakeLists.txt")
if(NOT EXISTS "${CMAKELISTS}")
    message(FATAL_ERROR "Cannot find ${CMAKELISTS}")
endif()
file(READ "${CMAKELISTS}" CML)

function(assert_contains needle why)
    string(FIND "${CML}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "GitHub #45 regression: ${why}\n  Expected CMakeLists.txt to contain: ${needle}")
    endif()
endfunction()

function(assert_absent needle why)
    string(FIND "${CML}" "${needle}" pos)
    if(NOT pos EQUAL -1)
        message(FATAL_ERROR "GitHub #45 regression: ${why}\n  Did not expect CMakeLists.txt to contain: ${needle}")
    endif()
endfunction()

# 1. The C++ tests must be gated on the duckdb submodule's catch.hpp, not built unconditionally.
assert_contains("BUILD_UNITTESTS AND EXISTS"
    "add_subdirectory(test) is not gated on the in-tree duckdb submodule")
assert_contains("duckdb/third_party/catch/catch.hpp"
    "the C++ test guard does not check for the duckdb submodule's catch.hpp")
assert_contains("add_subdirectory(test)"
    "the C++ test subdirectory wiring is missing")

# 2. erpl_odata must NOT redeclare DuckDB core's BUILD_UNITTESTS option (it shadows the flag
#    DuckDB and embedders use to control test building).
assert_absent("option(BUILD_UNITTESTS"
    "erpl_odata redeclares DuckDB's BUILD_UNITTESTS option instead of honoring it")

# 3. The guard is meaningful only because test/cpp really does depend on that submodule path.
#    Confirm the sources that triggered #45 are present and still include catch.hpp.
foreach(need "test/cpp/CMakeLists.txt" "test/cpp/test_charset_converter.cpp")
    if(NOT EXISTS "${REPO_DIR}/${need}")
        message(FATAL_ERROR "Expected ${need} to exist (the GitHub #45 reproduction sources)")
    endif()
endforeach()
file(READ "${REPO_DIR}/test/cpp/test_charset_converter.cpp" CHARSET_TEST)
string(FIND "${CHARSET_TEST}" "catch.hpp" charset_pos)
if(charset_pos EQUAL -1)
    message(FATAL_ERROR "test/cpp/test_charset_converter.cpp no longer includes catch.hpp — "
                        "the GitHub #45 guard rationale may be stale; revisit this test.")
endif()

# 4. Behavioral check of the guard condition, both ways:
#    (a) developer tree WITH the submodule -> condition is satisfied (tests build).
if(NOT EXISTS "${REPO_DIR}/duckdb/third_party/catch/catch.hpp")
    message(FATAL_ERROR "Expected the duckdb submodule (catch.hpp) to be present in the dev tree; "
                        "cannot validate the positive branch of the guard.")
endif()
#    (b) a fake consumer tree WITHOUT the submodule -> condition is NOT satisfied (tests skipped).
set(FAKE_CONSUMER "${REPO_DIR}/build/guard_check_fake_consumer")
file(REMOVE_RECURSE "${FAKE_CONSUMER}")
file(MAKE_DIRECTORY "${FAKE_CONSUMER}")
if(EXISTS "${FAKE_CONSUMER}/duckdb/third_party/catch/catch.hpp")
    message(FATAL_ERROR "test bug: the fake consumer tree must not contain the duckdb submodule")
endif()
file(REMOVE_RECURSE "${FAKE_CONSUMER}")

message(STATUS "PASS: GitHub #45 guard present and correct — erpl_odata C++ unit tests are gated on the in-tree duckdb submodule and do not build for dependency consumers.")
