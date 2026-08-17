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
# The target name carries the version (CDTraceRead_v7) so the user can confirm at a
# glance which build ran. Bump it every iteration.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

add_application(CDTraceRead_v7
    cd_trace_read.c
    ../common/cd_probe_common.c
    cd_trace_read.r
    TYPE     "APPL"
    CREATOR  "CDtr"
)

target_include_directories(CDTraceRead_v7 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)

# DriverLoaderLib supplies GetDriverInformation — the native-vs-classic
# classifier for P2. Everything else resolves from InterfaceLib.
target_link_libraries(CDTraceRead_v7 DriverLoaderLib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(CDTraceRead_v7 retrocrt)
endif()

# Wrap the raw HFS .dsk in an Apple Partition Map so OS 9 will mount it.
set(WRAP_APM $ENV{HOME}/Developer/claude-os9/client/scripts/wrap-apm.py)
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDTraceRead_v7.img
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDTraceRead_v7.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDTraceRead_v7.img
            CDTraceRead_v7
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDTraceRead_v7.dsk ${WRAP_APM}
    COMMENT "Wrapping CDTraceRead_v7.dsk in Apple Partition Map -> CDTraceRead_v7.img"
    VERBATIM)
add_custom_target(CDTraceRead_v7_disk ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDTraceRead_v7.img)
add_dependencies(CDTraceRead_v7_disk CDTraceRead_v7_APPL)

# Post-build: stage to the Pi share for the OS 9 machine to pick up over AFP.
# Non-fatal if the Pi is unreachable.
add_custom_command(TARGET CDTraceRead_v7_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/stage-artifacts.sh
            ${CMAKE_CURRENT_BINARY_DIR} CDTraceRead_v7
    COMMENT "Staging CDTraceRead_v7"
    VERBATIM)

# ★ The version banner in every log is the TARGET NAME itself, injected here as a
# generator expression, never a literal in the .c file. On 2026-08-17 a v11 build
# announced itself as v10 because only the target had been bumped, and the log is
# how you know what ran. A source that lacks CD_ARTIFACT_NAME fails to compile, so
# a target missing from this list is a loud error rather than an unlabelled binary.
target_compile_definitions(CDTraceRead_v7 PRIVATE
    CD_ARTIFACT_NAME=\"$<TARGET_PROPERTY:NAME>\")
