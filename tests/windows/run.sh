#!/bin/bash
# Windows plugin-window tests, built with MinGW and run under Wine on Linux, or natively on Windows:
# Win32Window on its own, then the CLAP and VST3 GUI paths with the host's real code and our test
# plugins' Windows GUIs (tests/gui/). With no display (DISPLAY and
# WAYLAND_DISPLAY unset) Wine creates and messages windows but shows nothing, so this never opens
# a window on screen. A scratch WINEPREFIX keeps the user's ~/.wine untouched. Skips without them.
set -eo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="${GAPH_TEST_TMP:-${TMPDIR:-/tmp}/godot-audio-plugin-host-tests}"; mkdir -p "$tmp"
# On Windows itself (Git Bash in CI): MinGW's gcc as it is installed there, and no Wine.
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) native=1 cc=gcc cxx=g++ ;;
*) native=0 cc=x86_64-w64-mingw32-gcc cxx=x86_64-w64-mingw32-g++ ;;
esac
if ! command -v "$cxx" > /dev/null || { [ $native = 0 ] && ! command -v wine > /dev/null; }; then
	echo "SKIP  needs MinGW ($cxx), and Wine when not on Windows"
	echo "WIN32 WINDOW OK (1 skipped)"
	exit 0
fi
"$cxx" -std=c++17 -O1 -Wall -Wextra -static -I"$root/src" \
	"$root/tests/windows/win32_window_test.cpp" "$root/src/core/win32_window.cpp" -o "$tmp/win32_window_test.exe" -lgdi32 -luser32
"$cc" -std=c11 -O1 -Wall -Wextra -Werror -shared -I"$root/thirdparty/clap/include" \
	"$root/tests/plugins/gaph_test_plugin.c" -o "$tmp/gaph_test.clap" -luser32
"$cxx" -std=c++17 -O1 -Wall -Wextra -static -I"$root/src" -I"$root/thirdparty/clap/include" \
	"$root/tests/gui/clap_gui_test.cpp" "$root/src/clap/clap_processor.cpp" "$root/src/clap/clap_library.cpp" \
	"$root/src/core/win32_window.cpp" -o "$tmp/clap_gui_test.exe" -lgdi32 -luser32
# VST3: the test plugin as a bundle (gaph_test_vst3.vst3/Contents/x86_64-win/gaph_test_vst3.vst3),
# and the test, with the VST3 host's sources and the SDK parts each needs.
sdk="$root/thirdparty/vst3sdk"
plugin_sdk=$(python3 - "$root/examples/SConstruct" << 'PY'
import re, sys
s = open(sys.argv[1]).read()
block = s[s.index("sdk_sources = ["):s.index("]", s.index("sdk_sources = ["))]
print(" ".join(re.findall(r'"([^"]+)"', block)))
PY
)
bundle="$tmp/gaph_test_vst3.vst3/Contents/x86_64-win"
mkdir -p "$bundle"
"$cxx" -std=c++17 -O1 -shared -static -DRELEASE=1 -Wno-deprecated-declarations -I"$sdk" "$root/tests/plugins/gaph_test_vst3.cpp" \
	$(for f in $plugin_sdk; do echo "$sdk/$f"; done) "$sdk/public.sdk/source/main/dllmain.cpp" \
	-o "$bundle/gaph_test_vst3.vst3" -lole32 -luser32
"$cxx" -std=c++17 -O1 -static -DRELEASE=1 -Wno-deprecated-declarations -I"$root/src" -I"$sdk" "$root/tests/gui/vst3_gui_test.cpp" \
	"$root/src/vst3/vst3_processor.cpp" "$root/src/vst3/vst3_module.cpp" "$root/src/vst3/vst3_threadchecker.cpp" "$root/src/core/win32_window.cpp" \
	$(grep -v '^#' "$root/vst3_sdk_sources.txt" | sed "s|module_linux.cpp|module_win32.cpp|; s|^|$sdk/|") \
	-o "$tmp/vst3_gui_test.exe" -lole32 -lshell32 -luuid -luser32 -lgdi32

if [ $native = 1 ]; then
	run() { "$@"; }
	plugin="$(cygpath -w "$tmp/gaph_test.clap")"
	vst3="$(cygpath -w "$tmp/gaph_test_vst3.vst3")"
else
	# winemenubuilder.exe=d: Wine would otherwise write file associations for the scratch prefix
	# into the user's real ~/.local/share/applications (seen 24.10.2026).
	export WINEPREFIX="$tmp/wineprefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
	unset DISPLAY WAYLAND_DISPLAY
	# Wine's invisible "null" display driver, asked for by name: up to Wine 11.19 it was chosen by
	# itself when there was no display, from 11.20 on no window can be created without this.
	if [ ! -f "$WINEPREFIX/.gaph-null-driver" ]; then
		timeout 120 wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f > /dev/null 2>&1
		wineserver -w
		touch "$WINEPREFIX/.gaph-null-driver"
	fi
	run() { timeout 120 wine "$@" 2> /dev/null; }
	plugin='Z:'"$(echo "$tmp/gaph_test.clap" | tr / '\\')"
	vst3='Z:'"$(echo "$tmp/gaph_test_vst3.vst3" | tr / '\\')"
fi
cd "$tmp"
run "$tmp/win32_window_test.exe" | tr -d '\r'
run "$tmp/clap_gui_test.exe" "$plugin" | tr -d '\r'
run "$tmp/vst3_gui_test.exe" "$vst3" | tr -d '\r'
