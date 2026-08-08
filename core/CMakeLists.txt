# Flip7 Core CMake Build
# Shared C++ library for protocol, game logic, and reconciliation

cmake_minimum_required(VERSION 3.16)
project(flip7core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform-neutral core - no Arduino/ESP32 dependencies
add_library(flip7core STATIC
    src/protocol.cpp
    src/serializer.cpp
    src/puzzle.cpp
    src/mastermind.cpp
    src/reconciliation.cpp
    src/active_game.cpp
    src/countdown_engine.cpp
)

target_include_directories(flip7core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/flip7>
        $<INSTALL_INTERFACE:include>
        $<INSTALL_INTERFACE:include/flip7>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(flip7core PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-unused-variable
)

# Install headers
install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

install(TARGETS flip7core
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# Testing
enable_testing()
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    find_package(GTest REQUIRED)
    
    add_executable(flip7core_tests
        tests/protocol_test.cpp
        tests/serialization_test.cpp
        tests/reconciliation_test.cpp
        tests/countdown_test.cpp
    )
    
    target_link_libraries(flip7core_tests PRIVATE
        flip7core
        GTest::GTest
        GTest::Main
    )
    
    add_test(NAME flip7core_tests COMMAND flip7core_tests)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/flip7coreConfig.cmake.in")
    include(CMakePackageConfigHelpers)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/flip7coreConfigVersion.cmake
        VERSION 1.0.0
        COMPATIBILITY SameMajorVersion
    )

    configure_package_config_file(
        cmake/flip7coreConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/flip7coreConfig.cmake
        INSTALL_DESTINATION lib/cmake/flip7core
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/flip7coreConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/flip7coreConfigVersion.cmake
        DESTINATION lib/cmake/flip7core
    )
endif()
