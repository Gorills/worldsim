#!/usr/bin/env sh
set -eu

# Launch the Godot 4.7 viewer scene. Requires Godot 4.7.x on PATH (or GODOT=...)
# and a built GDExtension at godot/project/bin/.
# https://docs.godotengine.org/en/4.7/tutorials/editor/command_line_tutorial.html

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
godot=${GODOT:-godot}
project="$root/godot/project"
scene=${SCENE:-res://main.tscn}

if ! command -v "$godot" >/dev/null 2>&1; then
    printf '%s\n' "Godot 4.7.x not found. Install it or set GODOT=/path/to/godot" >&2
    exit 1
fi

if [ ! -e "$project/bin/libworldsim_godot.so" ] &&
    [ ! -e "$project/bin/worldsim_godot.dll" ] &&
    [ ! -e "$project/bin/libworldsim_godot.dylib" ]; then
    printf '%s\n' "GDExtension library missing under godot/project/bin/. Run: make godot" >&2
    exit 1
fi

# The first editor import writes .godot/extension_list.cfg so WorldSimulationNode
# is registered. godot-cpp CI treats this process as best-effort because it can
# abort after generating the cache.
if [ ! -f "$project/.godot/extension_list.cfg" ]; then
    "$godot" --headless --path "$project" --import >/dev/null 2>&1 || true
fi

exec "$godot" --path "$project" --scene "$scene"
