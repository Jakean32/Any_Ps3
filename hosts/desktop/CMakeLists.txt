cmake_minimum_required(VERSION 3.16)

# Any_Ps3 desktop host: SDL2 + Dear ImGui. Builds on Windows (MSVC) and Linux.
# Dependencies are pulled via FetchContent so no vcpkg/manual installs are needed.
project(anyps3_desktop LANGUAGES C CXX)

# Version from the single VERSION file at the repo root (inherited from the
# root CMakeLists when built as a subdirectory; read here for standalone use).
if(NOT DEFINED ANYPS3_VERSION_TEXT)
  get_filename_component(ANYPS3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  file(READ "${ANYPS3_ROOT}/VERSION" ANYPS3_VERSION_TEXT)
  string(STRIP "${ANYPS3_VERSION_TEXT}" ANYPS3_VERSION_TEXT)
  add_compile_definitions(ANYPS3_VERSION="${ANYPS3_VERSION_TEXT}")
endif()
message(STATUS "Any_Ps3 version (from VERSION file): ${ANYPS3_VERSION_TEXT}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  sdl2
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-2.30.10
  GIT_SHALLOW TRUE
)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.6
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(sdl2)

FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  FetchContent_Populate(imgui)
endif()

add_executable(Any_Ps3
  main.cpp
  ui.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(Any_Ps3 PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(Any_Ps3 PRIVATE anyps3_core SDL2::SDL2)
target_compile_definitions(Any_Ps3 PRIVATE ANYPS3_IMGUI_DEMO=0)

if(MSVC)
  target_compile_options(Any_Ps3 PRIVATE /W4)
else()
  target_compile_options(Any_Ps3 PRIVATE -Wall -Wextra)
endif()

# Windows: place the exe next to a portable config dir; installer-less by design.
if(WIN32)
  set_target_properties(Any_Ps3 PROPERTIES OUTPUT_NAME "Any_Ps3")
endif()
