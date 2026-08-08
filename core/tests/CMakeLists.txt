# GoogleTest for flip7core
# Build: cmake -DBUILD_TESTS=ON .. && make flip7core_tests

cmake_minimum_required(VERSION 3.16)
project(flip7core_tests LANGUAGES CXX)

find_package(GTest REQUIRED)

# Build the core library first (defines flip7core target)
set(BUILD_TESTS OFF)
add_subdirectory(..)

# Test executable - only include test files that actually exist
add_executable(flip7core_tests
    protocol_test.cpp
    serialization_test.cpp
    reconciliation_test.cpp
    countdown_test.cpp
)

target_link_libraries(flip7core_tests PRIVATE
    flip7core
    GTest::GTest
    GTest::Main
)

add_test(NAME flip7core_tests COMMAND flip7core_tests)