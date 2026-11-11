# macOS Release and Distribution

This process builds the Release app bundle with its Qt runtime, signs it with
Developer ID, submits it to Apple's notary service, staples the ticket, and
creates an archive for distribution.

## Requirements

- macOS with Xcode Command Line Tools and `xcrun`.
- CMake 3.21.1+, Ninja, and the Qt 6.12 macOS arm64 kit, including Qt Quick,
  Quick Controls, Quick Dialogs, and Qt SVG.
- For public distribution: an Apple Developer Program account and a
  **Developer ID Application** certificate installed in the login keychain.
- For notarization: an app-specific password for an Apple ID associated with
  the developer team. An App Store Connect API key can be used instead.

Konsoll currently targets arm64 and macOS 14.4 or later. The app displays the
latest Git tag (without the leading `v`) and the build commit, while the macOS
bundle version remains the numeric project version (`2.0.0` for this beta
line). Tag each release before building it. Do not change the bundle identifier
between releases.

## 1. Configure and build the signed bundle

Find the Developer ID signing identity in Keychain Access or with:

```sh
security find-identity -v -p codesigning
```

Configure a clean Release build. Replace the Qt path and signing identity with
the values on the release machine:

```sh
cmake -S . -B build/release -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.12.0/macos/lib/cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DKONSOLL_CODESIGN_IDENTITY="Developer ID Application: Name (TEAMID)"

cmake --build build/release --config Release --target package-macos
```

`package-macos` builds and installs the bundle to:

```text
build/release/dist/konsoll.app
```

The install step deploys Qt frameworks, plugins, and QML imports into the app.
The `.app` under `build/release/konsoll.app` is the build-tree executable; do
not distribute that one. Qt's [CMake deployment API](https://doc.qt.io/qt-6/cmake-deployment.html)
performs this dependency deployment. With `KONSOLL_CODESIGN_IDENTITY` set,
the deployment tool signs the bundle with the hardened runtime and a secure
timestamp, as required for notarization.

Check the resulting signature and dependencies:

```sh
APP_BUNDLE="build/release/dist/konsoll.app"
codesign --verify --deep --strict --verbose=2 "$APP_BUNDLE"
otool -L "$APP_BUNDLE/Contents/MacOS/konsoll"
```

The Qt library entries from `otool` should point inside the app bundle using
`@loader_path/../Frameworks`, not to the Qt installation used to build it.

## 2. Save notarization credentials in Keychain

Create an app-specific password in the Apple ID account, then store it in the
Keychain. `notarytool` prompts for the password and keeps it in the named
profile:

```sh
xcrun notarytool store-credentials "KonsollNotary" \
  --apple-id "you@example.com" \
  --team-id "TEAMID"
```

This is a one-time setup per Mac/keychain. Apple documents this profile-based
workflow in its [notarytool migration guide](https://developer.apple.com/documentation/technotes/tn3147-migrating-to-the-latest-notarization-tool).

An App Store Connect API key can be stored instead of an Apple ID password:

```sh
xcrun notarytool store-credentials "KonsollNotary" \
  --key /path/to/AuthKey_KEYID.p8 \
  --key-id "KEYID" \
  --issuer "ISSUER-UUID"
```

## 3. Create and submit a ZIP archive

Use `ditto` to preserve the app bundle's macOS metadata:

```sh
APP_BUNDLE="build/release/dist/konsoll.app"
SUBMISSION_ZIP="build/release/Konsoll-2.0.0-beta.1-arm64.zip"

ditto -c -k --sequesterRsrc --keepParent "$APP_BUNDLE" "$SUBMISSION_ZIP"
xcrun notarytool submit "$SUBMISSION_ZIP" \
  --keychain-profile "KonsollNotary" \
  --wait
```

The submission must finish with status `Accepted`. If it is rejected, use the
submission ID printed by `notarytool` to retrieve Apple's diagnostic log:

```sh
xcrun notarytool log SUBMISSION_ID \
  --keychain-profile "KonsollNotary" \
  build/release/notarization-log.json
```

Fix reported signing or packaging issues, rebuild, and submit a new archive.

## 4. Staple and validate the ticket

Staple the accepted ticket to the app bundle, then validate it:

```sh
APP_BUNDLE="build/release/dist/konsoll.app"
xcrun stapler staple "$APP_BUNDLE"
xcrun stapler validate "$APP_BUNDLE"
spctl --assess --type execute --verbose=4 "$APP_BUNDLE"
```

ZIP files cannot be stapled directly. Recreate the final ZIP after stapling the
app, then distribute this final archive:

```sh
FINAL_ZIP="build/release/Konsoll-2.0.0-beta.1-arm64-notarized.zip"
ditto -c -k --sequesterRsrc --keepParent "$APP_BUNDLE" "$FINAL_ZIP"
```

Apple's [notarization guide](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)
requires Developer ID signing for software distributed outside the Mac App
Store. Its [custom workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow)
describes submitting archives and stapling the resulting ticket.

## Optional: distribute a disk image

Create a compressed DMG containing the already stapled app:

```sh
hdiutil create \
  -volname "Konsoll" \
  -srcfolder "$APP_BUNDLE" \
  -ov -format UDZO \
  build/release/Konsoll-2.0.0-beta.1-arm64.dmg
```

If distributing the DMG, submit that DMG to `notarytool` as well, then run
`xcrun stapler staple` and `xcrun stapler validate` on the DMG before publishing
it. The ZIP workflow above staples the app and then recreates the ZIP.

## CI/CD (GitHub Actions)

`.github/workflows/ci.yml` runs on every push to `main` and every pull request
on an Apple Silicon runner (`macos-26`): it installs Qt 6.12 with
[install-qt-action](https://github.com/jurplel/install-qt-action) (aqtinstall),
checks formatting, builds Release, runs
`konsoll --selftest` with `QT_QPA_PLATFORM=offscreen` and uploads an unsigned
`.app` ZIP as a workflow artifact.

`.github/workflows/release.yml` runs when a `v*` tag is pushed: it builds the
Release bundle, runs the self-test, ad-hoc signs it (`codesign --sign -`),
and publishes a GitHub Release with a ZIP, a DMG and `SHA256SUMS.txt`. It
does not use a Developer ID, so the app is not notarized and users clear the
quarantine flag once (README, "Install"). Steps 1–4 above remain the manual
path for a Developer ID-signed, notarized build.

Release:

```sh
git tag v3.5.0
git push origin v3.5.0
```
