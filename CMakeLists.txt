# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.13)
project(ballistic_engine VERSION 0.4.0 LANGUAGES C)

option(BE_SHARED "Build the shared library (for Python, Unity, Godot...)" ON)
option(BE_TESTS "Build the tests" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(BE_SOURCES src/engine.c src/params.c)

# The engine computes in the same order as the JavaScript one. Fused multiply-adds would change the rounding, so
# they are off; so is anything like -ffast-math.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(BE_FLAGS -ffp-contract=off -Wall -Wextra -Wno-unused-parameter)
elseif(MSVC)
  set(BE_FLAGS /fp:precise /W3 /D_CRT_SECURE_NO_WARNINGS)
endif()

add_library(ballistic_static STATIC ${BE_SOURCES})
target_include_directories(ballistic_static PUBLIC include PRIVATE src)
target_compile_options(ballistic_static PRIVATE ${BE_FLAGS})
set_target_properties(ballistic_static PROPERTIES OUTPUT_NAME ballistic_static C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_link_libraries(ballistic_static PUBLIC m)
endif()

if(BE_SHARED)
  add_library(ballistic SHARED ${BE_SOURCES})
  target_include_directories(ballistic PUBLIC include PRIVATE src)
  target_compile_options(ballistic PRIVATE ${BE_FLAGS})
  target_compile_definitions(ballistic PRIVATE BE_BUILD_SHARED)
  set_target_properties(ballistic PROPERTIES C_STANDARD 99 C_VISIBILITY_PRESET hidden VERSION ${PROJECT_VERSION} SOVERSION 0)
  if(NOT MSVC)
    target_link_libraries(ballistic PRIVATE m)
  endif()
endif()

add_executable(ballistic-render cli/ballistic-render.c)
target_link_libraries(ballistic-render PRIVATE ballistic_static)
target_compile_options(ballistic-render PRIVATE ${BE_FLAGS})
set_target_properties(ballistic-render PROPERTIES C_STANDARD 99)
if(NOT MSVC)
  target_compile_definitions(ballistic-render PRIVATE _POSIX_C_SOURCE=200809L)
endif()

if(BE_TESTS)
  enable_testing()
  add_executable(test_engine tests/test_engine.c)
  target_link_libraries(test_engine PRIVATE ballistic_static)
  target_include_directories(test_engine PRIVATE src)
  target_compile_options(test_engine PRIVATE ${BE_FLAGS})
  set_target_properties(test_engine PROPERTIES C_STANDARD 99)
  add_test(NAME engine COMMAND test_engine WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

include(GNUInstallDirs)
install(TARGETS ballistic_static ballistic-render)
if(BE_SHARED)
  install(TARGETS ballistic)
endif()
install(FILES include/ballistic.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
