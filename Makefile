# Developer entry points. The simulation kernel is still built with CMake presets;
# this Makefile only wraps those presets and the Godot 4.7 scene launch.
#
#   make run     # build GDExtension if needed, then launch godot/project/main.tscn
#   make map     # build GDExtension if needed, then launch godot/project/world_map.tscn
#   make lab     # build GDExtension, then launch the full-world simulation lab
#   make visual  # build core and write the five-seed C++ visual diagnostic suite
#   make climate # build core and run the two-year climate diagnostic
#   make longrun # build core and run the 100-year seed-42 stability diagnostic
#   make stability # run the multi-seed/resolution stability matrix

.PHONY: help core godot run run-scene map lab visual geology climate longrun stability test

GODOT ?= godot
export GODOT

STABILITY_YEARS ?= 100
STABILITY_SEEDS ?= 0,42,999
STABILITY_LEVELS ?= 1,2
STABILITY_MODES ?= coupled

help:
	@printf '%s\n' \
		'make core       build simulation kernel (cmake preset dev)' \
		'make godot      build Godot 4.7 GDExtension' \
		'make run        build GDExtension, then launch main.tscn' \
		'make run-scene  same as make run' \
		'make map        build GDExtension, then launch world_map.tscn' \
		'make lab        build GDExtension, then launch simulation_lab.tscn' \
		'make visual     build core and generate C++ visual diagnostic suite' \
		'make geology    build core and run geology plausibility benchmark' \
		'make climate    build core and run two-year climate diagnostics' \
		'make longrun    build core and run a 100-year level-2 seed-42 diagnostic' \
		'make stability  build core and run the multi-seed/resolution stability matrix' \
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

lab: godot
	SCENE=res://simulation_lab.tscn ./scripts/run_godot.sh

visual: core
	./out/dev/worldsim_visual_dump --suite --width 512 --height 256 --output out/visual-dump

geology: core
	./out/dev/worldsim_geology_benchmark --seed-count 64 --samples 4096 --cover-level 5 --output out/geology-benchmark

climate: core
	./out/dev/worldsim_climate_dump --days 730 --level 2 --adaptive --output out/climate-diagnostics

longrun: core
	./out/dev/worldsim_long_run --years 100 --level 2 --seed 42 --output out/long-run/seed42-coupled.csv

stability: core
	python3 scripts/run_stability_matrix.py \
		--binary ./out/dev/worldsim_long_run \
		--years $(STABILITY_YEARS) \
		--seeds $(STABILITY_SEEDS) \
		--levels $(STABILITY_LEVELS) \
		--modes $(STABILITY_MODES) \
		--assert-stable \
		--output out/stability-matrix

test:
	cmake --preset dev
	cmake --build --preset dev
	ctest --preset dev
