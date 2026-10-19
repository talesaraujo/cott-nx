# cott - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(cott LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(COTT_SOURCES
    src/main.cpp
    src/cott_ring_wait.cpp
)

if(WIN32)
    add_executable(cott WIN32 ${COTT_SOURCES})
else()
    add_executable(cott ${COTT_SOURCES})
endif()

rexglue_setup_target(cott GPU_PLUGINS xenos)

# PC build with the SDK as a subdirectory: rex_app.cpp includes imgui.h and rexruntime links imgui
# privately, so its include path does not reach the executable (same fix as nfsmw-nx). On Switch
# rexglue_switch.cmake handles it. The same goes for renderdoc_app.h.
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(cott PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(cott PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Nintendo Switch: cott.nro next to the ELF. On the SD card it goes in sdmc:/switch/cott-nx/ with the
# extracted disc beside it in game_root/ (src/cott_app.h looks for it there).
if(REXGLUE_PLATFORM_SWITCH)
    # LTO on the game code and ours (not on the SDK libraries), as in nfsmw-nx. Together with
    # tools/llamadas_directas.py (calls to functions without a hook go to __imp__, which is not weak) the
    # compiler can inline small game functions across files. Two LTO jobs: this machine has 15 GB of RAM.
    option(COTT_LTO "LTO on the game code and the app (Switch)" ON)
    if(COTT_LTO)
        foreach(target cott_recomp cott)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE -flto -fno-fat-lto-objects)
            endif()
        endforeach()
        target_link_options(cott PRIVATE -flto=2 -flto-partition=balanced)
    endif()
    # Homebrew menu icon: a 256x256 JPEG, as elf2nro expects. Not distributed; without it the NRO gets
    # libnx's default icon.
    set(COTT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../tools/switch/icon/cott_icon.jpg")
    set(COTT_ICON_ARG)
    if(EXISTS "${COTT_ICON}")
        set(COTT_ICON_ARG ICON "${COTT_ICON}")
    endif()
    rexglue_switch_add_nro(cott
        NAME "Crash of the Titans"
        AUTHOR "cott-nx"
        VERSION "0.0.1"
        ${COTT_ICON_ARG})
endif()
