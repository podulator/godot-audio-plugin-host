# Signing and notarising the macOS build

Every macOS build in CI is signed **ad hoc** (no certificate) and verified. That proves the frameworks are built right, but a Mac still treats a downloaded copy as untrusted: users have to run `xattr -dr com.apple.quarantine addons/` before Godot can load it.

Signed with a **Developer ID** and **notarised** by Apple, the frameworks load on any Mac with no extra step. CI does this when **macOS: sign with the Developer ID and notarise** is ticked in *Build all platforms*, once the six secrets below exist. Without them that job stops with an error naming the missing secret, and nothing is half-signed.

## What it needs

An **Apple Developer Program** membership (99 USD a year), as the account holder (only the account holder may create Developer ID certificates).

## 1. The certificate, without a Mac

Apple's usual route is Xcode or Keychain Access, both macOS only. openssl does the same on Linux:

```sh
mkdir -p ~/Documents/private/apple && cd ~/Documents/private/apple   # anywhere private, never in a repo
openssl genrsa -out devid.key 2048
openssl req -new -key devid.key -out devid.csr -subj "/emailAddress=YOUR_APPLE_ID_EMAIL/CN=YOUR NAME/C=DE"
```

Then at <https://developer.apple.com/account/resources/certificates/add>: choose **Developer ID Application**, profile type **G2 Sub-CA**, upload `devid.csr`, and download the certificate (`developerID_application.cer`). Back in the same folder:

```sh
curl -fsSLO https://www.apple.com/certificateauthority/DeveloperIDG2CA.cer   # Apple's intermediate
openssl x509 -inform DER -in developerID_application.cer -out devid.pem
openssl x509 -inform DER -in DeveloperIDG2CA.cer -out g2ca.pem
# -legacy: an encryption macOS's keychain tool is sure to read. Choose a password; it is secret 2.
openssl pkcs12 -export -legacy -inkey devid.key -in devid.pem -certfile g2ca.pem -out devid.p12
openssl x509 -in devid.pem -noout -subject   # shows the identity name, for secret 4
```

Keep `devid.key` and `devid.p12` safe and out of every repository: they let anyone sign as you.

## 2. An app-specific password, for notarisation

At <https://account.apple.com>, **Sign-In and Security > App-Specific Passwords**, create one (name it e.g. "notarytool CI"). It is secret 6.

## 3. The six secrets

| secret | what |
| --- | --- |
| `APPLE_CERT_BASE64` | `base64 -w0 devid.p12` |
| `APPLE_CERT_PASSWORD` | the password chosen for `devid.p12` |
| `APPLE_DEV_ID` | your Apple ID (the email you sign in with) |
| `APPLE_DEV_APP_ID` | the identity: `Developer ID Application: YOUR NAME (TEAMID)`, as in the subject's CN |
| `APPLE_DEV_TEAM_ID` | the 10-character Team ID (developer.apple.com > Account > Membership details) |
| `APPLE_DEV_PASSWORD` | the app-specific password from step 2 |

Set them from the repository folder, each read from the terminal so none lands in shell history:

```sh
base64 -w0 ~/Documents/private/apple/devid.p12 | gh secret set APPLE_CERT_BASE64
gh secret set APPLE_CERT_PASSWORD   # then paste, Enter, Ctrl+D
gh secret set APPLE_DEV_ID
gh secret set APPLE_DEV_APP_ID
gh secret set APPLE_DEV_TEAM_ID
gh secret set APPLE_DEV_PASSWORD
```

## 4. Run it

*Actions > Build all platforms > Run workflow*, tick **macOS** and **macOS: sign with the Developer ID and notarise**. The macOS job then signs each framework (hardened runtime, Apple's timestamp), sends them to Apple, and waits for the verdict, usually a few minutes. If Apple refuses, the log shows Apple's own report of why.

A framework cannot carry its notarisation ticket the way an app can ("stapling"), so the first time a Mac sees it, it asks Apple online. Offline, a never-seen framework is treated as unsigned.

## The scripts

`.github/actions/sign-macos/`: `setup.sh` (a throwaway keychain with the certificate) and `sign.sh` (sign, verify, notarise).
