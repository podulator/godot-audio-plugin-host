#!/bin/sh
# The MIDI port backend outside Godot (Linux): the ALSA event layout against ALSA's headers, and a
# loopback through Midi Through. Skipped where ALSA's headers are not installed.
root="$(cd "$(dirname "$0")/../.." && pwd)"
if [ "$(uname -s)" != "Linux" ] || [ ! -f /usr/include/alsa/asoundlib.h ]; then
	echo "SKIP  ALSA backend test: no ALSA headers"
	exit 0
fi
out="$root/tests/midi/build"
mkdir -p "$out"
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address -I"$root/src" "$root/tests/midi/alsa_loopback.cpp" "$root/src/midi/ports_alsa.cpp" -o "$out/alsa_loopback" -ldl -pthread
"$out/alsa_loopback"
