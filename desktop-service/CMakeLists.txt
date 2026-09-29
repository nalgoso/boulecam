cmake_minimum_required(VERSION 3.20)
project(boulecam-desktop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000003
    WINVER=0x0A00
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)


include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
)

set(SOURCES
    src/main.cpp
    src/ipc/shm_producer.cpp
    src/network/tcp_receiver.cpp
    src/network/http_bridge.cpp
    src/network/discovery_beacon.cpp
    src/usb/adb_manager.cpp
    src/usb/usbmuxd_client.cpp
    src/decoder/video_decoder.cpp
    src/audio/audio_output_manager.cpp
)

add_executable(boulecam-desktop ${SOURCES})

target_link_libraries(boulecam-desktop PRIVATE
    ws2_32
    iphlpapi
    mfplat
    mfuuid
    wmcodecdspuuid
    strmiids
    d3d11
    dxgi
    winmm
)

target_link_options(boulecam-desktop PRIVATE
    -static
)

