# Third-party code

Fetched by `scripts/fetch-deps.sh`, `scripts/fetch-angle.sh` and `scripts/fetch-ios-deps.sh` into the port's git-ignored `third_party/` directory (or installed with Homebrew), pinned as listed. Nothing here is committed to droidlift; each project's license notice stays in its own checkout. All of them are permissive and compatible with droidlift's MIT licence.

The loader's layout (`so_util`, `dynlib`, `reimpl/`, a FalsoJNI-style fake JNI) follows [stoicpingu/leos-fortune-vita](https://github.com/stoicpingu/leos-fortune-vita) (MIT), a PS Vita port. No code was copied from it.

| Name | Version | Source | License | Used for |
|---|---|---|---|---|
| dynarmic | commit `a46601580d5512d324104f985b5f0209dc980ddc` (2026-09-26) | https://github.com/azahar-emu/dynarmic (maintained fork; the original merryhime/dynarmic repo is gone) | 0BSD (`LICENSE.txt`) | ARMv7 JIT CPU backend on the Mac |
| fmt (dynarmic submodule) | `7b273fbb` (11.1.4+) | https://github.com/fmtlib/fmt | MIT | dynarmic dependency |
| mcl (dynarmic submodule) | `5fc4beaf` (0.1.14) | https://github.com/azahar-emu/mcl | MIT | dynarmic dependency |
| oaknut (dynarmic submodule) | `94c726ce` (2.0.2+) | https://github.com/merryhime/oaknut | MIT | dynarmic arm64 code emitter |
| robin-map (dynarmic submodule) | `054ec5ad` (1.3.0+) | https://github.com/Tessil/robin-map | MIT | dynarmic dependency |
| ANGLE | commit `09dcbb2998e5459118a1bc8f6ea1c386e3664f68` (branch chromium/8139, 2026-11-02) | https://chromium.googlesource.com/angle/angle (official) | BSD-3-Clause (`LICENSE`) | GLES 2.0 on Metal; fetched and built by `scripts/fetch-angle.sh`, loaded at run time. Its `gclient sync` pulls ANGLE's own pinned dependencies (DEPS) |
| depot_tools | commit `8defb316a8fccb3645e5bcf3ed078b757106c857` | https://chromium.googlesource.com/chromium/tools/depot_tools (official) | BSD-3-Clause (`LICENSE`) | build tooling for ANGLE (gclient, gn, siso) |
| Boost (headers) | 1.92.0 (Homebrew) | `brew install boost` | BSL-1.0 | dynarmic dependency |
| SDL3 | 3.4.18 (Homebrew) | `brew install sdl3` (upstream https://github.com/libsdl-org/SDL) | Zlib | Window, input and audio output on the Mac |
| SDL3 (iOS) | 3.4.18, release tarball `SDL3-3.4.18.tar.gz`, sha256 `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3` (matches GitHub's published digest; tag `release-3.4.18`, commit `829a65d769d935c4852f8159e964312c0957260a`) | https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz (official) | Zlib (`LICENSE.txt`) | Window, touch and audio in the iOS app; fetched and built static (arm64, iOS 18.0+) by `scripts/fetch-ios-deps.sh` for the iOS Simulator into `third_party/sdl3-ios-sim-arm64` and, for the iPhone(`fetch-ios-deps.sh device`), into `third_party/sdl3-ios-arm64` |
| ANGLE (iOS) | the same checkout as above (commit `09dcbb2998e5459118a1bc8f6ea1c386e3664f68`) | https://chromium.googlesource.com/angle/angle (official) | BSD-3-Clause (`LICENSE`) | GLES 2.0 on Metal in the iOS app: `libEGL.framework` and `libGLESv2.framework`, built by `scripts/fetch-ios-deps.sh` for the iOS Simulator into `third_party/angle/out/ios-sim-arm64` (`target_environment="simulator"`) and for the iPhone into `third_party/angle/out/ios-arm64` (`target_environment="device"`); Metal only, `target_os="ios"`; embedded in the app's `Frameworks/` |
