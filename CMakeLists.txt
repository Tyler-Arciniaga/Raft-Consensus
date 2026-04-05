cmake_minimum_required(VERSION 4.3.1)

project(raft)

add_executable(${PROJECT_NAME} src/main.cpp src/raft_node.cpp src/network.cpp src/rpc.h src/randomizer.cpp)

target_compile_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined -g)
target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined)
