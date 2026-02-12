cmake_minimum_required(VERSION 3.10)
project(eventpp_examples CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# eventpp is header-only
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)

find_package(Threads REQUIRED)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(example_highperf example_highperf_eventqueue.cpp)
target_link_libraries(example_highperf Threads::Threads)

add_executable(example_ao_hsm example_active_object_hsm.cpp)
target_link_libraries(example_ao_hsm Threads::Threads)
