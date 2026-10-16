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
)

if(WIN32)
    add_executable(cott WIN32 ${COTT_SOURCES})
else()
    add_executable(cott ${COTT_SOURCES})
endif()

rexglue_setup_target(cott)
