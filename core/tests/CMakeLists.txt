# GoogleTest for flip7core
# Build: cmake -DBUILD_TESTS=ON .. && make flip7core_tests

cmake_minimum_required(VERSION 3.16)
project(flip7core_tests LANGUAGES CXX)

find_package(GTest REQUIRED)

# Test executable
add_executable(flip7core_tests
    protocol_test.cpp
    serialization_test.cpp
    reconciliation_test.cpp
    mastermind_test.cpp
    puzzle_test.cpp
    golden_packet_test.cpp
)

target_link_libraries(flip7core_tests PRIVATE
    flip7core
    GTest::GTest
    GTest::Main
)

add_test(NAME flip7core_tests COMMAND flip7core_tests)

# Also build the main library with tests enabled
set(BUILD_TESTS ON)
add_subdirectory(..)