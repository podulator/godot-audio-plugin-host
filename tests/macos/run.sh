#!/bin/bash
# Plugin windows on macOS (skips elsewhere): CocoaWindow on its own, then the CLAP and VST3 GUI
# paths with the host's real code and our test plugins' macOS GUIs (tests/gui/, the same tests as
# on Windows). Windows open briefly, which is fine on a CI runner; on a Mac they flash up.
set -eo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="${GAPH_TEST_TMP:-${TMPDIR:-/tmp}/godot-audio-plugin-host-tests}"; mkdir -p "$tmp"
if [ "$(uname -s)" != Darwin ]; then
	echo "SKIP  macOS only"
	echo "COCOA WINDOW OK (1 skipped)"
	exit 0
fi
clang++ -std=c++17 -fobjc-arc -Wall -Wextra -I"$root/src" "$root/tests/macos/cocoa_window_test.mm" "$root/src/core/cocoa_window.mm" \
	-framework Cocoa -o "$tmp/cocoa_window_test"

"$(command -v scons || echo "$root/.venv/bin/scons")" -C "$root/examples" -Q test_plugin=yes > /dev/null
clang++ -std=c++17 -x objective-c++ -fobjc-arc -Wall -Wextra -I"$root/src" -I"$root/thirdparty/clap/include" \
	"$root/tests/gui/clap_gui_test.cpp" "$root/src/clap/clap_processor.cpp" "$root/src/clap/clap_library.cpp" \
	"$root/src/core/cocoa_window.mm" -framework Cocoa -o "$tmp/clap_gui_test"
"$tmp/clap_gui_test" "$root/examples/build/gaph_test.clap"
"$tmp/cocoa_window_test"

"$(command -v scons || echo "$root/.venv/bin/scons")" -C "$root/examples" -Q test_plugin=yes > /dev/null
clang++ -std=c++17 -x objective-c++ -fobjc-arc -Wall -Wextra -I"$root/src" -I"$root/thirdparty/clap/include" \
	"$root/tests/gui/clap_gui_test.cpp" "$root/src/clap/clap_processor.cpp" "$root/src/clap/clap_library.cpp" \
	"$root/src/core/cocoa_window.mm" -framework Cocoa -o "$tmp/clap_gui_test"
"$tmp/clap_gui_test" "$root/examples/build/gaph_test.clap"

# VST3: the test file is Objective-C++ (it looks into AppKit views); "-x none" puts the rest back
# to being judged by extension (.cpp C++, .mm Objective-C++).
sdk="$root/thirdparty/vst3sdk"
clang++ -std=c++17 -fobjc-arc -Wall -Wno-deprecated-declarations -Wno-pragma-pack -DRELEASE=1 -I"$root/src" -I"$sdk" \
	-x objective-c++ "$root/tests/gui/vst3_gui_test.cpp" -x none \
	"$root/src/vst3/vst3_processor.cpp" "$root/src/vst3/vst3_module.cpp" "$root/src/vst3/vst3_threadchecker.cpp" "$root/src/core/cocoa_window.mm" \
	$(grep -v '^#' "$root/vst3_sdk_sources.txt" | sed "s|module_linux.cpp|module_mac.mm|; s|^|$sdk/|") \
	-framework Cocoa -framework CoreFoundation -o "$tmp/vst3_gui_test" 2>&1 | grep -E "error" || true
"$tmp/vst3_gui_test" "$root/examples/build/gaph_test_vst3.vst3"
