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

add_application(CDPlayProbe_v17
    cd_play_probe.c
    ../common/cd_probe_common.c
    cd_play_probe.r
    TYPE     "APPL"
    CREATOR  "CDpp"
)

target_include_directories(CDPlayProbe_v17 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine   # cd_engine.h: the real CDEnginePublic
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)

# DriverLoaderLib supplies GetDriverInformation (the native-vs-classic
# classifier); everything else resolves from InterfaceLib.
target_link_libraries(CDPlayProbe_v17 DriverLoaderLib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(CDPlayProbe_v17 retrocrt)
endif()

set(WRAP_APM $ENV{HOME}/Developer/claude-os9/client/scripts/wrap-apm.py)
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDPlayProbe_v17.img
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDPlayProbe_v17.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDPlayProbe_v17.img
            CDPlayProbe_v17
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDPlayProbe_v17.dsk ${WRAP_APM}
    COMMENT "Wrapping CDPlayProbe_v17.dsk in Apple Partition Map -> CDPlayProbe_v17.img"
    VERBATIM)
add_custom_target(CDPlayProbe_v17_disk ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDPlayProbe_v17.img)
add_dependencies(CDPlayProbe_v17_disk CDPlayProbe_v17_APPL)

add_custom_command(TARGET CDPlayProbe_v17_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/stage-artifacts.sh
            ${CMAKE_CURRENT_BINARY_DIR} CDPlayProbe_v17
    COMMENT "Staging CDPlayProbe_v17"
    VERBATIM)

# ★ The version banner in every log is the TARGET NAME itself, injected here as a
# generator expression, never a literal in the .c file. On 2026-08-17 a v11 build
# announced itself as v10 because only the target had been bumped, and the log is
# how you know what ran. A source that lacks CD_ARTIFACT_NAME fails to compile, so
# a target missing from this list is a loud error rather than an unlabelled binary.
target_compile_definitions(CDPlayProbe_v17 PRIVATE
    CD_ARTIFACT_NAME=\"$<TARGET_PROPERTY:NAME>\")
