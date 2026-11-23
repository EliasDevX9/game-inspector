cmake_minimum_required(VERSION 3.22)
project(game_inspector LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MOD_LIBRARY_NAME "game_inspector" CACHE STRING "Native library name without lib prefix or .so suffix")

# Preloader SDK (headers + the `preloader` target), pinned to a known commit.
include(FetchContent)

FetchContent_Declare(
  preloader_android
  GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
  GIT_TAG cc08b40a31471d3e2dedb267f5f17b28da2b0604)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_Declare(boost_pfr GIT_REPOSITORY https://github.com/boostorg/pfr.git GIT_TAG boost-1.88.0)
FetchContent_Declare(magic_enum GIT_REPOSITORY https://github.com/Neargye/magic_enum.git GIT_TAG v0.9.7)
FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG 11.2.0)

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)

# Same dependency set as the SDK so it reuses them instead of fetching copies.
FetchContent_MakeAvailable(nlohmann_json boost_pfr magic_enum fmt)
FetchContent_MakeAvailable(preloader_android)
FetchContent_GetProperties(preloader_android)
set(LEVI_PRELOADER_INCLUDE_DIR "${preloader_android_SOURCE_DIR}/include")

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Game Inspector only supports arm64-v8a, got: ${CMAKE_ANDROID_ARCH_ABI}")
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/${CMAKE_ANDROID_ARCH_ABI}")

find_library(LL_LOG_LIB log REQUIRED)

add_library(${MOD_LIBRARY_NAME} SHARED
  src/main.cpp
  src/mod/InspectorMod.cpp
  src/mod/MemScanner.cpp
  src/mod/FuncMonitor.cpp
  src/mod/Symbols.cpp)

set_target_properties(${MOD_LIBRARY_NAME} PROPERTIES OUTPUT_NAME "${MOD_LIBRARY_NAME}")

target_include_directories(${MOD_LIBRARY_NAME} PRIVATE src "${LEVI_PRELOADER_INCLUDE_DIR}")

target_compile_options(${MOD_LIBRARY_NAME} PRIVATE
  $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall -Wextra>
  $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fvisibility=hidden -fvisibility-inlines-hidden>)

# Android 15+ devices use 16 KB pages.
target_link_options(${MOD_LIBRARY_NAME} PRIVATE "-Wl,-z,max-page-size=16384")

target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE
  ${LL_LOG_LIB}
  nlohmann_json::nlohmann_json
  Boost::pfr
  magic_enum::magic_enum
  fmt::fmt)

# The launcher already loads the preloader runtime; link against it so the
# pl:: symbols resolve, or leave them unresolved for the loader to bind.
if(TARGET preloader)
  target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE preloader)
else()
  target_link_options(${MOD_LIBRARY_NAME} PRIVATE "-Wl,--unresolved-symbols=ignore-all")
endif()

add_custom_command(TARGET ${MOD_LIBRARY_NAME} POST_BUILD
  COMMAND "${CMAKE_STRIP}" --strip-unneeded "$<TARGET_FILE:${MOD_LIBRARY_NAME}>"
  VERBATIM)
