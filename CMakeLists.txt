cmake_minimum_required(VERSION 3.24)
project(worldsim VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WORLDSIM_BUILD_TESTS "Build WorldSim tests" ON)
option(WORLDSIM_BUILD_CLI "Build WorldSim CLI" ON)
option(WORLDSIM_BUILD_GODOT "Build Godot 4.7 GDExtension adapter" OFF)
option(WORLDSIM_FETCH_GODOT_CPP "Fetch godot-cpp when building Godot adapter" ON)

add_library(worldsim
    src/spatial.cpp
    src/tectonics.cpp
    src/terrain.cpp
    src/fields.cpp
    src/state.cpp
    src/simulation.cpp
    src/modules.cpp
)
add_library(worldsim::worldsim ALIAS worldsim)
target_include_directories(worldsim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(worldsim PUBLIC cxx_std_20)
set_target_properties(worldsim PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(worldsim_c SHARED src/c_api.cpp)
target_link_libraries(worldsim_c PRIVATE worldsim)
target_include_directories(worldsim_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(worldsim_c PRIVATE WORLDSIM_C_EXPORTS)
set_target_properties(worldsim_c PROPERTIES OUTPUT_NAME worldsim_c)

function(worldsim_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
endfunction()

worldsim_enable_warnings(worldsim)
worldsim_enable_warnings(worldsim_c)

if(WORLDSIM_BUILD_CLI)
  add_executable(worldsim_cli apps/worldsim_cli.cpp)
  target_link_libraries(worldsim_cli PRIVATE worldsim)
  worldsim_enable_warnings(worldsim_cli)

  add_executable(worldsim_visual_dump apps/worldsim_visual_dump.cpp)
  target_link_libraries(worldsim_visual_dump PRIVATE worldsim)
  worldsim_enable_warnings(worldsim_visual_dump)
endif()

if(WORLDSIM_BUILD_TESTS)
  enable_testing()
  add_executable(worldsim_tests tests/worldsim_tests.cpp)
  target_link_libraries(worldsim_tests PRIVATE worldsim worldsim_c)
  worldsim_enable_warnings(worldsim_tests)
  add_test(NAME worldsim_tests COMMAND worldsim_tests)
  add_executable(worldsim_sanitizer_smoke tests/sanitizer_smoke.cpp)
  target_link_libraries(worldsim_sanitizer_smoke PRIVATE worldsim)
  worldsim_enable_warnings(worldsim_sanitizer_smoke)
  add_test(NAME worldsim_sanitizer_smoke COMMAND worldsim_sanitizer_smoke)
  if(TARGET worldsim_visual_dump)
    add_test(
      NAME worldsim_visual_dump_smoke
      COMMAND worldsim_visual_dump
        --seed 42
        --width 64
        --height 32
        --output ${CMAKE_BINARY_DIR}/visual-smoke
    )
  endif()
endif()

if(WORLDSIM_BUILD_GODOT)
  add_subdirectory(godot)
endif()
