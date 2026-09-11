cmake_minimum_required(VERSION 3.21)
project(streamfind_sdk_consumer LANGUAGES CXX)
find_package(streamfind CONFIG REQUIRED)

add_executable(example_plugin plugin.cpp)
target_compile_features(example_plugin PRIVATE cxx_std_20)
streamfind_add_plugin(example_plugin example)
