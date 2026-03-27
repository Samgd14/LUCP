cmake_minimum_required(VERSION 3.10)
project(LUCP_CPP VERSION 0.2)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

enable_testing()

add_executable(test_node tests/test_node.cpp)
add_test(NAME test_node COMMAND test_node)

add_executable(test_core tests/test_core.cpp)
add_test(NAME test_core COMMAND test_core)

add_executable(test_message tests/test_message.cpp)
add_test(NAME test_message COMMAND test_message)

add_executable(test_ack_echo tests/test_ack_echo.cpp)
add_test(NAME test_ack_echo COMMAND test_ack_echo)
