# Architecture

How a game's ARM code ends up running on a Mac or an iPhone, one layer at a time. File names are under `core/src/` unless noted.

```text
 host process (arm64, macOS or iOS)
 +----------------------------------------------------------------------------+
 |  dl_main: command line, load, self-tests, frame loop      <-- dl_game hooks |
 |                                                                            |
 |  guest address space: one 4 GiB reservation, guest address = g_mem offset  |
 |  +------------------+  +-----------+  +-----------+  +------------------+  |
 |  | libgame.so  .text|  | guest heap|  | thread    |  | thunks:          |  |
 |  |  .data .bss      |  | (malloc)  |  | stacks    |  |  svc #n ; bx lr  |  |
 |  +--------+---------+  +-----------+  +-----------+  +--------+---------+  |
 |           | runs on                                           | traps to   |
 |  +--------v-----------------------+                 +---------v---------+  |
 |  | CPU backend (cpu.h)            |  -- svc -->     | dispatch          |  |
 |  |  dynarmic | interp | recomp    |                 |  dynlib: imports  |  |
 |  +--------------------------------+                 |  fake JNI: JNIEnv |  |
 |                                                     +---------+---------+  |
 |                                                               |            |
 |  host shims (reimpl/): bionic libc/libm, pthreads, stdio, GLES 2, EGL,     |
 |  OpenSL ES, AAsset   -->   ANGLE (Metal) . SDL3 (window, touch, audio)     |
 +----------------------------------------------------------------------------+
```

## Guest memory (`guest_mem.h`)

The guest is a 32-bit ARM process, so its whole address space fits in one 4 GiB host reservation. A guest address is an offset from `g_mem`. `g2h` and `h2g` convert between guest and host pointers, and `gm_r32` and `gm_w32` read and write guest words. Every guest allocation comes from `gm_alloc`: libraries, the heap, stacks and thunk pages. Where the host refuses 4 GiB of address space (an iPhone app without a paid entitlement), the reservation shrinks to 3, 2, 1.5 or 1 GiB and allocation stays below it.

The guest heap (`reimpl/mem.c`) is a size-class allocator in one guest region, with 16-byte alignment and one lock. Guest threads are host threads, so it must be thread-safe.

## Loading (`so_util.c`, `apk.c`)

`apk.c` unpacks `lib/armeabi-v7a/*.so` and `assets/` from the APK. `so_load` maps each library's `PT_LOAD` segments into guest memory. `so_relocate` applies `R_ARM_RELATIVE`, `ABS32`, `GLOB_DAT` and `JUMP_SLOT` and binds every undefined symbol in this order:

1. to another loaded library that exports it;
2. to a droidlift **import thunk** (`dynlib.c`);
3. to a zeroed **data block** for imported objects (`__sF`, `_ctype_`...);
4. to 0 for weak imports Android also leaves unresolved.

`--check` fails if anything is left. `--report` writes every binding to a TSV file.

After relocation, the game's **load-time patches** are applied in guest memory (`game_patches.h`). Then, as Android's linker and `System.loadLibrary` do, each library's constructors run, followed by its `JNI_OnLoad`.

## Imports: thunks and shims (`dynlib.c`, `reimpl/`)

Each imported function becomes two ARM instructions in guest memory:

```text
svc  #(0x800000 | index)
bx   lr
```

The CPU backend traps the `svc`, and `cpu_svc` hands the index to `dynlib_dispatch`, which calls the host implementation. A host implementation reads its arguments with `cpu_arg(c, i)` (AAPCS: r0-r3, then the stack) and returns with `cpu_ret` or `cpu_ret64`. armeabi-v7a is **softfp**: floats and doubles travel in core registers as bit patterns.

The shims translate bionic's 32-bit ABI to the host's: struct layouts (`stat`, `tm`, `dirent`, `pthread_attr_t`, `FILE`), errno values and flags. They also enforce the phone's rules:
- File access is limited to the app's data directory, and a root check on `/system` fails as on an unrooted phone.
- Sockets fail cleanly, as on a phone with no connection.
- A save the game writes appears atomically when the file is closed (`<path>.dl-new` renamed over the original).

A function pointer the guest must call back into the host, such as an OpenSL ES vtable slot, is a **host thunk** (`dynlib_host_thunk`, `svc #0x42xxxx`).

## The fake JNI (`fake_jni.c`)

`JNIEnv` and `JavaVM` are guest tables of thunks (`svc #0x40xxxx`), so native code calls them like the real ones. Objects are opaque 32-bit handles:
- strings keep their UTF-8;
- primitive arrays keep their elements in guest memory, so `Get<Type>ArrayElements` can return direct pointers;
- object arrays hold handles.

`FindClass` resolves the android/java basics plus the profile's `jni_classes`. Any other class fails with a pending `NoClassDefFoundError`, as a class missing from `classes.dex` would.

Java methods that native code calls are host functions the profile registers by class, name and signature (`jni_register_method`). The variadic, `va_list` and `jvalue[]` call forms all arrive unpacked in a `jni_call`. A method nobody registered is logged with its signature, returns 0 and is counted, so `--jni-test` and the smoke tests can fail on it.

`RegisterNatives` calls are recorded, and `dl_native` finds a native method either there or as an exported `Java_<class>_<method>` symbol with JNI's name mangling.

## CPU backends (`cpu.h`)

The rest of droidlift depends only on `cpu_ops`: registers, run, halt and invalidate. Three implementations:

- **dynarmic** (`cpu_dynarmic.cpp`): a JIT with fastmem over the guest reservation. It runs on the Mac and in the Simulator, and it is the reference the others are tested against.
- **interp** (`interp/`): droidlift's own ARMv7 interpreter for ARM, Thumb-2, VFPv3 and NEON.
- **recomp** (`cpu_recomp.c` plus generated C): see [recompiler.md](recompiler.md).

`cpu_call(c, fn, n, args)` runs a guest function to completion. It sets up AAPCS arguments, points LR at a return-sentinel thunk and runs until the sentinel is hit. When a shim has to call back into the guest (`qsort`'s comparator, `pthread_once`), it uses a nested cpu on the same host thread.

## Threads

`pthread_create` in the guest makes a host thread with its own cpu of the same backend and its own guest stack. Per-thread errno and thread-specific data live in host thread-locals. Mutexes, condition variables and `pthread_once` map onto host primitives with bionic's 32-bit layouts. Guest threads that never end (audio mixers, worker pools) are why droidlift exits with `_exit` instead of tearing guest memory down under them.

## Graphics, audio and input

- **GLES 2.0** (`reimpl/gles.c`): each call is forwarded to ANGLE, which runs on Metal, with guest pointers translated.
  - GL strings and limits are answered on the host.
  - PVRTC is decoded in software where the GPU lacks it.
  - Every texture upload is checked, and failures are counted.
  - For a pillarbox, viewports, scissors and pixel reads are offset into the game's rectangle.
- **EGL**: droidlift owns the display, surface and context (`gfx.c`), as `GLSurfaceView` does on Android. The game sees only `eglGetProcAddress`.
- **OpenSL ES** (`reimpl/opensles.c`): the engine, an output mix, and a player fed by an Android simple buffer queue. A feeder thread hands buffers to SDL audio (`audio.c`) and calls the game's buffer-queue callback, as Android's audio thread does. `DL_AUDIO=off` swaps the device for a virtual clock that still counts samples, for the tests.
- **Touch** (`input.c`): host pointer events and scripted test touches are queued, then delivered on the render thread before the next frame through the profile's `touch` hook.

## The frame loop and lifecycle (`dl_main.c`, `startup.c`, `lifecycle.c`)

`--init-test` runs, in this order:

1. constructors, then `JNI_OnLoad`;
2. the profile's `start`;
3. the window or headless surface (with a pillarbox if the screen is wider than `max_aspect`);
4. the profile's `surface`;
5. the loop: poll events, the `frame` hook, swap and pace to 60 fps.

At the end it runs `stop`, which is when Android games save.

On iOS, app-state notifications drive `dl_app_pause`, `dl_app_stop` and `dl_app_resume`. Each calls the profile's hook once per state change, and frames stop while the app is paused.

## iOS (`ios/`)

On iOS the loader becomes the app. SDL owns the real `main` (`dl_entry.c`), the APK's libraries and assets are copied into the bundle at build time and opened with `--unpacked`, and `ios_log.c` tees stdout and stderr into `Documents/logs/` and `os_log`. Signing and installing are covered in [ios.md](ios.md).
