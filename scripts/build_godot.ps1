$ErrorActionPreference = "Stop"
cmake --preset godot-dev
cmake --build --preset godot-dev
ctest --preset godot-dev
Write-Host "GDExtension output: godot/project/bin/"
Write-Host "Open Godot 4.7.x with project: godot/project/project.godot"
