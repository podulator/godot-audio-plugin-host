#!/bin/bash
# End-to-end export check: a Linux export of the demo (debug, or release with EXPORT_MODE=release),
# with the CLAP example plugin (examples/clap-gain) in res://bin/clap and system plugin folders
# switched off, must load the plugin from bin/clap beside the executable. Needs Godot's export
# templates; skips without them.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
demo="$root/demo"
# Scratch space: GAPH_TEST_TMP, or a folder in the system temp dir (works on any machine, CI too).
tmp="${GAPH_TEST_TMP:-${TMPDIR:-/tmp}/godot-audio-plugin-host-tests}"; mkdir -p "$tmp"
out="$tmp/export_test"
"$(command -v scons || echo "$root/.venv/bin/scons")" -C "$root/examples" -Q > /dev/null
src="$root/examples/build/clap_gain.clap"
ver=$(godot --version | sed -E 's/^([0-9]+\.[0-9]+(\.[0-9]+)?)\.([a-z]+).*/\1.\3/')
if [ ! -f "$src" ] || [ ! -d "$HOME/.local/share/godot/export_templates/$ver" ]; then
	echo "SKIP  needs $src and export templates for $ver"
	echo "EXPORT OK (1 skipped)"
	exit 0
fi
rm -rf "$out"
mkdir -p "$out" "$demo/bin/clap"
cp "$src" "$demo/bin/clap/"
cp "$demo/project.godot" "$out/project.godot.bak"
cleanup() {
	rm -rf "$demo/bin"
	cp "$out/project.godot.bak" "$demo/project.godot"
	rm -f "$demo/export_presets.cfg" "$demo/tests/export_unshipped.tres"
}
trap cleanup EXIT

# A plugin that is NOT shipped: in a "system" folder only (CLAP_PATH, for the export and the game),
# used by an effect on a bus named Reverb in an exported bus layout. The export must warn about it
# by name and bus, and the game must not find it: exported games ignore the system folders unless
# use_system_paths_in_exports is on (off by default; use_system_paths stays on, as for auditioning).
mkdir -p "$out/system_clap"
cp "$root/examples/build/clap_gain.clap" "$out/system_clap/unshipped_gain.clap"
export CLAP_PATH="$out/system_clap"
cat > "$demo/tests/export_unshipped.tres" <<'TRES'
[gd_resource type="AudioBusLayout" load_steps=2 format=3]

[sub_resource type="ClapPluginEffect" id="ClapPluginEffect_x1"]
resource_name = "Clap_UnshippedGain"
plugin_path = "unshipped_gain.clap"

[resource]
bus/1/name = &"Reverb"
bus/1/effect/0/effect = SubResource("ClapPluginEffect_x1")
TRES

# The exported game runs the probe scene.
cat >> "$demo/project.godot" <<'CFG'

[application]
run/main_scene="res://tests/export_probe.tscn"
CFG
cat > "$demo/export_presets.cfg" <<'CFG'
[preset.0]
name="Linux"
platform="Linux"
runnable=true
export_filter="all_resources"
export_path=""
[preset.0.options]
binary_format/architecture="x86_64"
binary_format/embed_pck=false
CFG
timeout 300 godot --headless --path "$demo" --import >/dev/null 2>&1 || true
# EXPORT_MODE=release: a release export, which loads the template_release libraries (build them
# first). The default is debug.
mode="${EXPORT_MODE:-debug}"
timeout 300 godot --headless --path "$demo" --export-$mode "Linux" "$out/game.x86_64" > "$out/export.log" 2>&1 || true

fails=0
check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
check "the export produced an executable" '[ -x "$out/game.x86_64" ]'
check "the export step copied the plugin to bin/clap beside it" '[ -f "$out/bin/clap/clap_gain.clap" ]'
check "the .pck does not carry the plugin binary" '! grep -qa "org.godot-audio-plugin-host.example.gain" "$out/game.pck" 2>/dev/null || [ $(stat -c %s "$out/game.pck") -lt $(stat -c %s "$src") ]'
rm -rf "$demo/bin" # gone from the project: the game must use its own copy
set +e
result=$(cd "$out" && timeout 60 ./game.x86_64 --headless 2>&1 | grep EXPORTPROBE)
set -e
echo "      $result"
check "the exported $mode game loads the plugin from bin/clap" '[[ "$result" == *"loaded=true exported=true"* ]]'
check "the export warns about the plugin it cannot ship, naming the bus" 'grep -q "Reverb bus.*uses unshipped_gain.clap" "$out/export.log"'
check "the exported game ignores the system folders: the unshipped plugin is not found there" '[[ "$result" == *"system=false"* ]]'
echo "EXPORT $([ $fails -eq 0 ] && echo OK || echo "FAILED ($fails)")"
[ $fails -eq 0 ]
