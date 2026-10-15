#!/bin/bash
# The scan guard, end to end, with a plugin built to crash on load (crasher.c):
#   1. a scan with it installed dies, and the log's last line names it;
#   2. the next start warns, skips it, and still lists the other plugins;
#   3. touching the file (as an update would) gets it retried: it crashes and is blocked again.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
# Scratch space: GAPH_TEST_TMP, or a folder in the system temp dir (works on any machine, CI too).
tmp="${GAPH_TEST_TMP:-${TMPDIR:-/tmp}/godot-audio-plugin-host-tests}"; mkdir -p "$tmp"
work="$tmp/scan_guard_test"
rm -rf "$work"
mkdir -p "$work/plugins" "$work/state"
gcc -shared -fPIC -I"$root/thirdparty/clap/include" "$root/tests/scan_guard/crasher.c" -o "$work/plugins/Crasher.clap"

scan() {
	GAPH_SCAN_STATE_DIR="$work/state" GAPH_MENU_CLASSES=1 CLAP_PATH="$work/plugins" \
		timeout 60 godot --headless --path "$root/demo" res://tests/menu_test.tscn > "$work/out.txt" 2>&1
}
fails=0
check() {
	if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}

set +e
scan; code=$?
set -e
check "a scan with the crasher installed dies (exit $code)" '[ $code -ne 0 ]'
last=$(tail -1 "$work/state/clap_scan.log")
check "the log's last line names it: '${last#* }'" '[[ "$last" == *"found"*"Crasher.clap"* ]]'
check "the state file still marks it in progress" 'grep -q "in_progress=\".*Crasher.clap\"" "$work/state/clap_scan.cfg"'

set +e
scan; code=$?
set -e
# "Survives" = the scan ran to its summary line. The scene's own checks are about other things
# (it expects the VST3 folder too), so its exit status is not the measure here.
check "the next start survives: the scan finishes (exit $code, not a crash)" '[ $code -ne 134 ] && grep -q "CLAP scan: .* skipped as blocked" "$work/out.txt"'
check "  ...warning that Crasher was skipped" 'grep -q "crashed while loading .*Crasher.clap" "$work/out.txt"'
check "  ...and still lists the other plugins" 'grep -q "Clap_DragonflyHallReverb" "$work/out.txt" || [ ! -f /usr/lib/clap/DragonflyHallReverb.clap ]'
check "  ...the log says BLOCKED, then skipped" 'grep -q "BLOCKED .*Crasher" "$work/state/clap_scan.log" && grep -q "skipped .*Crasher" "$work/state/clap_scan.log"'

sleep 1; touch "$work/plugins/Crasher.clap"
set +e
scan; code=$?
set -e
check "after an update (touch) it is retried, and crashes again (exit $code)" '[ $code -ne 0 ] && grep -q "retrying .*Crasher" "$work/state/clap_scan.log"'
set +e
scan; code=$?
set -e
check "...and the start after that skips it again (exit $code)" '[ $code -ne 134 ] && grep -q "skipped .*Crasher" "$work/state/clap_scan.log"'

echo "SCANGUARD $([ $fails -eq 0 ] && echo OK || echo "FAILED ($fails)")"
[ $fails -eq 0 ]
