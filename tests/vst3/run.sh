#!/bin/sh
# Builds and runs the VST3 host tests, under AddressSanitizer: our own test plugin and the example,
# plus a third-party instrument (OneTrick URCHIN) when installed, to check instruments are refused.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/tests/vst3/build"
sdk="$root/thirdparty/vst3sdk"
mkdir -p "$out"
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-deprecated-declarations -fsanitize=address -DRELEASE=1 -I"$root/src" -I"$sdk" \
	"$root/tests/vst3/test_vst3.cpp" "$root"/src/vst3/vst3_processor.cpp "$root"/src/vst3/vst3_module.cpp "$root"/src/vst3/vst3_threadchecker.cpp \
	$(grep -v '^#' "$root/vst3_sdk_sources.txt" | sed "s|^|$sdk/|") \
	"$sdk/public.sdk/source/vst/moduleinfo/moduleinfocreator.cpp" \
	-o "$out/test_vst3" -ldl -pthread
# Our plugins, built here every time, so these tests never skip.
"$(command -v scons || echo "$root/.venv/bin/scons")" -C "$root/examples" -Q -j"$(nproc)" test_plugin=yes > /dev/null
ASAN_OPTIONS=fast_unwind_on_malloc=0 LSAN_OPTIONS="suppressions=$root/tests/vst3/lsan.supp:print_suppressions=0" "$out/test_vst3" "$root/examples/build/gaph_test_vst3.vst3" "$root/examples/build/vst3_gain.vst3"
