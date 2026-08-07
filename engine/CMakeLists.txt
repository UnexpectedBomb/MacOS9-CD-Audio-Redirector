cmake_minimum_required(VERSION 3.13)
project(CDEngine C)

# Step 2 of the Phase-2 re-plan: a resident PowerPC engine, and an installer that
# proves residency and validates the descriptor WITHOUT modifying anything.
#
# PowerPC throughout — configure with the PPC toolchain:
#   cmake -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build
#
# Two products:
#   cd_engine.pef      the resident engine, shaped as a native driver so
#                      GetDriverMemoryFragment accepts it
#   CDPump_v8 a PPC app that embeds that PEF, holds its closure, and calls
#                      its DoDriverIO to validate
#
# The 68K generation (patch/) is retired: a 68K DRVR shell plus tail jump worked on the
# classic early driver and crashed the machine on all three attempts against the real
# ATAPI .AppleCD. See FINDINGS.md and PHASE2.md §8.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

# --- the engine PEF ---
# A native driver must have DoDriverIO BOTH exported AND as the fragment `main`.
# The linker's -e would drop it from the export table, so we export normally and
# patch `main` afterwards — same recipe the eSATA driver uses.
add_library(cd_engine SHARED cd_engine_driver.c)
set_target_properties(cd_engine PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/cd_engine.exp")
target_include_directories(cd_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(cd_engine DriverServicesLib NameRegistryLib InterfaceLib)

add_custom_command(OUTPUT cd_engine.pef
    COMMAND ${MAKE_PEF} libcd_engine.so -o cd_engine.pef
    COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py
            cd_engine.pef DoDriverIO
    DEPENDS cd_engine ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py)

# Hex-encode the PEF into a Rez 'data' resource the installer can Get1Resource.
add_custom_command(OUTPUT engine_pef.r
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/embed-pef.sh
            cd_engine.pef engine_pef.r cdPF 128
    DEPENDS cd_engine.pef ${CMAKE_CURRENT_SOURCE_DIR}/embed-pef.sh)

# --- the installer app ---
add_application(CDPump_v8
    cd_engine_install.c
    cd_pump_audio.c
    ../probes/common/cd_probe_common.c
    cd_engine_install.r
    ${CMAKE_CURRENT_BINARY_DIR}/engine_pef.r
    TYPE     "APPL"
    CREATOR  "CDei")
target_include_directories(CDPump_v8 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../probes/common)
# DriverLoaderLib supplies GetDriverMemoryFragment / SetDriverClosureMemory.
target_link_libraries(CDPump_v8 DriverLoaderLib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(cd_engine retrocrt)
    add_dependencies(CDPump_v8 retrocrt)
endif()

set(WRAP_APM $ENV{HOME}/Developer/claude-os9/client/scripts/wrap-apm.py)
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDPump_v8.img
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDPump_v8.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDPump_v8.img
            CDPump_v8
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDPump_v8.dsk ${WRAP_APM}
    COMMENT "Wrapping CDPump_v8.dsk -> .img" VERBATIM)
add_custom_target(CDPump_v8_disk ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDPump_v8.img)
add_dependencies(CDPump_v8_disk CDPump_v8_APPL)

add_custom_command(TARGET CDPump_v8_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/push-to-pi.sh
            ${CMAKE_CURRENT_BINARY_DIR} CDPump_v8
    COMMENT "Pushing CDPump_v8 to pi3.local" VERBATIM)

# --- the SHIPPING artifact: the same code, faceless ---------------------------
#
# Same sources, same PEF, CD_FACELESS=1 and a different SIZE resource. Built from one
# source tree on purpose: the thing that ships has to be the thing that was tested, and
# a parallel copy of an installer is exactly how the two drift apart.
add_application(CDAudioRedirector_v4
    cd_engine_install.c
    cd_pump_audio.c
    ../probes/common/cd_probe_common.c
    cd_engine_faceless.r
    ${CMAKE_CURRENT_BINARY_DIR}/engine_pef.r
    TYPE     "APPL"
    CREATOR  "CDar")
target_compile_definitions(CDAudioRedirector_v4 PRIVATE CD_FACELESS=1)
target_include_directories(CDAudioRedirector_v4 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../probes/common)
target_link_libraries(CDAudioRedirector_v4 DriverLoaderLib InterfaceLib)
if(TARGET retrocrt)
    add_dependencies(CDAudioRedirector_v4 retrocrt)
endif()

add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v4.img
    # Fail the build if the faceless flag word is not exactly 0x14C0. A Rez 'SIZE'
    # resource is sixteen anonymous booleans; getting one out of order is silent.
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-size-flags.sh
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v4.bin 14C0
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v4.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v4.img
            CDAudioRedirector_v4
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v4.dsk ${WRAP_APM}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-size-flags.sh
    COMMENT "Checking SIZE flags, then wrapping CDAudioRedirector_v4.dsk -> .img"
    VERBATIM)
add_custom_target(CDAudioRedirector_v4_disk ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v4.img)
add_dependencies(CDAudioRedirector_v4_disk CDAudioRedirector_v4_APPL)

add_custom_command(TARGET CDAudioRedirector_v4_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/push-to-pi.sh
            ${CMAKE_CURRENT_BINARY_DIR} CDAudioRedirector_v4
    COMMENT "Pushing CDAudioRedirector_v4 to pi3.local" VERBATIM)
