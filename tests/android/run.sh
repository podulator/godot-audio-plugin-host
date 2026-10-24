#!/bin/bash
# The Android test: demo/tests/platform_test.tscn in a real Android export, on a device or an
# emulator, with this repository's two CLAP test plugins shipped as Android libraries.
#
# Needs:
#   ANDROID_HOME   an Android SDK with build-tools and platform-tools (adb), and the NDK
#   NDK_VERSION    the NDK in $ANDROID_HOME/ndk to use (default 30.0.16248370)
#   JAVA_HOME      a JDK, for signing the APK (default /usr/lib/jvm/default)
#   ANDROID_SERIAL the device to use, if adb sees more than one
#   godot on PATH, with its Android export templates installed, and SCons in .venv/bin or on PATH.
# The device or emulator must be running. Nothing in your own Godot editor settings is used or
# changed: the export runs with its own configuration folder in the scratch directory.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="${GAPH_TEST_TMP:-${TMPDIR:-/tmp}/godot-audio-plugin-host-tests}/android"
ndk_version="${NDK_VERSION:-30.0.16248370}"
java_home="${JAVA_HOME:-/usr/lib/jvm/default}"
: "${ANDROID_HOME:?set ANDROID_HOME to an Android SDK}"
scons="$(command -v scons || echo "$root/.venv/bin/scons")"
toolchain="$ANDROID_HOME/ndk/$ndk_version/toolchains/llvm/prebuilt/linux-x86_64/bin"
package="org.godotaudiopluginhost.test"
adb=(adb)
[ -n "$ANDROID_SERIAL" ] && adb=(adb -s "$ANDROID_SERIAL")

for arch in arm64 x86_64; do
	"$scons" -C "$root" -Q platform=android arch="$arch" target=template_debug ndk_version="$ndk_version" > /dev/null
done

# The test plugins for both ABIs.
rm -rf "$tmp"
mkdir -p "$tmp"
flags=(-std=c11 -O2 -fPIC -Wall -Wextra -Werror -I"$root/thirdparty/clap/include")
for abi in arm64-v8a:aarch64 x86_64:x86_64; do
	dir="$tmp/plugins/${abi%%:*}"
	cc="$toolchain/${abi##*:}-linux-android24-clang"
	mkdir -p "$dir"
	"$cc" "${flags[@]}" -shared -Wl,-z,max-page-size=16384 "$root/tests/plugins/gaph_test_plugin.c" -o "$dir/libgaph_test.so"
	"$cc" "${flags[@]}" -shared -Wl,-z,max-page-size=16384 "$root/examples/clap-gain/clap_gain.c" -o "$dir/libclap_gain.so"
done

# A copy of the demo project, set up to run the platform test on Android with the Dummy audio
# driver (an emulator has no reliable audio) and the Compatibility renderer (an emulator's
# software graphics cannot show Vulkan).
project="$tmp/project"
mkdir -p "$project"
(cd "$root/demo" && tar cf - --exclude=./.godot --exclude=./bin --exclude=./export_presets.cfg .) | (cd "$project" && tar xf -)
for abi in arm64-v8a x86_64; do
	mkdir -p "$project/bin/clap/android/$abi"
	cp "$tmp/plugins/$abi/"*.so "$project/bin/clap/android/$abi/"
done
python3 - "$project/project.godot" << 'EOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r'\nrun/main_scene="[^"]*"', '', s)
s = s.replace('[application]\n', '[application]\n\nrun/main_scene="res://tests/platform_test.tscn"\n', 1)
s += '\n[audio]\n\ndriver/driver.android="Dummy"\n'
s += '\n[rendering]\n\nrenderer/rendering_method="gl_compatibility"\nrenderer/rendering_method.mobile="gl_compatibility"\ntextures/vram_compression/import_etc2_astc=true\n'
open(p, 'w').write(s)
EOF
cat > "$project/export_presets.cfg" << EOF
[preset.0]
name="Android"
platform="Android"
runnable=true
export_filter="all_resources"
include_filter=""
exclude_filter=""
export_path=""
[preset.0.options]
gradle_build/use_gradle_build=false
architectures/armeabi-v7a=false
architectures/arm64-v8a=true
architectures/x86=false
architectures/x86_64=true
package/unique_name="$package"
package/name="GAPH test"
EOF
export XDG_CONFIG_HOME="$tmp/config" XDG_DATA_HOME="$tmp/data"
mkdir -p "$XDG_CONFIG_HOME/godot" "$XDG_DATA_HOME/godot"
ln -s "${GODOT_TEMPLATES:-$HOME/.local/share/godot/export_templates}" "$XDG_DATA_HOME/godot/export_templates"
version="$(godot --version | cut -d. -f1-2)"
printf '[gd_resource type="EditorSettings" format=3]\n\n[resource]\nexport/android/android_sdk_path = "%s"\nexport/android/java_sdk_path = "%s"\n' \
	"$ANDROID_HOME" "$java_home" > "$XDG_CONFIG_HOME/godot/editor_settings-$version.tres"
# A headless first import of a project with GDExtensions can abort (godotengine/godot#111645).
(timeout 300 godot --headless --path "$project" --import > /dev/null 2>&1) 2> /dev/null || true
(timeout 300 godot --headless --path "$project" --import > /dev/null 2>&1) 2> /dev/null || true

failures=0
echo "[android: platform test, CLAP, on $("${adb[@]}" shell getprop ro.product.model | tr -d '\r') $("${adb[@]}" shell getprop ro.product.cpu.abi | tr -d '\r')]"
if ! timeout 600 godot --headless --path "$project" --export-debug Android "$tmp/test.apk" > "$tmp/export.log" 2>&1 || [ ! -f "$tmp/test.apk" ]; then
	echo "FAIL  the export failed:"
	tail -20 "$tmp/export.log"
	exit 1
fi
# Uninstalled first: an installed copy signed with another debug key would refuse the update.
"${adb[@]}" uninstall "$package" > /dev/null 2>&1 || true
"${adb[@]}" install -g "$tmp/test.apk" > /dev/null
"${adb[@]}" logcat -c
"${adb[@]}" shell am start -n "$package/com.godot.game.GodotAppLauncher" > /dev/null
log="$tmp/logcat.txt"
for _ in $(seq 1 60); do
	sleep 3
	"${adb[@]}" logcat -d -s 'godot:*' | sed -E 's/^.*godot\s*: //' > "$log"
	grep -qE "^PLATFORM (OK|FAILED)" "$log" && break
	"${adb[@]}" logcat -d | grep -q "Fatal signal" && break
done
"${adb[@]}" shell am force-stop "$package"
grep -E "^(platform:|PASS|FAIL|INFO|SKIP|PLATFORM)" "$log" || true
if ! grep -q "^PLATFORM OK" "$log"; then
	grep -q "^PLATFORM" "$log" || echo "FAIL  no result from the game (crashed or hung; logcat in $log)"
	failures=1
fi
"${adb[@]}" uninstall "$package" > /dev/null || true

if [ $failures = 0 ]; then
	echo "ANDROID OK"
else
	echo "ANDROID FAILED ($failures)"
	exit 1
fi
