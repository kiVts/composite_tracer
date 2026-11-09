cmake_minimum_required(VERSION 3.24)
project(link_tracer LANGUAGES C CXX)

# Needs the SWML SDK folder: cmake -B build -A x64 -DSWML_SDK_DIR=path/to/sdk
set(SWML_SDK_DIR "" CACHE PATH "SWML SDK folder (the one with SwmlMod.cmake)")
if(NOT EXISTS "${SWML_SDK_DIR}/SwmlMod.cmake")
  message(FATAL_ERROR "Set SWML_SDK_DIR to the SWML SDK folder")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_subdirectory("${SWML_SDK_DIR}" swml_sdk)

swml_add_mod(link_tracer SOURCES
  src/setup.cpp
  src/hook_mc_select.cpp
  src/xml_lite.cpp
  src/tracer.cpp
  src/definition_files.cpp
)
target_include_directories(link_tracer PRIVATE src)
