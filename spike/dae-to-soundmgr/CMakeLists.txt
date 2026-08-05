cmake_minimum_required(VERSION 3.13)
project(CDAudioSpike C)

# Phase 1: join the two halves Phase 0 proved separately — read CD-DA off the drive
# and play it through the Sound Manager. Still an ordinary application, because
# nothing here needs to be resident: the interception INIT is Phase 2, and it will
# reuse this engine.
#
#   cmake -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

add_application(CDAudioSpike_v1
    cd_audio_spike.c
    ../../probes/common/cd_probe_common.c
    cd_audio_spike.r
    TYPE     "APPL"
    CREATOR  "CDas"
)

target_include_directories(CDAudioSpike_v1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../probes/common)

# DriverLoaderLib for GetDriverInformation (used by the shared DCE dump);
# the Sound Manager and everything else resolve from InterfaceLib.
target_link_libraries(CDAudioSpike_v1 DriverLoaderLib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(CDAudioSpike_v1 retrocrt)
endif()

set(WRAP_APM $ENV{HOME}/Developer/claude-os9/client/scripts/wrap-apm.py)
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDAudioSpike_v1.img
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioSpike_v1.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioSpike_v1.img
            CDAudioSpike_v1
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDAudioSpike_v1.dsk ${WRAP_APM}
    COMMENT "Wrapping CDAudioSpike_v1.dsk in Apple Partition Map -> CDAudioSpike_v1.img"
    VERBATIM)
add_custom_target(CDAudioSpike_v1_disk ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDAudioSpike_v1.img)
add_dependencies(CDAudioSpike_v1_disk CDAudioSpike_v1_APPL)

add_custom_command(TARGET CDAudioSpike_v1_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/push-to-pi.sh
            ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Pushing CDAudioSpike_v1 to pi3.local:/home/csell/shared/"
    VERBATIM)
