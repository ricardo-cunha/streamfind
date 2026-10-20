cmake_minimum_required(VERSION 3.24)
project(raman_wip_export_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(raman_wip_export_check
    main.cpp
    ../../../../cpp/plugins/raman/src/readers/text_reader.cpp
    ../../../../cpp/plugins/raman/src/readers/wip_reader.cpp)
target_include_directories(raman_wip_export_check PRIVATE
    ../../../../cpp/plugins/raman/src)
