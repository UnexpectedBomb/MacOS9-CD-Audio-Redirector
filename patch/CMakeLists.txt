cmake_minimum_required(VERSION 3.13)
project(CDPatch2a C)

# Phase 2a: the CD driver Control-entry patch, trace only, no audio.
#
# 68K, so configure with the m68k toolchain — NOT the PowerPC one used by the probes:
#   cmake -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain-m68k/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build build
#
# Two flat code resources, combined by Rez into one extension file. Use
# add_executable with --mac-flat, not add_application: these are code resources, not
# applications. Each OUTPUT_NAME must match the filename cd_patch.r $$reads.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

# --- the installer INIT ---
add_executable(cd_patch_init cd_patch_init.c)
set_target_properties(cd_patch_init PROPERTIES
    OUTPUT_NAME cd_patch_init.flt
    LINK_FLAGS  -Wl,--mac-flat)
target_include_directories(cd_patch_init PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# --- the resident patch blob ---
add_executable(cd_patch_blob cd_patch_blob.c)
set_target_properties(cd_patch_blob PROPERTIES
    OUTPUT_NAME cd_patch_blob.flt
    LINK_FLAGS  -Wl,--mac-flat)
target_include_directories(cd_patch_blob PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

if(TARGET retrocrt)
    add_dependencies(cd_patch_init retrocrt)
    add_dependencies(cd_patch_blob retrocrt)
endif()

# Rez needs the RIncludes path explicitly, or the first #include in the .r fails.
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/CDPatch2a.bin
    COMMAND ${REZ} -I $ENV{HOME}/Retro68-build/toolchain-m68k/RIncludes
            ${CMAKE_CURRENT_SOURCE_DIR}/cd_patch.r
            -o ${CMAKE_CURRENT_BINARY_DIR}/CDPatch2a.bin
            -t INIT -c CDau
    DEPENDS cd_patch_init cd_patch_blob ${CMAKE_CURRENT_SOURCE_DIR}/cd_patch.r
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Rez: INIT 128 + CDpt 128 -> CDPatch2a.bin"
    VERBATIM)
add_custom_target(CDPatch2a ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/CDPatch2a.bin)

add_custom_command(TARGET CDPatch2a POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/stage-artifacts.sh
            ${CMAKE_CURRENT_BINARY_DIR} CDPatchInstall
    COMMENT "Staging CDPatch2a"
    VERBATIM)

# --- on-demand installer app (68K, so it can $$read the same blob) ---
# The first hardware boot refused to patch because no CD driver existed yet during
# the extension parade. This app performs the identical install from a normal
# application, after everything has loaded, so the patch mechanics are testable
# without a reboot per attempt.
add_application(CDPatchInstall
    cd_patch_install.c
    cd_patch_install.r
    TYPE    "APPL"
    CREATOR "CDpi")
target_include_directories(CDPatchInstall PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_dependencies(CDPatchInstall_APPL cd_patch_blob)
