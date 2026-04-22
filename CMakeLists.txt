cmake_minimum_required(VERSION 3.20)
project(raft)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

# main binary
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/raft_node.cpp
    src/network.cpp
    src/randomizer.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined -g)
target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined)

# tests
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

add_executable(raft_tests
    tests/election_test.cpp
    tests/log_replication_test.cpp
    src/raft_node.cpp
    src/network.cpp
    src/randomizer.cpp
)
target_include_directories(raft_tests PRIVATE src)
target_link_libraries(raft_tests GTest::gtest_main)
target_compile_options(raft_tests PRIVATE -fsanitize=thread,undefined)
target_link_options(raft_tests PRIVATE -fsanitize=thread,undefined)

include(GoogleTest)
gtest_discover_tests(raft_tests)
