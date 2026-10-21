cmake_minimum_required(VERSION 3.22)
project(silent_public_contract LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core)
if(NOT TARGET silent_interfaces)
    add_subdirectory(../../src/interfaces interfaces)
endif()
add_executable(public_keys_contract
    keys_consumer.cpp
    ../../src/interfaces/keys.h
)
target_include_directories(public_keys_contract BEFORE PRIVATE generated_guard)
target_link_libraries(public_keys_contract PRIVATE silent_interfaces Qt6::Core)
enable_testing()
add_test(NAME public_keys_contract COMMAND public_keys_contract)
