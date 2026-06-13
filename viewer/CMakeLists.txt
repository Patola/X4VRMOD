cmake_minimum_required(VERSION 3.16)
project(x4vr_viewer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Vulkan REQUIRED)
find_package(OpenXR REQUIRED)

add_executable(x4vr-viewer src/main.c)
target_compile_definitions(x4vr-viewer PRIVATE XR_USE_GRAPHICS_API_VULKAN)
target_compile_options(x4vr-viewer PRIVATE -Wall -Wextra)
target_include_directories(x4vr-viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared)
target_link_libraries(x4vr-viewer PRIVATE Vulkan::Vulkan OpenXR::openxr_loader m rt)
