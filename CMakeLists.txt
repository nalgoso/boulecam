cmake_minimum_required(VERSION 3.20)
project(BouleCam LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared definitions include path
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/shared/include)

if(WIN32)
    # Enable Unicode
    add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

# Output all compiled binaries directly into build/<Config>/ (e.g. build/Release/)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")

# Subdirectories
add_subdirectory(desktop-service)
add_subdirectory(virtual-camera)
