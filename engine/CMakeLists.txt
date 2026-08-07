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
#   CDPump_v13 a PPC app that embeds that PEF, holds its closure, and calls
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
add_application(CDPump_v13
    cd_engine_install.c
    cd_pump_audio.c
    ../probes/common/cd_probe_common.c
    cd_engine_install.r
    ${CMAKE_CURRENT_BINARY_DIR}/engine_pef.r
    TYPE     "APPL"
    CREATOR  "CDei")
target_include_directories(CDPump_v13 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../probes/common)
# DriverLoaderLib supplies GetDriverMemoryFragment / SetDriverClosureMemory.
target_link_libraries(CDPump_v13 DriverLoaderLib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(cd_engine retrocrt)
    add_dependencies(CDPump_v13 retrocrt)
endif()

set(WRAP_APM $ENV{HOME}/Developer/claude-os9/client/scripts/wrap-apm.py)
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDPump_v13.img
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDPump_v13.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDPump_v13.img
            CDPump_v13
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDPump_v13.dsk ${WRAP_APM}
    COMMENT "Wrapping CDPump_v13.dsk -> .img" VERBATIM)
add_custom_target(CDPump_v13_disk ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDPump_v13.img)
add_dependencies(CDPump_v13_disk CDPump_v13_APPL)

add_custom_command(TARGET CDPump_v13_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/push-to-pi.sh
            ${CMAKE_CURRENT_BINARY_DIR} CDPump_v13
    COMMENT "Pushing CDPump_v13 to pi3.local" VERBATIM)

# --- the SHIPPING artifact: the same code, faceless ---------------------------
#
# Same sources, same PEF, CD_FACELESS=1 and a different SIZE resource. Built from one
# source tree on purpose: the thing that ships has to be the thing that was tested, and
# a parallel copy of an installer is exactly how the two drift apart.
add_application(CDAudioRedirector_v9
    cd_engine_install.c
    cd_pump_audio.c
    ../probes/common/cd_probe_common.c
    cd_engine_faceless.r
    ${CMAKE_CURRENT_BINARY_DIR}/engine_pef.r
    TYPE     "APPL"
    CREATOR  "CDar")
target_compile_definitions(CDAudioRedirector_v9 PRIVATE CD_FACELESS=1)
target_include_directories(CDAudioRedirector_v9 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../probes/common)
target_link_libraries(CDAudioRedirector_v9 DriverLoaderLib InterfaceLib)
if(TARGET retrocrt)
    add_dependencies(CDAudioRedirector_v9 retrocrt)
endif()

add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v9.img
    # Fail the build if the faceless flag word is not exactly 0x14C0. A Rez 'SIZE'
    # resource is sixteen anonymous booleans; getting one out of order is silent.
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-size-flags.sh
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v9.bin 14C0
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v9.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v9.img
            CDAudioRedirector_v9
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v9.dsk ${WRAP_APM}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-size-flags.sh
    COMMENT "Checking SIZE flags, then wrapping CDAudioRedirector_v9.dsk -> .img"
    VERBATIM)
add_custom_target(CDAudioRedirector_v9_disk ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_v9.img)
add_dependencies(CDAudioRedirector_v9_disk CDAudioRedirector_v9_APPL)

add_custom_command(TARGET CDAudioRedirector_v9_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/push-to-pi.sh
            ${CMAKE_CURRENT_BINARY_DIR} CDAudioRedirector_v9
    COMMENT "Pushing CDAudioRedirector_v9 to pi3.local" VERBATIM)

# --- bisectB: the good behaviour, on a SMALL allocation ----------------------
#
# bisectA answered the previous question: v1's behaviour on v4's memory still froze, so
# the drain loop and the variable-offset write are innocent and the SIZE of the block is
# what tracks the fault. bisectB therefore keeps the behaviour that was exonerated -
# drain every request, nothing dropped - and shrinks the ring from 16 entries to 2, so
# the published block goes from 456 bytes back to 176, near v1's ~148.
#
# Unlike bisectA this is a potential SHIPPING candidate rather than a pure diagnostic:
# two slots still fix the realistic Stop-then-Play drop, and reqDropped still reports
# any overflow, which the ship gate requires. Whether two slots are ENOUGH is a separate
# question from whether this freezes.
add_application(CDAudioRedirector_bisectB
    cd_engine_install.c
    cd_pump_audio.c
    ../probes/common/cd_probe_common.c
    cd_engine_faceless.r
    ${CMAKE_CURRENT_BINARY_DIR}/engine_pef_ring2.r
    TYPE     "APPL"
    CREATOR  "CDbB")
target_compile_definitions(CDAudioRedirector_bisectB PRIVATE CD_FACELESS=1 kEngineReqRingEntries=2)
target_include_directories(CDAudioRedirector_bisectB PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../probes/common)
target_link_libraries(CDAudioRedirector_bisectB DriverLoaderLib InterfaceLib)
if(TARGET retrocrt)
    add_dependencies(CDAudioRedirector_bisectB retrocrt)
endif()

# The RESIDENT engine must be built with the same switch, since the producer lives in
# it. A bisect whose two halves disagree measures nothing.
add_library(cd_engine_ring2 SHARED cd_engine_driver.c)
target_compile_definitions(cd_engine_ring2 PRIVATE kEngineReqRingEntries=2)
set_target_properties(cd_engine_ring2 PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/cd_engine.exp")
target_include_directories(cd_engine_ring2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(cd_engine_ring2 DriverServicesLib NameRegistryLib InterfaceLib)
add_custom_command(OUTPUT cd_engine_ring2.pef
    COMMAND ${MAKE_PEF} libcd_engine_ring2.so -o cd_engine_ring2.pef
    COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py
            cd_engine_ring2.pef DoDriverIO
    DEPENDS cd_engine_ring2 ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py)
add_custom_command(OUTPUT engine_pef_ring2.r
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/embed-pef.sh
            cd_engine_ring2.pef engine_pef_ring2.r cdPF 128
    DEPENDS cd_engine_ring2.pef ${CMAKE_CURRENT_SOURCE_DIR}/embed-pef.sh)

add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectB.img
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-size-flags.sh
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectB.bin 14C0
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectB.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectB.img
            CDAudioRedirector_bisectB
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectB.dsk ${WRAP_APM}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-size-flags.sh
    COMMENT "Checking SIZE flags, then wrapping CDAudioRedirector_bisectB" VERBATIM)
add_custom_target(CDAudioRedirector_bisectB_disk ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectB.img)
add_dependencies(CDAudioRedirector_bisectB_disk CDAudioRedirector_bisectB_APPL)
add_custom_command(TARGET CDAudioRedirector_bisectB_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/push-to-pi.sh
            ${CMAKE_CURRENT_BINARY_DIR} CDAudioRedirector_bisectB
    COMMENT "Pushing CDAudioRedirector_bisectB to pi3.local" VERBATIM)

# --- bisectC: same, with FOUR slots -----------------------------------------
#
# bisectB (2 slots, 188 bytes) was stable but OVERFLOWED once per probe run: three
# requests arrive inside one pump pass, and two slots cannot hold three. Four slots
# covers the observed burst with one to spare, at 228 bytes - a 40-byte step from a
# size known to be safe on this machine, rather than a leap back to 468. ----------------------
#
# bisectA answered the previous question: v1's behaviour on v4's memory still froze, so
# the drain loop and the variable-offset write are innocent and the SIZE of the block is
# what tracks the fault. bisectC therefore keeps the behaviour that was exonerated -
# drain every request, nothing dropped - and shrinks the ring from 16 entries to 4, so
# the published block goes to 228 bytes.
#
# Unlike bisectA this is a potential SHIPPING candidate rather than a pure diagnostic:
# two slots still fix the realistic Stop-then-Play drop, and reqDropped still reports
# any overflow, which the ship gate requires. Whether two slots are ENOUGH is a separate
# question from whether this freezes.
add_application(CDAudioRedirector_bisectC
    cd_engine_install.c
    cd_pump_audio.c
    ../probes/common/cd_probe_common.c
    cd_engine_faceless.r
    ${CMAKE_CURRENT_BINARY_DIR}/engine_pef_ring4.r
    TYPE     "APPL"
    CREATOR  "CDbC")
target_compile_definitions(CDAudioRedirector_bisectC PRIVATE CD_FACELESS=1 kEngineReqRingEntries=4)
target_include_directories(CDAudioRedirector_bisectC PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../probes/common)
target_link_libraries(CDAudioRedirector_bisectC DriverLoaderLib InterfaceLib)
if(TARGET retrocrt)
    add_dependencies(CDAudioRedirector_bisectC retrocrt)
endif()

# The RESIDENT engine must be built with the same switch, since the producer lives in
# it. A bisect whose two halves disagree measures nothing.
add_library(cd_engine_ring4 SHARED cd_engine_driver.c)
target_compile_definitions(cd_engine_ring4 PRIVATE kEngineReqRingEntries=4)
set_target_properties(cd_engine_ring4 PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/cd_engine.exp")
target_include_directories(cd_engine_ring4 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(cd_engine_ring4 DriverServicesLib NameRegistryLib InterfaceLib)
add_custom_command(OUTPUT cd_engine_ring4.pef
    COMMAND ${MAKE_PEF} libcd_engine_ring4.so -o cd_engine_ring4.pef
    COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py
            cd_engine_ring4.pef DoDriverIO
    DEPENDS cd_engine_ring4 ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py)
add_custom_command(OUTPUT engine_pef_ring4.r
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/embed-pef.sh
            cd_engine_ring4.pef engine_pef_ring4.r cdPF 128
    DEPENDS cd_engine_ring4.pef ${CMAKE_CURRENT_SOURCE_DIR}/embed-pef.sh)

add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectC.img
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-size-flags.sh
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectC.bin 14C0
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectC.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectC.img
            CDAudioRedirector_bisectC
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectC.dsk ${WRAP_APM}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-size-flags.sh
    COMMENT "Checking SIZE flags, then wrapping CDAudioRedirector_bisectC" VERBATIM)
add_custom_target(CDAudioRedirector_bisectC_disk ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDAudioRedirector_bisectC.img)
add_dependencies(CDAudioRedirector_bisectC_disk CDAudioRedirector_bisectC_APPL)
add_custom_command(TARGET CDAudioRedirector_bisectC_disk POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/push-to-pi.sh
            ${CMAKE_CURRENT_BINARY_DIR} CDAudioRedirector_bisectC
    COMMENT "Pushing CDAudioRedirector_bisectC to pi3.local" VERBATIM)

# NOTE: the former bisectD target is gone - its configuration IS the shipping build
# now (CD_RING_SEPARATE defaults to 1). bisectB (2 slots) and bisectC (4 slots) are kept
# above as the inline-ring data points that established the threshold.
