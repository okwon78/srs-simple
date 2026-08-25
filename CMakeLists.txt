# srs_simple — 교육용 단순화 RTMP 서버 (원본: https://github.com/ossrs/srs)
# 원본은 bash configure 스크립트를 쓰지만, 학습 가치가 없어 CMake로 교체 (CLAUDE.md §6).
cmake_minimum_required(VERSION 3.14)
project(srs_simple CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# VSCode IntelliSense(.vscode/c_cpp_properties.json의 compileCommands)가 읽는다.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
# -Werror=parentheses: `if (x = C)` 오타를 경고가 아니라 에러로 만든다.
# Yoda 조건(`C == x`)이 지키려던 것을 컴파일러가 대신하므로 코드는 원본 SRS와 같은 방향을 유지한다.
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Werror=parentheses)

find_package(Threads REQUIRED)

# 원본과 동일하게 <srs_kernel_error.hpp> 식의 플랫 인클루드를 쓴다.
file(GLOB_RECURSE SRS_SOURCES CONFIGURE_DEPENDS
    src/core/*.cpp src/kernel/*.cpp src/protocol/*.cpp src/app/*.cpp)
add_library(srs_simple_lib STATIC ${SRS_SOURCES})
target_include_directories(srs_simple_lib PUBLIC
    src/core src/kernel src/protocol src/app)
target_link_libraries(srs_simple_lib PUBLIC Threads::Threads)

add_executable(srs_simple src/main/srs_main_server.cpp)
target_link_libraries(srs_simple PRIVATE srs_simple_lib)

# 유닛테스트 — gtest는 FetchContent로 수급 (1.12.1이 C++11을 지원하는 마지막 릴리스)
include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/release-1.12.1.zip)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
file(GLOB SRS_UTEST_SOURCES CONFIGURE_DEPENDS utest/*.cpp)
add_executable(srs_utest ${SRS_UTEST_SOURCES})
target_include_directories(srs_utest PRIVATE utest)
target_link_libraries(srs_utest PRIVATE srs_simple_lib gtest)
add_test(NAME srs_utest COMMAND srs_utest)
