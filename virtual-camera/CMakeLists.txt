cmake_minimum_required(VERSION 3.20)
project(boulecam-vcam LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
)

# 1. Virtual Camera COM DLL
set(DLL_SOURCES
    src/dllmain.cpp
    src/ipc/shm_consumer.cpp
    src/mf/media_stream.cpp
    src/mf/virtual_camera_source.cpp
    src/mf/class_factory.cpp
)

add_library(boulecam-vcam SHARED ${DLL_SOURCES})

target_link_libraries(boulecam-vcam PRIVATE
    mfplat
    mfuuid
    mf
    shlwapi
)

# 2. Registration CLI Tool
add_executable(register_vcam src/installer/register_vcam.cpp)

target_link_libraries(register_vcam PRIVATE
    mfplat
    mfuuid
    mf
    mfsensorgroup
)
