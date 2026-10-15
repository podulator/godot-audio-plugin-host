#!/bin/bash
# The cross-platform test, for Linux, Windows (Git Bash) and macOS alike: builds this repository's
# plugins, installs them in the demo project's res://bin/clap and res://bin/vst3, runs
# demo/tests/platform_test.tscn headless, and checks the results file it writes.
# Needs: the extensions built (template_debug), `godot` on PATH, and SCons in .venv/bin.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
demo="$root/demo"
tmp="${GAPH_TEST_TMP:-${TMPDIR:-/tmp}/godot-audio-plugin-host-tests}"
mkdir -p "$tmp"
# SCons from PATH where installed (CI); else the project's .venv (this machine).
scons="$(command -v scons || echo "$root/.venv/bin/scons")"
# A time limit for each Godot run, so a hang fails the test instead of stalling it. macOS has no
# `timeout` (GNU coreutils); `gtimeout` is the Homebrew name for it, and Perl, on every Mac, can do
# the same with alarm(): the signal it sets survives the exec into Godot.
if command -v timeout > /dev/null; then
	limit() { timeout "$@"; }
elif command -v gtimeout > /dev/null; then
	limit() { gtimeout "$@"; }
else
	limit() { perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$@"; }
fi
# Paths handed to Godot must be native: on Windows (Git Bash), /tmp/... means nothing to it.
native() { if command -v cygpath > /dev/null; then cygpath -w "$1"; else echo "$1"; fi; }

"$scons" -C "$root/examples" -Q test_plugin=yes > /dev/null

rm -rf "$demo/bin"
mkdir -p "$demo/bin/clap" "$demo/bin/vst3"
trap 'rm -rf "$demo/bin"' EXIT
cp -R "$root/examples/build/clap_gain.clap" "$root/examples/build/gaph_test.clap" "$demo/bin/clap/"
cp -R "$root/examples/build/vst3_gain.vst3" "$demo/bin/vst3/"

# A headless first import of a project with GDExtensions can abort (godotengine/godot#111645)
# after importing; a second one confirms it.
limit 300 godot --headless --path "$demo" --import > /dev/null 2>&1 || true
limit 300 godot --headless --path "$demo" --import > /dev/null 2>&1 || true

results="$tmp/platform_test.txt"
rm -f "$results"
set +e
GAPH_MENU_CLASSES=1 PLATFORM_TEST_OUT="$(native "$results")" limit 300 godot --headless --path "$demo" res://tests/platform_test.tscn > "$tmp/platform_test.log" 2>&1
code=$?
set -e
if [ -f "$results" ]; then
	cat "$results"
else
	echo "FAIL  no results file (exit $code); Godot's output:"
	tail -40 "$tmp/platform_test.log"
	exit 1
fi
grep -q "^PLATFORM OK" "$results"
