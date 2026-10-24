#!/bin/bash
# Runs every test: core unit tests (ASan + TSan), the CLAP and VST3 hosts (ASan), then the headless
# Godot scenes. They use this repository's own plugins (examples/, tests/plugins/), built first;
# tests for third-party plugins (Dragonfly Reverb, OneTrick URCHIN) skip themselves when those are
# not installed. Stops at the first failing suite.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"

# No test may open a window on the user's real screen (GUI pop-ups that steal
# focus are not acceptable). Without DISPLAY no plugin window can open: open_editor() refuses with
# "no DISPLAY", which the checkbox probe relies on anyway. The one window test,
# demo/tests/gui_probe.gd, is never run from here: run it by hand, with the user's OK, or under a
# virtual display such as xvfb-run.
unset DISPLAY WAYLAND_DISPLAY

# Our plugins, for every suite below: examples/build/{gaph_test,clap_gain}.clap and
# {gaph_test_vst3,vst3_gain}.vst3 (demo/tests/test_util.gd's own_plugin() finds them there).
own="$root/examples/build"
"$(command -v scons || echo "$root/.venv/bin/scons")" -C "$root/examples" -Q -j"$(nproc)" test_plugin=yes > /dev/null

"$root/tests/core/run.sh"
echo "[clap host, no Godot]"
"$root/tests/clap/run.sh"
echo "[vst3 host, no Godot]"
"$root/tests/vst3/run.sh"

if [ ! -f "$root/demo/.godot/extension_list.cfg" ]; then
	# A headless first import of any project with a GDExtension crashes in Godot's doc
	# generation (godotengine/godot#111645, open since 4.5). The import itself completes first,
	# so the crash is ignored here and a second import confirms it.
	timeout 120 godot --headless --path "$root/demo" --import >/dev/null 2>&1 || true
	timeout 120 godot --headless --path "$root/demo" --import >/dev/null 2>&1
fi

run_scene() {
	echo "[godot $1]"
	timeout 300 godot --headless --path "$root/demo" "res://tests/$1.tscn" 2>&1 | grep -E "^(PASS|FAIL|SKIP|SMOKE|CLAP|VST3|BOTH|SIDECHAIN|STATE|LOCK|INFO|MENU|PATHS|PLATFORM)|WARNING|ERROR"
	return "${PIPESTATUS[0]}"
}
run_scene smoke
run_scene clap_test
run_scene vst3_test
run_scene both_test
run_scene sidechain_test
run_scene state_test
run_scene lock_test
# The per-plugin menu classes: once forced on (as the editor would have them), once in a plain
# game run, where they must not exist.
GAPH_MENU_CLASSES=1 CLAP_PATH="$own" GAPH_VST3_PATH="$own" run_scene menu_test
run_scene paths_test
echo "[plugin windows on Windows: Win32Window and the CLAP GUI path, under Wine, nothing on screen]"
"$root/tests/windows/run.sh" | grep -E "^(FAIL|SKIP|WIN32|CLAP GUI|VST3 GUI)"
echo "[platform test: our own plugins, both formats, as on every CI runner]"
"$root/tests/platform/run.sh" | grep -E "^(PASS|FAIL|SKIP|PLATFORM)"
echo "[export: macOS app layout, assembled on this machine]"
"$root/tests/export/macos.sh" | grep -E "^(PASS|FAIL|SKIP|MACOS)"
# Android: only with an Android SDK and a device or emulator connected (tests/android/run.sh).
if [ -n "$ANDROID_HOME" ] && adb get-state > /dev/null 2>&1; then
	(set -o pipefail; "$root/tests/android/run.sh" | grep -E "^(\[android|PASS|FAIL|SKIP|INFO|ANDROID)")
else
	echo "SKIP  Android: needs ANDROID_HOME and a device or emulator (adb)"
fi
echo "[export, end to end: debug]"
"$root/tests/export/run.sh"
echo "[export, end to end: release]"
"$root/.venv/bin/scons" -C "$root" platform=linux target=template_release -j"$(nproc)" -Q > /dev/null
EXPORT_MODE=release "$root/tests/export/run.sh"
echo "[real editor: Add Effect menu]"
menu=$("$root/tests/editor_probe.sh" res://tests/menu_probe.gd CLAP_PATH="$own" GAPH_VST3_PATH="$own" | grep MENUPROBE)
if [[ "$menu" == *"ClapSidechainSend"* && "$menu" != *"ClapPluginEffect"* && "$menu" != *"Vst3PluginEffect"* ]]; then
	echo "MENU OK: generic effects hidden, sends listed"
else
	echo "FAIL  editor menu: $menu"
	exit 1
fi
echo "[real editor: a click on the enable checkbox doesn't open the window]"
probe=$("$root/tests/editor_probe.sh" res://tests/toggle_probe.gd -u DISPLAY | grep -E "TOGGLEPROBE (A|B|C) (begin|end)|TOGGLEPROBE SKIP|cannot open the plugin window")
# "|| true": grep -c exits 1 when it counts zero, which is the expected answer for B, and
# set -e would otherwise end the whole run there.
a=$(echo "$probe" | sed -n '/A begin/,/A end/p' | grep -c "cannot open" || true)
b=$(echo "$probe" | sed -n '/B begin/,/B end/p' | grep -c "cannot open" || true)
# C selects the enabled effect (one attempt), then the switched-off one (none).
c=$(echo "$probe" | sed -n '/C begin/,/C end/p' | grep -c "cannot open" || true)
if [[ "$probe" == *"TOGGLEPROBE SKIP"* ]]; then
	echo "SKIP  toggle probe: $(echo "$probe" | grep -o 'TOGGLEPROBE SKIP.*' | head -1 | cut -d' ' -f3-)"
elif [ "$a" -eq 1 ] && [ "$b" -eq 0 ] && [ "$c" -eq 1 ]; then
	echo "TOGGLE OK: selecting opens, toggling doesn't, a switched-off effect doesn't"
else
	echo "FAIL  toggle probe: selecting tried to open $a time(s), toggling $b, enabled+disabled $c (want 1, 0, 1)"
	exit 1
fi
echo "[real editor: the class reference (F1) shows our docs]"
docs=$("$root/tests/editor_probe.sh" res://tests/docs_probe.gd | grep DOCSPROBE)
if [[ "$docs" == *"has   Runs a CLAP"* && "$docs" != *"LACKS"* ]]; then
	echo "DOCS OK: ClapPluginEffect's help page is complete"
else
	echo "FAIL  docs probe: $docs"
	exit 1
fi
# The XML must match what the extensions register: the doc tool, run over a copy, changes nothing.
tmp="${GAPH_TEST_TMP:-${TMPDIR:-/tmp}/godot-audio-plugin-host-tests}"
rm -rf "$tmp/doccheck" && mkdir -p "$tmp/doccheck" && cp -r "$root/doc_classes" "$tmp/doccheck/"
timeout 120 godot --headless --path "$root/demo" --doctool "$tmp/doccheck" --gdextension-docs > /dev/null 2>&1 || true
# Compared with the api_type="..." attribute stripped from both sides: Godot 4.7's doc tool writes
# it, 4.5's (CI's) does not, and either version reads both. Everything else must match exactly.
strip_api_type() { sed -E 's/ api_type="[a-z_]*"//' "$1"; }
docs_ok=1
for f in "$root"/doc_classes/*.xml; do
	if ! diff -q <(strip_api_type "$f") <(strip_api_type "$tmp/doccheck/doc_classes/$(basename "$f")") > /dev/null; then
		docs_ok=0
		diff <(strip_api_type "$f") <(strip_api_type "$tmp/doccheck/doc_classes/$(basename "$f")") | head -20
	fi
done
if [ $docs_ok -eq 1 ] && [ "$(ls "$tmp/doccheck/doc_classes" | wc -l)" -eq "$(ls "$root"/doc_classes | wc -l)" ]; then
	echo "DOCS OK: doc_classes/ matches the registered API"
else
	echo "FAIL  doc_classes/ is out of date: run godot --doctool <dir> --gdextension-docs and merge"
	exit 1
fi
echo "[scan guard]"
"$root/tests/scan_guard/run.sh" 2>&1 | grep -v "Aborted"
run_scene menu_test
