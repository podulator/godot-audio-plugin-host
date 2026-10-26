#!/bin/sh
# Builds and runs the core tests, with AddressSanitizer and ThreadSanitizer in turn.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/tests/core/build"
mkdir -p "$out"
for san in address thread; do
	g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=$san -I"$root/src" \
		"$root/tests/core/test_core.cpp" "$root/src/core/effect_runtime.cpp" "$root/src/core/hosted_effect.cpp" \
		-o "$out/test_core_$san" -pthread
	echo "[$san]"
	"$out/test_core_$san"
	# The MIDI driver's core: header-only, main-thread code with made-up times.
	g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=$san -I"$root/src" "$root/tests/core/test_midi.cpp" -o "$out/test_midi_$san"
	"$out/test_midi_$san"
done
