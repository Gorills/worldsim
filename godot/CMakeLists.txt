include(FetchContent)

# godot-cpp 10.x requires an explicit API target. Keep 4.7 as the project default,
# while allowing packagers to override target/configuration from the command line.
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version")
set(GODOTCPP_TARGET "template_debug" CACHE STRING "godot-cpp target")
option(WORLDSIM_GODOT_HOT_RELOAD "Build godot-cpp with GDExtension hot-reload accounting" ON)
set(GODOTCPP_USE_HOT_RELOAD ${WORLDSIM_GODOT_HOT_RELOAD} CACHE BOOL "godot-cpp hot reload" FORCE)

# Keep godot-cpp proportional to the adapter, not to the entire Godot API.
# The adapter inherits only godot::Node; rendering classes are used from GDScript.
set(WORLDSIM_GODOT_BUILD_PROFILE
    "${CMAKE_CURRENT_SOURCE_DIR}/build_profile.json"
    CACHE FILEPATH "godot-cpp feature profile; set empty to build the full API")
if(WORLDSIM_GODOT_BUILD_PROFILE)
  set(GODOTCPP_BUILD_PROFILE
      "${WORLDSIM_GODOT_BUILD_PROFILE}"
      CACHE FILEPATH "godot-cpp feature build profile" FORCE)
endif()
set(GODOTCPP_GENERATE_TEMPLATE_GET_NODE OFF CACHE BOOL
    "WorldSim does not use the templated Node::get_node helper" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/godot-cpp/CMakeLists.txt")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../third_party/godot-cpp" "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp")
elseif(WORLDSIM_FETCH_GODOT_CPP)
  FetchContent_Declare(
    godot_cpp
    GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
    GIT_TAG 10.0.0-rc2
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(godot_cpp)
else()
  message(FATAL_ERROR "godot-cpp not found in third_party/godot-cpp and fetching is disabled")
endif()

# godot-cpp templates are not warning-clean under -Wconversion/-Wunused-parameter.
# Keep those flags on WorldSim adapter sources; treat godot-cpp as a system dependency.
# https://cmake.org/cmake/help/latest/prop_tgt/SYSTEM.html
# https://cmake.org/cmake/help/v3.24/prop_tgt/INTERFACE_SYSTEM_INCLUDE_DIRECTORIES.html
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
  set_property(TARGET godot-cpp PROPERTY SYSTEM TRUE)
else()
  get_target_property(_worldsim_godot_cpp_inc godot-cpp INTERFACE_INCLUDE_DIRECTORIES)
  if(_worldsim_godot_cpp_inc)
    set_property(TARGET godot-cpp APPEND PROPERTY
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_worldsim_godot_cpp_inc}")
  endif()
endif()

add_library(worldsim_godot SHARED
  src/register_types.cpp
  src/world_simulation_node.cpp
)
target_link_libraries(worldsim_godot PRIVATE worldsim godot-cpp)
target_include_directories(worldsim_godot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(worldsim_godot PRIVATE cxx_std_20)
worldsim_enable_warnings(worldsim_godot)
set_target_properties(worldsim_godot PROPERTIES
  OUTPUT_NAME worldsim_godot
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/project/bin"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/project/bin"
)

# Multi-config generators (Visual Studio/Xcode) otherwise append Debug/Release,
# which would make the .gdextension paths wrong.
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(worldsim_godot PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_CURRENT_SOURCE_DIR}/project/bin"
    RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_CURRENT_SOURCE_DIR}/project/bin"
  )
endforeach()
