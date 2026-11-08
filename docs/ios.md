# iPhone and iOS Simulator

The game's target becomes `<APP_NAME>.app`. `droidlift/ios/bundle-app.sh` fills it with the APK's libraries and assets, ANGLE's frameworks and the launcher icon. Each script below finds the port through `droidlift.env` in the current directory or a parent directory.

## Dependencies

```sh
droidlift/scripts/fetch-ios-deps.sh          # SDL3 and ANGLE for the Simulator
droidlift/scripts/fetch-ios-deps.sh device   # and for the iPhone
```

## Simulator (no Apple ID)

```sh
droidlift/scripts/build-ios-sim.sh
droidlift/scripts/run-ios-sim.sh [--type iPhone-14] [--wait 25] [--show] [-- loader args]
```

The Simulator build defaults to the recompiled backend. Its C comes from the Mac build in `build/`, which the script configures on first use. `DL_SIM_CPU=dynarmic|interp` picks another backend. The script signs ad hoc, saves a screenshot to `build/ios-sim/shots/` and writes the log to `build/ios-sim/logs/`.

## iPhone with a free Apple ID

```sh
droidlift/scripts/build-ios-device.sh --install
```

One-time setup:

- Sign in to Xcode with your Apple ID (Settings > Accounts), which creates a "Personal Team".
- Turn on Developer Mode on the phone and connect it.

**Team and bundle id.** The team is taken from Xcode's preferences, or from `DL_TEAM_ID`. The bundle id is `<DL_BUNDLE_PREFIX>.<team id>`, because an App ID must be unique across all of Apple's teams. Both are written to `device.local.env` in your port, which is git-ignored and personal. The bundle id names the app's data container, so it must never change after the first install.

**Provisioning profile.** Apple issues free-team profiles only through Xcode, and they last 7 days.
- `scripts/ios-profile.sh` writes a stub Xcode project (`DroidliftSigning.xcodeproj`) with the same bundle id and team, outside iCloud in `DL_STAGING_DIR`. It builds the project with `xcodebuild -allowProvisioningUpdates`, which registers the phone and makes the profile.
- If that fails, the script prints the one-time Xcode steps.

**Signing.** `ios/sign-device.sh` signs a copy of the app made outside iCloud, because iCloud's file-provider attributes make codesign refuse bundles in synced folders. It uses the longest-lasting matching profile and its identity, plus the entitlements Xcode would use.

**Every week:** run `droidlift/scripts/resign-ios.sh`. It renews the profile, re-signs and installs over the old app, so the saves are kept.

**Logs.** stdout and stderr are copied to `Documents/logs/run-<time>.log` in the app's container (the last 5 runs), to `os_log` and to the console when one is attached. `droidlift/scripts/fetch-ios-log.sh` copies the logs to `build/ios-device/device-logs/`. They also show in Finder and in the Files app.

## Limits of a free team

- **No JIT,** so the device defaults to `--cpu recomp`. `interp` also works, but slowly.
- **No Extended Virtual Addressing.** The guest's 4 GiB reservation usually fits. If it doesn't, droidlift falls back to 3, 2, 1.5 or 1 GiB and logs it (`DL_GUEST_MB=N` forces a cap on the Mac, to test this).
- **Increased Memory Limit** may be requested with `DL_IOS_MORE_MEMORY=1`.
- Profiles last 7 days, and at most 3 such apps can be on a device.
