cmake_minimum_required(VERSION 3.13)
project(CDTraceRead C)

# Phase-0 recon app for the OS 9 CD Audio Redirector. A plain double-clickable
# PowerPC application: probes P2 (driver classification), P3 (audio tracks as
# files) and P4 (the DAE gate) all run at task level from a normal process, so
# none of this needs to be boot code. The interception INIT comes later, and only
# after P4 says GO.
#
#   cmake -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build
#
# The target name carries the version (CDTraceRead_v2) so the user can confirm at a
# glance which build ran. Bump it every iteration.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

add_application(CDTraceRead_v2
    cd_trace_read.c
    ../common/cd_probe_common.c
    cd_trace_read.r
    TYPE     "APPL"
    CREATOR  "CDtr"
)

target_include_directories(CDTraceRead_v2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)

# DriverLoaderLib supplies GetDriverInformation — the native-vs-classic
# classifier for P2. Everything else resolves from InterfaceLib.
target_link_libraries(CDTraceRead_v2 DriverLoaderLib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(CDTraceRead_v2 retrocrt)
endif()

# Wrap the raw HFS .dsk in an Apple Partition Map so OS 9 will mount it.
set(WRAP_APM $ENV{HOME}/Developer/claude-os9/client/scripts/wrap-apm.py)
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDTraceRead_v2.img
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDTraceRead_v2.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDTraceRead_v2.img
            CDTraceRead_v2
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDTraceRead_v2.dsk ${WRAP_APM}
    COMMENT "Wrapping CDTraceRead_v2.dsk in Apple Partition Map -> CDTraceRead_v2.img"
    VERBATIM)
add_custom_target(CDTraceRead_v2_disk ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDTraceRead_v2.img)
add_dependencies(CDTraceRead_v2_disk CDTraceRead_v2_APPL)

# Post-build: stage to the Pi share for the OS 9 machine to pick up over AFP.
# Non-fatal if the Pi is unreachable.
add_custom_command(TARGET CDTraceRead_v2_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/push-to-pi.sh
            ${CMAKE_CURRENT_BINARY_DIR} CDTraceRead_v2
    COMMENT "Pushing CDTraceRead_v2 to pi3.local:/home/csell/shared/"
    VERBATIM)
