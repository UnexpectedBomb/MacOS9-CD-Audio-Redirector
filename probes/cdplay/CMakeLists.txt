cmake_minimum_required(VERSION 3.13)
project(CDPlayProbe C)

# Phase-0 probe P5a: drive the legacy CD-audio API directly and log what the
# driver does. This replaces the "trace a running game" step for the purpose of
# settling H1 vs H2, because that question is about the driver, not the game — and
# answering it from an ordinary app costs a double-click instead of a reboot.
#
#   cmake -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

add_application(CDPlayProbe_v1
    cd_play_probe.c
    ../common/cd_probe_common.c
    cd_play_probe.r
    TYPE     "APPL"
    CREATOR  "CDpp"
)

target_include_directories(CDPlayProbe_v1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)

# DriverLoaderLib supplies GetDriverInformation (the native-vs-classic
# classifier); everything else resolves from InterfaceLib.
target_link_libraries(CDPlayProbe_v1 DriverLoaderLib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(CDPlayProbe_v1 retrocrt)
endif()

set(WRAP_APM $ENV{HOME}/Developer/claude-os9/client/scripts/wrap-apm.py)
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDPlayProbe_v1.img
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDPlayProbe_v1.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDPlayProbe_v1.img
            CDPlayProbe_v1
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDPlayProbe_v1.dsk ${WRAP_APM}
    COMMENT "Wrapping CDPlayProbe_v1.dsk in Apple Partition Map -> CDPlayProbe_v1.img"
    VERBATIM)
add_custom_target(CDPlayProbe_v1_disk ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDPlayProbe_v1.img)
add_dependencies(CDPlayProbe_v1_disk CDPlayProbe_v1_APPL)

add_custom_command(TARGET CDPlayProbe_v1_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/push-to-pi.sh
            ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Pushing CDPlayProbe_v1 to pi3.local:/home/csell/shared/"
    VERBATIM)
