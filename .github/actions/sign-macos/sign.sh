#!/bin/bash
# Signs every macOS framework the build made (demo/addons/*/bin/*.framework).
#   sign.sh adhoc     ad-hoc signature + strict verify: proves the frameworks are signable.
#   sign.sh identity  Developer ID + hardened runtime + secure timestamp, then notarisation.
#
# Notarisation: Apple scans the code and records it as checked; Gatekeeper then lets it load on
# any Mac without the quarantine dance. A framework cannot be "stapled" (the ticket attached to
# the file), so a Mac looks the ticket up online the first time it sees the framework.
# Plain bash 3.2 (/bin/bash on macOS): no mapfile, no associative arrays.
set -eo pipefail
mode="$1"
frameworks=()
while IFS= read -r f; do
	frameworks+=("$f")
done < <(find demo/addons -maxdepth 4 -path "*/bin/*.framework" -type d | sort)
if [ ${#frameworks[@]} -eq 0 ]; then
	echo "ERROR: no frameworks under demo/addons/*/bin (did the build run?)"
	exit 1
fi
for f in "${frameworks[@]}"; do
	[ -f "$f/Resources/Info.plist" ] || { echo "ERROR: $f has no Resources/Info.plist (SConstruct writes it)"; exit 1; }
done

if [ "$mode" = adhoc ]; then
	for f in "${frameworks[@]}"; do
		codesign --force --sign - "$f"
		codesign --verify --strict --verbose=2 "$f"
	done
	echo "Ad-hoc signed and verified: ${#frameworks[@]} frameworks."
	exit 0
fi

for v in APPLE_DEV_ID APPLE_DEV_APP_ID APPLE_DEV_TEAM_ID APPLE_DEV_PASSWORD; do
	if [ -z "${!v}" ]; then
		echo "ERROR: secret $v is not set (see docs/macos-signing.md). Nothing was signed."
		exit 1
	fi
done
for f in "${frameworks[@]}"; do
	# --options runtime: the hardened runtime, which notarisation requires. --timestamp: Apple's
	# time server vouches for when it was signed, so the signature outlives the certificate.
	codesign --force --timestamp --options runtime --sign "$APPLE_DEV_APP_ID" "$f"
	codesign --verify --strict --verbose=2 "$f"
done

# One submission for all of them: notarytool takes one zip, and --wait blocks until Apple answers.
# ditto zips one folder, so the frameworks are gathered in one first (cp -R keeps the signatures).
stage="$RUNNER_TEMP/notarise"
archive="$RUNNER_TEMP/frameworks-to-notarise.zip"
rm -rf "$stage" "$archive"
mkdir -p "$stage"
for f in "${frameworks[@]}"; do
	cp -R "$f" "$stage/"
done
ditto -c -k "$stage" "$archive"
out=$(xcrun notarytool submit "$archive" --apple-id "$APPLE_DEV_ID" --team-id "$APPLE_DEV_TEAM_ID" \
	--password "$APPLE_DEV_PASSWORD" --wait --output-format json)
echo "$out"
id=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))')
status=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("status",""))')
if [ "$status" != "Accepted" ]; then
	echo "ERROR: notarisation $status. Apple's log:"
	[ -n "$id" ] && xcrun notarytool log "$id" --apple-id "$APPLE_DEV_ID" --team-id "$APPLE_DEV_TEAM_ID" --password "$APPLE_DEV_PASSWORD" || true
	exit 1
fi
rm -rf "$stage" "$archive"
echo "Signed with the Developer ID and notarised: ${#frameworks[@]} frameworks."
