#!/bin/bash
# Runs a @tool script inside a headless Godot EDITOR: the script is added to project.godot as an
# autoload for one run, then project.godot is restored. (override.cfg autoloads, the obvious
# alternative, are ignored by the editor.) Usage: tests/editor_probe.sh res://tests/x.gd [env...]
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
demo="$root/demo"
script="$1"
shift
# Scratch space: GAPH_TEST_TMP, or a folder in the system temp dir (works on any machine, CI too).
tmp="${GAPH_TEST_TMP:-${TMPDIR:-/tmp}/godot-audio-plugin-host-tests}"; mkdir -p "$tmp"
cp "$demo/project.godot" "$tmp/project.godot.probe.bak"
trap 'cp "$tmp/project.godot.probe.bak" "$demo/project.godot"' EXIT
printf '\n[autoload]\n\nEditorProbe="*%s"\n' "$script" >> "$demo/project.godot"
env "$@" timeout 120 godot --headless --editor --path "$demo" 2>&1
