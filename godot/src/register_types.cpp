#include "world_simulation_node.hpp"
#include "survival_simulation_node.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static void initialize_worldsim(ModuleInitializationLevel level) {
    if (level!=MODULE_INITIALIZATION_LEVEL_SCENE) return;
    GDREGISTER_CLASS(worldsim::godot_adapter::WorldSimulationNode);
    GDREGISTER_CLASS(worldsim::godot_adapter::SurvivalSimulationNode);
}

static void uninitialize_worldsim(ModuleInitializationLevel level) {
    if (level!=MODULE_INITIALIZATION_LEVEL_SCENE) return;
}

extern "C" GDExtensionBool GDE_EXPORT worldsim_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init_obj(get_proc_address,library,initialization);
    init_obj.register_initializer(initialize_worldsim);
    init_obj.register_terminator(uninitialize_worldsim);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
