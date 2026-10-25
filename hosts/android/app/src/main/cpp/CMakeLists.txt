# Point the Android build at the shared Any_Ps3 core (repo root).
cmake_minimum_required(VERSION 3.22)
project(anyps3_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find the repository root by searching upward for core/include/anyps3/types.h
# (robust against layout changes; this file lives at hosts/android/app/src/main/cpp).
set(ANYPS3_ROOT "" CACHE PATH "Any_Ps3 repository root")
if(NOT ANYPS3_ROOT)
    set(_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    while(NOT EXISTS "${_dir}/core/include/anyps3/types.h")
        get_filename_component(_dir "${_dir}/.." REALPATH)
        if("${_dir}" STREQUAL "/")
            message(FATAL_ERROR "Any_Ps3 core not found; set -DANYPS3_ROOT=<repo>")
        endif()
    endwhile()
    set(ANYPS3_ROOT "${_dir}")
endif()
message(STATUS "Any_Ps3 core root: ${ANYPS3_ROOT}")

# Version from the single VERSION file at the repo root — the same string the
# Gradle build uses for versionName. Injected as ANYPS3_VERSION so the core
# and the JNI bridge report the identical version inside the app.
file(READ "${ANYPS3_ROOT}/VERSION" ANYPS3_VERSION_TEXT)
string(STRIP "${ANYPS3_VERSION_TEXT}" ANYPS3_VERSION_TEXT)
if(ANYPS3_VERSION_TEXT STREQUAL "")
  message(FATAL_ERROR "Any_Ps3: VERSION file is empty or missing")
endif()
message(STATUS "Any_Ps3 version (from VERSION file): ${ANYPS3_VERSION_TEXT}")
# Re-configure when the VERSION file changes, so version bumps always reach
# the compiled-in string (without this, the NDK build cache kept stale flags).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ANYPS3_ROOT}/VERSION")
add_compile_definitions(ANYPS3_VERSION="${ANYPS3_VERSION_TEXT}")

file(GLOB_RECURSE ANYPS3_CORE_SOURCES ${ANYPS3_ROOT}/core/src/*.cpp)
if(NOT ANYPS3_CORE_SOURCES)
    message(FATAL_ERROR "no core sources found under ${ANYPS3_ROOT}/core/src")
endif()
add_library(anyps3_core STATIC ${ANYPS3_CORE_SOURCES})
target_include_directories(anyps3_core PUBLIC ${ANYPS3_ROOT}/core/include)

# JNI bridge
add_library(anyps3 SHARED
    anyps3_jni.cpp)
target_link_libraries(anyps3 PRIVATE anyps3_core)
target_include_directories(anyps3 PRIVATE ${ANYPS3_ROOT}/core/include)

find_library(log-lib log)
target_link_libraries(anyps3 PRIVATE ${log-lib} android)
