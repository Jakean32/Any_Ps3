cmake_minimum_required(VERSION 3.16)
project(Any_Ps3 VERSION 0.1.0 LANGUAGES CXX)

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
