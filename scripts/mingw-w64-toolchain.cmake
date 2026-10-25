# CMake toolchain file for cross-compiling the Any_Ps3 Windows host with
# MinGW-w64 (x86_64). Set MINGW_PREFIX to the directory CONTAINING
# bin/x86_64-w64-mingw32-g++ (for the local unpack described in
# scripts/build_windows_mingw.sh that is <repo>/tools/mingw64/usr).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED MINGW_PREFIX)
  set(MINGW_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../tools/mingw64/usr")
endif()

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MINGW_PREFIX)

find_program(MINGW_CXX x86_64-w64-mingw32-g++
  HINTS "${MINGW_PREFIX}/bin" /usr/bin /usr/local/bin
  REQUIRED)
find_program(MINGW_CC x86_64-w64-mingw32-gcc
  HINTS "${MINGW_PREFIX}/bin" /usr/bin /usr/local/bin
  REQUIRED)

set(CMAKE_CXX_COMPILER ${MINGW_CXX})
set(CMAKE_C_COMPILER ${MINGW_CC})
find_program(MINGW_RC x86_64-w64-mingw32-windres
  HINTS "${MINGW_PREFIX}/bin" /usr/bin /usr/local/bin)
if(MINGW_RC)
  set(CMAKE_RC_COMPILER ${MINGW_RC})
endif()

set(CMAKE_FIND_ROOT_PATH ${MINGW_PREFIX}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
