# X4VRMOD — top-level build.
# GPLv3 with a Linux-only X4 linking exception (see LICENSE, LICENSE.exception).
cmake_minimum_required(VERSION 3.20)
project(x4vrmod LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED) # headers; the layer does not link libvulkan

include_directories(${Vulkan_INCLUDE_DIRS})

add_subdirectory(layer)
add_subdirectory(injector)
# add_subdirectory(cursor)   # later phase
