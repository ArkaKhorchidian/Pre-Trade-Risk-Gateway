cmake_minimum_required(VERSION 3.20)
project(pretrade_risk_gateway VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(RISKGATE_BUILD_TESTS "Build unit tests" ON)
option(RISKGATE_SANITIZE "Build tests with ASan + UBSan" ON)
option(RISKGATE_NATIVE "Compile with -march=native" OFF)
option(RISKGATE_WERROR "Treat warnings as errors" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(RISKGATE_WERROR)
    add_compile_options(-Werror)
endif()
if(RISKGATE_NATIVE)
    add_compile_options(-march=native)
endif()

# ── Library ───────────────────────────────────────────────────────────────────
add_library(riskgate STATIC
    src/risk_engine.cpp
    src/fix_gateway.cpp
)
target_include_directories(riskgate PUBLIC include)

# ── Tools ─────────────────────────────────────────────────────────────────────
add_executable(riskgate_replay tools/riskgate_replay.cpp)
target_link_libraries(riskgate_replay PRIVATE riskgate)

add_executable(riskgate_bench bench/bench_risk.cpp)
target_link_libraries(riskgate_bench PRIVATE riskgate)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(RISKGATE_BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
        )
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    # Separate library build with sanitizers so the bench stays uninstrumented.
    add_library(riskgate_test_lib STATIC src/risk_engine.cpp src/fix_gateway.cpp)
    target_include_directories(riskgate_test_lib PUBLIC include)

    add_executable(riskgate_tests
        tests/test_containers.cpp
        tests/test_config.cpp
        tests/test_risk_engine.cpp
        tests/test_fix_gateway.cpp
    )
    target_link_libraries(riskgate_tests PRIVATE riskgate_test_lib GTest::gtest_main)
    target_compile_definitions(riskgate_tests PRIVATE RISKGATE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    if(RISKGATE_SANITIZE)
        foreach(t riskgate_test_lib riskgate_tests)
            target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${t} PRIVATE -fsanitize=address,undefined)
        endforeach()
    endif()

    include(GoogleTest)
    gtest_discover_tests(riskgate_tests DISCOVERY_MODE PRE_TEST)
endif()
