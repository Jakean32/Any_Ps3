cmake_minimum_required(VERSION 3.16)

# Single source of truth for the version string: the VERSION file at the repo
# root. Read BEFORE project() so project(VERSION) matches, then injected as the
# ANYPS3_VERSION macro so the code (both UIs, cache manifests) carries it.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ANYPS3_VERSION_TEXT)
string(STRIP "${ANYPS3_VERSION_TEXT}" ANYPS3_VERSION_TEXT)
if(ANYPS3_VERSION_TEXT STREQUAL "")
  message(FATAL_ERROR "Any_Ps3: VERSION file is empty or missing")
endif()
project(Any_Ps3 VERSION ${ANYPS3_VERSION_TEXT} LANGUAGES CXX)
message(STATUS "Any_Ps3 version (from VERSION file): ${ANYPS3_VERSION_TEXT}")
add_compile_definitions(ANYPS3_VERSION="${ANYPS3_VERSION_TEXT}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Options
option(ANYPS3_BUILD_TESTS "Build unit tests" ON)
option(ANYPS3_LLVM "Enable LLVM AOT backend (requires LLVM dev packages)" OFF)
option(ANYPS3_VULKAN "Enable Vulkan RSX backend (requires Vulkan SDK headers)" OFF)
option(ANYPS3_BUILD_HOST_DESKTOP "Build the desktop host (SDL2 + Dear ImGui; FetchContent)" OFF)

if(ANYPS3_LLVM)
  find_package(LLVM REQUIRED CONFIG)
  add_compile_definitions(ANYPS3_WITH_LLVM)
  message(STATUS "Any_Ps3: LLVM backend enabled (${LLVM_VERSION_STRING})")
endif()
if(ANYPS3_VULKAN)
  find_package(Vulkan REQUIRED)
  add_compile_definitions(ANYPS3_WITH_VULKAN)
  message(STATUS "Any_Ps3: Vulkan backend enabled")
endif()

# Core static library — zero external dependencies.
file(GLOB_RECURSE ANYPS3_CORE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.cpp)
add_library(anyps3_core STATIC ${ANYPS3_CORE_SOURCES})
target_include_directories(anyps3_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include)

if(ANYPS3_LLVM)
  target_include_directories(anyps3_core SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
  target_compile_options(anyps3_core PUBLIC -std=c++17)
  # LLVM-C is a stable C API
  target_link_directories(anyps3_core PUBLIC ${LLVM_LIBRARY_DIRS})
  target_link_libraries(anyps3_core PUBLIC LLVM)
  llvm_map_components_to_libnames(ANYPS3_LLVM_LIBS core orcjit native)
  target_link_libraries(anyps3_core PUBLIC ${ANYPS3_LLVM_LIBS})
endif()

if(ANYPS3_VULKAN)
  target_link_libraries(anyps3_core PUBLIC Vulkan::Vulkan)
endif()

if(ANYPS3_BUILD_HOST_DESKTOP)
  add_subdirectory(hosts/desktop)
endif()

if(ANYPS3_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
