#!/bin/bash
# macOS export check, run from Linux: Godot can assemble a macOS app on any OS, and this checks
# what our export step puts in it (export_copy.h). Running the app needs a Mac; this does not.
#   * every plugin BUNDLE in res://bin/<format> lands in <app>/Contents/PlugIns/, flat, including
#     one from a subfolder;
#   * a single-file .clap (a Linux/Windows build, never a macOS plugin) is left out, with a message.
# Works on a scratch copy of the demo, so the real project is never touched: a universal macOS
# export needs a texture setting the demo doesn't use, and our own macOS libraries (built only on
# macOS) are stood in for by empty folders, with code signing off. Skips without the template.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="${GAPH_TEST_TMP:-${TMPDIR:-/tmp}/godot-audio-plugin-host-tests}"; mkdir -p "$tmp"
out="$tmp/macos_export"
ver=$(godot --version | sed -E 's/^([0-9]+\.[0-9]+(\.[0-9]+)?)\.([a-z]+).*/\1.\3/')
if [ ! -f "$HOME/.local/share/godot/export_templates/$ver/macos.zip" ]; then
	echo "SKIP  needs the macOS export template for $ver"
	echo "MACOS EXPORT OK (1 skipped)"
	exit 0
fi
"$(command -v scons || echo "$root/.venv/bin/scons")" -C "$root/examples" -Q test_plugin=yes > /dev/null

rm -rf "$out"
mkdir -p "$out"
cp -R "$root/demo" "$out/demo"
p="$out/demo"
rm -rf "$p/.godot" "$p/bin" "$p/export_presets.cfg"
mkdir -p "$p/bin/clap/vendor/Sub.clap/Contents/MacOS" "$p/bin/vst3"
cp -R "$root/examples/build/vst3_gain.vst3" "$p/bin/vst3/"
cp "$root/examples/build/gaph_test.clap" "$p/bin/clap/" # a single file: must be left out
printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleExecutable</key><string>Sub</string></dict></plist>\n' \
	> "$p/bin/clap/vendor/Sub.clap/Contents/Info.plist"
echo stand-in > "$p/bin/clap/vendor/Sub.clap/Contents/MacOS/Sub"
for x in clap vst3; do
	for t in debug release; do
		mkdir -p "$p/addons/${x}_host/bin/lib${x}_host.macos.template_$t.framework"
	done
done
printf '\n[rendering]\n\ntextures/vram_compression/import_etc2_astc=true\n' >> "$p/project.godot"
cat > "$p/export_presets.cfg" <<'CFG'
[preset.0]
name="macOS"
platform="macOS"
runnable=true
export_filter="all_resources"
include_filter=""
exclude_filter=""
export_path=""
[preset.0.options]
application/bundle_identifier="org.godot-audio-plugin-host.test"
binary_format/architecture="universal"
codesign/codesign=0
notarization/notarization=0
CFG
timeout 300 godot --headless --path "$p" --import > /dev/null 2>&1 || true
timeout 300 godot --headless --path "$p" --import > /dev/null 2>&1 || true
timeout 300 godot --headless --path "$p" --export-debug "macOS" "$out/game.zip" > "$out/export.log" 2>&1 || true
rm -rf "$out/unzipped"
mkdir -p "$out/unzipped"
(cd "$out/unzipped" && unzip -q ../game.zip) || true
plugins="$(find "$out/unzipped" -maxdepth 3 -type d -name PlugIns | head -1)"

fails=0
check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
check "the export produced an app with Contents/PlugIns" '[ -n "$plugins" ]'
check "the VST3 bundle is in Contents/PlugIns, whole" '[ -n "$(find "$plugins/vst3_gain.vst3/Contents" -type f -name "vst3_gain*" 2>/dev/null)" ]'
check "a CLAP bundle from a subfolder is in Contents/PlugIns, flat" '[ -f "$plugins/Sub.clap/Contents/MacOS/Sub" ]'
check "the single-file .clap is left out" '[ ! -e "$plugins/gaph_test.clap" ]'
check "... and the export says so" 'grep -q "left out of the macOS export.*gaph_test.clap" "$out/export.log"'
check "nothing went beside the app (bin/ is for Linux and Windows)" '[ ! -d "$out/bin" ]'
echo "MACOS EXPORT $([ $fails -eq 0 ] && echo OK || echo "FAILED ($fails)")"
[ $fails -eq 0 ]
