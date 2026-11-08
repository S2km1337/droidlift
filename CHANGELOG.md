# Changelog

droidlift follows [semantic versioning](https://semver.org) once it reaches 1.0. Until then, minor versions may change the profile API.

## Unreleased

## 0.1.0 (2026-11-08)

The first public release, split out of a working single-game port.

- **Loader:** ELF32/ARM loading and relocation into a 4 GiB guest space. Imports bind to other libraries, to host shims, to data blocks or to weak null. `--check` and `--report` list the bindings.
- **Shims:** about 270 bionic libc and libm functions, pthreads, stdio with atomic saves, GLES 2.0 forwarded to ANGLE on Metal (with software PVRTC), EGL, OpenSL ES buffer queues to SDL audio, and the AAsset manager.
- **Fake JNI:** the full `JNIEnv` and `JavaVM` tables, host-implemented Java methods, `RegisterNatives` and `JNI_OnLoad`. Unimplemented calls are logged with their signatures.
- **CPU backends:** dynarmic (JIT), an ARMv7 interpreter (ARM, Thumb-2, VFPv3, NEON), and an ahead-of-time ARMv7-to-C recompiler that falls back to the interpreter. Differential tests check each against dynarmic.
- **Game profiles:** `dl_game` hooks for start, surface, frame, touch and the activity lifecycle. `droidlift_add_game()` builds the loader, the recompiled libraries and the iOS app.
- **iOS:** a Simulator and device app shell, free-Apple-ID signing through a stub Xcode project, weekly re-signing, and log collection.
- **Docs:** porting guide, limits, architecture, recompiler, iOS, and a template port.

Known issue: the interpreter lacks one rare Thumb-2 dual-exclusive encoding that `--interp-test`'s random generator can produce with large test counts.
