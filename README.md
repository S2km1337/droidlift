<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/banner-dark.svg">
    <img src="docs/assets/banner-light.svg" width="100%" alt="droidlift: native Android games on macOS and iPhone">
  </picture>
</p>

# droidlift

Run a native Android game on macOS and on a stock iPhone, with no Android, no emulator and no paid Apple developer account.

droidlift loads the game's own `armeabi-v7a` libraries from the APK you own, binds their imports to host implementations of bionic, OpenGL ES 2.0, OpenSL ES and the asset manager, and runs their ARM code in one of three ways:

| Backend | How | Where |
|---|---|---|
| `dynarmic` | a JIT ([dynarmic](https://github.com/azahar-emu/dynarmic)) | Mac, iOS Simulator |
| `interp` | droidlift's own ARMv7 interpreter (ARM, Thumb-2, VFPv3, NEON) | everywhere |
| `recomp` | the libraries **recompiled ahead of time to C** and compiled with clang, with the interpreter for anything the recompiler missed | everywhere, and the only fast one on an iPhone (iOS forbids JIT) |

Graphics go through [ANGLE](https://chromium.googlesource.com/angle/angle)'s Metal backend. Window, touch and audio go through [SDL3](https://libsdl.org). On the iPhone the app is signed with a free Apple ID ("Personal Team") through a stub Xcode project, and the scripts re-sign it every week.

Everything the game's Java side did stays your job: which `Java_*` entry points to call and when, and the Java methods native code calls back. droidlift gives you a fake JNI to write them against and a profile struct to hook them into. [docs/limits.md](docs/limits.md) says plainly which games this fits and which it doesn't. Read it first.

> **Status:** experimental. One game, a 2010s C++ engine with FMOD audio, runs completely on all three backends, on the Mac and on an iPhone 14: title, gameplay, sound, touch and saves. Expect to extend the shims for your game.

## What a port looks like

A port is a separate repository that holds droidlift as a submodule, a short CMake file, a `droidlift.env` and a game profile in C:

```c
#include "droidlift.h"

static const char *const libs[] = {"libengine.so", "libgame.so", NULL};
static const char *const classes[] = {"com/example/game/GameActivity", NULL};

static int init(const char *data_dir) { /* jni_register_method(...) for the Java side */ return 0; }
static int start(cpu *c, const so_module *m) { /* Java_..._onCreate */ return 0; }
static int surface(cpu *c, const so_module *m, uint32_t w, uint32_t h) { /* onSurfaceChanged */ return 0; }
static int frame(cpu *c, const so_module *m) { input_dispatch(c, m); /* onDrawFrame */ return 0; }

static const dl_game game = {
    .name = "My Game", .storage = "My Game", .libs = libs, .jni_classes = classes,
    .init = init, .start = start, .surface = surface, .frame = frame,
};
DL_MAIN(&game)
```

```cmake
add_subdirectory(droidlift)
droidlift_add_game(my-game
  APK "${DL_APK}"
  SOURCES game/profile.c
  RECOMP "libengine:engine:8" "libgame:game:32")
```

[docs/porting.md](docs/porting.md) walks through a port from an empty profile to a playable game. [examples/template](examples/template) is a starting point you can copy.

## Requirements

- An Apple-silicon Mac with Xcode (27 or later for the iPhone scripts), CMake 3.24+, Ninja, and Homebrew `sdl3` and `boost`.
- The game's APK, with `lib/armeabi-v7a/`.
- For the iPhone: a free Apple ID signed in to Xcode, and the phone in Developer Mode.

## Build and run on the Mac

From your port's directory:

```sh
droidlift/scripts/fetch-deps.sh     # dynarmic, pinned
droidlift/scripts/fetch-angle.sh    # ANGLE for macOS (a long first build)
cmake -S . -B build -G Ninja -DDL_APK=$PWD/original/game.apk
cmake --build build
build/my-game --apk original/game.apk --check            # every import bound?
build/my-game --apk original/game.apk --init-test --window
```

Useful flags: `--cpu dynarmic|interp|recomp`, `--frames N --screenshot out.png` (headless), `--touch` scripts for tests, and the self-tests `--cpu-test`, `--interp-test`, `--jni-test` and `--fmt-test`. `--help` lists them all.

## iPhone and Simulator

```sh
droidlift/scripts/build-ios-sim.sh && droidlift/scripts/run-ios-sim.sh
droidlift/scripts/build-ios-device.sh --install   # free Apple ID; see docs/ios.md
droidlift/scripts/resign-ios.sh                   # every 7 days
```

## Layout

| Path | What |
|---|---|
| `core/src/` | The loader: ELF loading and relocation (`so_util`), import binding (`dynlib`), the shims (`reimpl/`), the fake JNI, GL, audio and input, the CPU backends and the self-tests. `droidlift.h` is the API for ports. |
| `core/src/interp/` | The ARMv7 interpreter |
| `recomp/` | The ahead-of-time recompiler (ARMv7 ELF to C), its runtime and its differential tests |
| `ios/` | The iOS app shell, logging, bundling and free-team signing |
| `cmake/droidlift.cmake` | `droidlift_add_game` and the recompiler test helpers |
| `scripts/` | Dependency fetching, the iOS build, run and sign scripts, analysis helpers |
| `docs/` | Porting guide, limits, iOS, recompiler design |

## Legal

droidlift contains no game code or assets. You supply your own copy of a game. Output derived from a game (unpacked libraries and assets, recompiled C, app bundles) belongs in ignored folders and must not be published or distributed. droidlift is MIT-licensed. Its dependencies are listed in [THIRD_PARTY.md](THIRD_PARTY.md) and are all permissive.
