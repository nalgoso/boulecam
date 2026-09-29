cmake_minimum_required(VERSION 3.20)
project(boulecam-vcam LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
)

# 1. Virtual Camera DirectShow COM DLL
set(DLL_SOURCES
    src/dllmain.cpp
    src/dshow_filter.cpp
    src/ipc/shm_consumer.cpp
)

add_library(boulecam-vcam SHARED ${DLL_SOURCES})
set_target_properties(boulecam-vcam PROPERTIES OUTPUT_NAME "boulecam-vcam" PREFIX "")
target_link_options(boulecam-vcam PRIVATE -static)

target_link_libraries(boulecam-vcam PRIVATE
    strmiids
    ole32
    oleaut32
    uuid
    shlwapi
)

# 2. Registration CLI Tool
add_executable(register_vcam src/installer/register_vcam.cpp)
set_target_properties(register_vcam PROPERTIES OUTPUT_NAME "register_vcam")
target_link_options(register_vcam PRIVATE -static)

target_link_libraries(register_vcam PRIVATE
    ole32
    oleaut32
    uuid
    shlwapi
)
