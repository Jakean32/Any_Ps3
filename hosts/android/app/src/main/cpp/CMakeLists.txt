# Point the Android build at the shared Any_Ps3 core (repo root).
cmake_minimum_required(VERSION 3.22)
project(anyps3_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The core library lives at the repository root: ../../../../../.. from this file.
get_filename_component(ANYPS3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../" ABSOLUTE)

file(GLOB_RECURSE ANYPS3_CORE_SOURCES ${ANYPS3_ROOT}/core/src/*.cpp)
add_library(anyps3_core STATIC ${ANYPS3_CORE_SOURCES})
target_include_directories(anyps3_core PUBLIC ${ANYPS3_ROOT}/core/include)

# JNI bridge
add_library(anyps3 SHARED
    anyps3_jni.cpp)
target_link_libraries(anyps3 PRIVATE anyps3_core log android)
target_include_directories(anyps3 PRIVATE ${ANYPS3_ROOT}/core/include)

find_library(log-lib log)
target_link_libraries(anyps3 PRIVATE ${log-lib})
