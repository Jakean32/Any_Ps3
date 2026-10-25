# Any_Ps3 Windows host (Dear ImGui + Win32 + D3D11) — built for x86_64 with
# MinGW-w64 (see scripts/mingw-w64-toolchain.cmake) or MSVC.
#
# The exe is fully statically linked against the C++ runtime (-static) so it
# runs on a clean Windows install without any redistributables.
cmake_minimum_required(VERSION 3.16)
project(anyps3_windows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root (hosts/windows/../..)
get_filename_component(ANYPS3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Version from the single VERSION file at the repo root (same value the Gradle
# build uses for versionName and the root CMakeLists uses for the core).
file(READ "${ANYPS3_ROOT}/VERSION" ANYPS3_VERSION_TEXT)
string(STRIP "${ANYPS3_VERSION_TEXT}" ANYPS3_VERSION_TEXT)
if(ANYPS3_VERSION_TEXT STREQUAL "")
  message(FATAL_ERROR "Any_Ps3: VERSION file is empty or missing")
endif()
message(STATUS "Any_Ps3 version (from VERSION file): ${ANYPS3_VERSION_TEXT}")
add_compile_definitions(ANYPS3_VERSION="${ANYPS3_VERSION_TEXT}")

option(ANYPS3_STATIC_CRT "Statically link the C++ runtime (MinGW: -static)" ON)

# --- Dear ImGui (vendored under third_party/imgui) ---------------------------
set(IMGUI_DIR "${ANYPS3_ROOT}/third_party/imgui")
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

# --- stb (vendored single header, public domain) ------------------------------
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${ANYPS3_ROOT}/third_party/stb)

# --- Any_Ps3 core --------------------------------------------------------------
file(GLOB_RECURSE ANYPS3_CORE_SOURCES CONFIGURE_DEPENDS
     ${ANYPS3_ROOT}/core/src/*.cpp)
add_library(anyps3_core STATIC ${ANYPS3_CORE_SOURCES})
target_include_directories(anyps3_core PUBLIC ${ANYPS3_ROOT}/core/include)

# --- The app -------------------------------------------------------------------
# App icon resource (generated from the user's logo; multi-frame .ico).
if(MINGW)
  configure_file(resources/anyps3.rc.in anyps3.rc COPYONLY)
  # windres needs the .ico next to the generated .rc
  configure_file(resources/app.ico app.ico COPYONLY)
  add_executable(Any_Ps3
    main.cpp
    shell.cpp
    diagnostics.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/anyps3.rc)
else()
  add_executable(Any_Ps3
    main.cpp
    shell.cpp
    diagnostics.cpp
    resources/anyps3.rc)
endif()
target_link_libraries(Any_Ps3 PRIVATE anyps3_core imgui stb)
target_include_directories(Any_Ps3 PRIVATE ${ANYPS3_ROOT}/core/include)

if(MINGW)
  target_compile_options(Any_Ps3 PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(ANYPS3_STATIC_CRT)
    # one self-contained exe; only Windows system DLLs are used at runtime
    target_link_options(Any_Ps3 PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
  target_link_libraries(Any_Ps3 PRIVATE
    d3d11 d3dcompiler dxgi ole32 uuid user32 gdi32 winmm imm32 dwmapi
    comctl32 shell32 setupapi version)
  # GUI entry point (WinMain)
  target_link_options(Any_Ps3 PRIVATE -mwindows)
endif()
