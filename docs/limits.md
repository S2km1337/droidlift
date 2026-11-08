# Where droidlift does not generalise

droidlift has been used to port exactly **one** game so far. Everything below is either a hard limit of the approach or a gap that the first port never needed filled. Read this before picking a game.

## Hard limits

These come from the approach itself. Working around them means writing a different tool.

- **Native games only.**
  - The game logic must live in NDK `.so` libraries that are called through `Java_*` entry points or `JNI_OnLoad`.
  - Apps written in Java or Kotlin cannot be ported: there is no Dalvik or ART, and droidlift never runs `classes.dex`.
  - Games whose menus, text input or other UI are Android Views are only partly portable. Each of those screens has to be rebuilt by hand.
- **The Java side is rewritten by hand, once per game.**
  - Every Java method the native code calls (`CallStaticVoidMethod` and the like), every field it reads, and every `Java_*` entry point the activity calls has to be written in C in the game profile.
  - The work grows with the size of the Java shell. It is a few hundred lines for a thin shell around a C++ engine. For an engine that drives itself through reflection, or ships large Java libraries such as Google Play Services or ads SDKs, it can be unbounded.
- **32-bit ARM only.**
  - The CPU backends, the interpreter and the recompiler handle armeabi-v7a only: ARM, Thumb-2, VFPv3 and NEON.
  - **arm64-v8a is not supported.** Google Play has required 64-bit builds since August 2019, so many newer APKs ship only `lib/arm64-v8a`. Those cannot be ported without a second decoder, interpreter and emitter. Older APKs, and many that still ship both ABIs, work.
  - x86 and x86_64 libraries are not supported either.
- **No JIT on the iPhone.** A stock iPhone does not let an app create executable memory. On the device, only code that was recompiled ahead of time runs at native speed; anything else falls back to the interpreter. Games that generate machine code at run time (LuaJIT, a Mono JIT, a scripting VM with its own JIT) run that code interpreted, which may be too slow. The Mac and the Simulator have dynarmic.
- **Apple hosts only.**
  - You build on an Apple-silicon Mac.
  - The run targets are macOS, the iOS Simulator and an iPhone.
  - Timing uses `mach_absolute_time`, and the scripts and signing flow assume Xcode and zsh. dynarmic, SDL3 and ANGLE are portable, but no one has tried Linux or Windows.
- **You bring the game.** droidlift contains no game code. Each user supplies their own APK. The recompiled C and the unpacked assets are derived from the game, so they must stay out of public repositories and must not be distributed.

## Gaps: what the first port did not need

All of these could be built. They don't exist yet.

| Area | What exists | Not there |
|---|---|---|
| Graphics | OpenGL ES 2.0 through ANGLE's Metal backend; EGL handled by the toolkit; PVRTC 4bpp (the GPU's own, or decoded in software where ANGLE lacks it) | GLES 1.x fixed function, GLES 3.x, Vulkan; other compressed formats (ETC1, ATC, S3TC/DXT, ASTC) only work where ANGLE on that device supports them, with no software fallback |
| Audio | OpenSL ES buffer queues to SDL audio | AAudio, Oboe, `MediaPlayer`/`SoundPool` (Java), OpenSL ES file and URI players |
| libandroid | `AAssetManager`, `AAsset`, `AAssetDir` | `ANativeActivity`/`android_native_app_glue`, `ANativeWindow`, `ALooper`, `AInputQueue`, sensors, `AConfiguration` |
| bionic libc/libm | The roughly 270 functions the first game imports, with their 32-bit struct layouts | Anything else. `--check` lists every missing import, and each one is usually a short shim in `core/src/reimpl/` |
| Input | Touch (the mouse on the Mac); scripted touches for tests | Game controllers, keyboard text input, accelerometer, back button |
| Network | Sockets that fail cleanly, as on a phone with no connection | Any real networking: online play, leaderboards, in-app purchases, ads, cloud saves |
| Lifecycle | onPause, onStop, onRestart and onResume, mapped from iOS app state | Orientation changes, multi-window, surface loss and recreation (the GL context is kept) |
| Files | The data folder, a cache folder and assets from the APK, with atomic save replacement | APK expansion files (OBB), external storage, content providers |

## Things that work but need per-game effort

- **Recompiler discovery.** The recompiler finds functions from `.ARM.exidx`, dynamic symbols and the call graph. It also understands GCC's position-independent jump tables and Thumb/ARM interworking veneers. Code it misses runs interpreted, and `--cpu recomp` prints the hottest interpreted addresses so you can see what was missed. A different compiler, such as an old ARM RVCT or a hand-written assembly engine, may need new discovery patterns.
- **Size.** The first game recompiled to about 80 MB of C: about 100 s to build on an M4 and about 140 MB on disk. Large engines will take proportionally longer.
- **Guest memory.** The guest has a 32-bit address space, and droidlift reserves up to 4 GiB of host memory for it. On the iPhone, the app's memory limit applies. The `increased-memory-limit` entitlement can be requested, but a free Apple ID may not be granted it.
- **Game bugs.** Bugs that Android hides (locale, timezone or file-system differences, uninitialised memory) can show up. Load-time patches (`game_patches.h`) fix these in guest memory with guard bytes, without changing the APK.
- **Free Apple ID signing.** It works without a paid developer account, but Apple's limits apply: apps must be re-signed every 7 days (`resign-ios.sh`), only 3 such apps can be on a device at once, and entitlements are restricted.

## How to tell quickly whether a game fits

1. `unzip -l game.apk | grep '^.*lib/'`: is there `lib/armeabi-v7a/`?
2. Count the Java side. Decompile `classes.dex` (jadx) and look at the activity and every class that has `native` methods or is called from native code (`FindClass` strings in the `.so`). That list is your hand-written work.
3. `--check` after a first profile with just `libs` filled in reports every import droidlift lacks.
4. Look for `ANativeActivity_onCreate` (NativeActivity, not supported yet), `libGLESv3`/`libvulkan` imports, and `AAudio` imports.
