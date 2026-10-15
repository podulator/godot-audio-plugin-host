#!/bin/bash
# Puts the Developer ID certificate into a temporary keychain that codesign can use without a
# password prompt. The certificate arrives as a base64 .p12 in a secret (docs/macos-signing.md).
# The steps are the standard ones for a CI runner, as GitHub documents them ("Installing an Apple
# certificate on macOS runners"). The runner is discarded after the job, keychain and all.
set -eo pipefail

for v in APPLE_CERT_BASE64 APPLE_CERT_PASSWORD; do
	if [ -z "${!v}" ]; then
		echo "ERROR: secret $v is not set (see docs/macos-signing.md). Nothing was signed."
		exit 1
	fi
done

certificate="$RUNNER_TEMP/developer-id.p12"
keychain="$RUNNER_TEMP/signing.keychain-db"
# A keychain needs a password of its own. Nobody ever types it: a random one, for this run only.
keychain_password="$(openssl rand -base64 24)"

printf '%s' "$APPLE_CERT_BASE64" | base64 --decode > "$certificate"

security create-keychain -p "$keychain_password" "$keychain"
# Locks again after 6 hours (-t, seconds), not on sleep (-l) or a timeout before then (-u).
security set-keychain-settings -lut 21600 "$keychain"
security unlock-keychain -p "$keychain_password" "$keychain"
# -T: codesign may use the key without asking. The partition list then allows Apple's own tools
# to use it silently, which macOS 10.12 and later require on top of -T.
security import "$certificate" -P "$APPLE_CERT_PASSWORD" -f pkcs12 -k "$keychain" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple: -s -k "$keychain_password" "$keychain" > /dev/null
# The search list codesign looks in: ours first, then the user's usual login keychain.
security list-keychains -d user -s "$keychain" "$HOME/Library/Keychains/login.keychain-db"

rm -f "$certificate"
echo "Signing identities available:"
security find-identity -v -p codesigning "$keychain"
