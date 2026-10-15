#!/bin/sh
# Builds and runs the CLAP host tests, under AddressSanitizer: our own test plugin and the example,
# plus Dragonfly Reverb when installed (skipped, not failed, when it is not).
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/tests/clap/build"
mkdir -p "$out"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address -I"$root/src" -I"$root/thirdparty/clap/include" \
	"$root/tests/clap/test_clap.cpp" "$root/src/clap/clap_processor.cpp" "$root/src/clap/clap_library.cpp" "$root/src/core/x11_window.cpp" \
	-o "$out/test_clap" -ldl -pthread
# Our own test plugin, built here every time: it never skips (tests/plugins/gaph_test_plugin.c).
cc -std=c11 -Wall -Wextra -Werror -shared -fPIC -O2 -I"$root/thirdparty/clap/include" "$root/tests/plugins/gaph_test_plugin.c" -o "$out/gaph_test.clap"
"$(command -v scons || echo "$root/.venv/bin/scons")" -C "$root/examples" -Q -j"$(nproc)" > /dev/null
"$out/test_clap" "$out/gaph_test.clap" "$root/examples/build/clap_gain.clap"
