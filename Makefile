# Developer entry points. The simulation kernel is still built with CMake presets;
# this Makefile only wraps those presets and the Godot 4.7 scene launch.
#
#   make run    # build GDExtension if needed, then launch godot/project/main.tscn
#   make map    # build GDExtension if needed, then launch godot/project/world_map.tscn

.PHONY: help core godot run run-scene map test

GODOT ?= godot
export GODOT

help:
	@printf '%s\n' \
		'make core       build simulation kernel (cmake preset dev)' \
		'make godot      build Godot 4.7 GDExtension' \
		'make run        build GDExtension, then launch main.tscn' \
		'make run-scene  same as make run' \
		'make map        build GDExtension, then launch world_map.tscn' \
		'make test       build kernel and run ctest --preset dev' \
		'' \
		'Override the editor with GODOT=/path/to/godot'

core:
	cmake --preset dev
	cmake --build --preset dev

godot:
	cmake --preset godot-dev
	cmake --build --preset godot-dev --target worldsim_godot

run run-scene: godot
	./scripts/run_godot.sh

map: godot
	SCENE=res://world_map.tscn ./scripts/run_godot.sh

test:
	cmake --preset dev
	cmake --build --preset dev
	ctest --preset dev
