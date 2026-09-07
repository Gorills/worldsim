#!/usr/bin/env sh
set -eu
cmake --preset godot-dev
cmake --build --preset godot-dev
ctest --preset godot-dev
printf '%s\n' "GDExtension output: godot/project/bin/"
printf '%s\n' "Open Godot 4.7.x with project: godot/project/project.godot"
