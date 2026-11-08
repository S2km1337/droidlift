# Porting a game

This guide takes a game from an APK to a playable Mac build, then to the iPhone. It assumes you have read [limits.md](limits.md) and the game passes its quick check: `lib/armeabi-v7a/` exists, and the game logic is native.

Throughout, *main library* means the last library in your profile's `libs` list, usually the game itself.

## 1. Set up the port's repository

Copy [examples/template](../examples/template) somewhere, then:

```sh
git init my-port && cd my-port
git submodule add <droidlift's URL> droidlift
mkdir original && cp ~/path/to/game.apk original/      # ignored by .gitignore
```

The template contains:

- `CMakeLists.txt`: one `droidlift_add_game()` call.
- `droidlift.env`: the app name and APK path for the scripts.
- `.gitignore`: keeps the APK, build trees and generated C out of git.
- `game/profile.c`: an empty profile.

Before you write anything, set two things in the template:

- **`libs`**: the libraries in the order the Java side calls `System.loadLibrary`. Search the decompiled Java for `loadLibrary`. The game usually goes last, after its middleware (FMOD, OpenAL, a physics engine).
- **`DL_APP_NAME`** in `droidlift.env`, and `APP_NAME` in `CMakeLists.txt`.

Build without the recompiler at first. It is slow to generate, and you don't need it until the game runs:

```sh
droidlift/scripts/fetch-deps.sh && droidlift/scripts/fetch-angle.sh
cmake -S . -B build -G Ninja -DDL_RECOMP=OFF
cmake --build build
```

## 2. Make every import bind

```sh
build/my-game --apk original/game.apk --check --report build/imports.tsv
```

`--check` loads and relocates every library and binds each import to another loaded library, to a droidlift shim, or (for weak imports) to 0. It fails if anything is left unresolved. `build/imports.tsv` lists every binding.

For each missing import:

- **libc, libm, libdl, liblog:** add a `STUB(...)` entry to `core/src/dynlib.c` (the table is sorted per library) and an implementation in the matching `core/src/reimpl/*.c` table.
  - Guest arguments come from `cpu_arg(c, i)` and are returned with `cpu_ret`.
  - Guest pointers become host pointers with `g2h`.
  - Bionic's 32-bit struct layouts (`stat`, `tm`, `dirent`...) differ from the host's, so convert them field by field the way the existing shims do.
  - A `STUB` with no implementation logs each call and returns 0, which is enough for functions the game calls but doesn't rely on.
- **GLES 2.0:** add the function to `GL_FUNCS` in `core/src/reimpl/gles.c`. Most GL calls forward to ANGLE unchanged.
- **Something droidlift doesn't cover at all** (GLES 3, AAudio, NativeActivity): see [limits.md](limits.md). That is new toolkit work.

Please send shim additions upstream. They are what makes the next port cheaper.

## 3. Write the Java side

Decompile the APK (`jadx -d out game.apk`) and read three things:

1. **The activity and renderer.** Which natives does the activity call in `onCreate`, `onSurfaceCreated`/`onSurfaceChanged`, `onDrawFrame`, `onPause`/`onResume`, and on touch events? These calls become your profile's hooks.
2. **Which classes native code looks up.** Run `strings -a lib/armeabi-v7a/libgame.so | grep '^com/'` and keep the ones that exist in the decompiled Java. They go in `jni_classes`. A class not in the list fails `FindClass` the way a class missing from `classes.dex` would.
3. **Which Java methods native code calls.** Run the game and watch stderr. Every `GetMethodID`/`GetStaticMethodID` is logged with its signature, and a call to a method you haven't implemented is logged as `[jni] ... no host implementation` and counted.

Implement each method in `init` with `jni_register_method`:

```c
static uint64_t get_files_dir(const jni_call *call) {
  (void)call;
  return jni_new_string(files_dir); /* a Java String, as a jobject handle */
}

static int init(const char *data_dir) {
  snprintf(files_dir, sizeof files_dir, "%sfiles/", data_dir);
  jni_register_method("com/example/game/GameActivity", "getFilesDir", "()Ljava/lang/String;",
                      get_files_dir);
  return 0;
}
```

`call->self` is the object (or the class, for a static method), and `call->args[]` holds the arguments, already unpacked from the variadic, `va_list` or `jvalue[]` form native code used. `fake_jni.h` lists the helpers: strings, object arrays, instances and pending exceptions.

**Calling into the game.** Look up natives with `dl_native(main, "com/example/game/GameLib", "onDrawFrame")`. It finds both natives registered with `RegisterNatives` from `JNI_OnLoad` (droidlift calls `JNI_OnLoad` after each library's constructors) and exported `Java_com_example_game_GameLib_onDrawFrame` symbols. Call one with `cpu_call(c, fn, nargs, args)`:
- The first two arguments are `jni_env()` and the object or class (`jni_new_instance` / `jni_find_class`).
- ARM's soft-float convention applies: floats go in core registers as their bit patterns, and 64-bit values (`double`, `long`) are 8-byte aligned in register pairs or on the stack.

## 4. Fill in the hooks

| Hook | Android equivalent | Typical body |
|---|---|---|
| `init(data_dir)` | the Java side existing | `jni_register_method` for everything native code calls |
| `start` | `Activity.onCreate` | the game's init native |
| `surface(w, h)` | `onSurfaceCreated` + `onSurfaceChanged` | the game's GL init and resize natives |
| `frame` | `onDrawFrame` | `input_dispatch(c, m)`, then the render native |
| `touch` | `onTouchEvent` | the game's touch native with id, x, y (and time) |
| `pause`, `stop`, `restart`, `resume` | the activity lifecycle | the game's lifecycle natives. Most games save in `onPause` or `onStop`, and droidlift calls `stop` at quit and when iOS backgrounds the app |

`max_aspect_w`/`max_aspect_h` adds black bars when the screen is wider than the game was made for. Many Android games assume 16:9 at most.

Check progress headless, then in a window:

```sh
build/my-game --apk original/game.apk --init-test --frames 600 --screenshot build/shot.png
build/my-game --apk original/game.apk --init-test --window
```

A guest fault is logged as `[cpu] guest call ... faulted at pc ...`. Subtract the base address printed for each library at load time to get an offset you can look up in a disassembler. `DL_TRACE_IMPORTS=1` logs every shim call with its arguments, and `DL_TRACE_FILES=1` logs every file the game opens.

## 5. Make it fast: the recompiler

Once the game runs on dynarmic, list its libraries for ahead-of-time recompilation and reconfigure:

```cmake
RECOMP "libgame:game:32" "libfmodex:fmodex:8"   # library:C prefix:shards
```

```sh
cmake -S . -B build -DDL_RECOMP=ON && cmake --build build
build/my-game --apk original/game.apk --cpu recomp --init-test --window
```

At exit, `--cpu recomp` prints the guest addresses that fell back to the interpreter, with how often and for how long. A few cold addresses are normal. A hot one usually means the recompiler missed a function, often an indirect jump target or a jump-table pattern it doesn't know. [recompiler.md](recompiler.md) explains discovery and how to add patterns. `DL_RECOMP_MAX_FALLBACK=N` fails a run with more fallback entries than N, which is useful in tests.

Two differential tests catch recompiler bugs before the game does. Each runs recompiled functions and dynarmic from the same random states and compares the results:

```cmake
droidlift_add_recomp_sampletest(my-game recomp_sampletest)        # random functions + callees
droidlift_add_recomp_difftest(my-game recomp_difftest LIB libgame PREFIX gamet FUNCS some_leaf_fn)
```

## 6. Fix the game's own bugs, if any

Sometimes the game misbehaves only because something differs from a phone, such as a locale, a timezone or a file listing order. Fix the shim if the difference is droidlift's. If the game itself is at fault, write a load-time patch in a `PATCHES` file (`game_patches.h`). A patch lists the exact bytes it expects at an address plus a guard on the code that uses them. It is applied in guest memory, never to the APK, and is skipped if another build of the game has different bytes. The recompiler applies the same table, so recompiled code matches.

## 7. The iPhone

See [ios.md](ios.md). In short:

```sh
droidlift/scripts/build-ios-sim.sh && droidlift/scripts/run-ios-sim.sh   # no Apple ID needed
droidlift/scripts/build-ios-device.sh --install                          # free Apple ID
```

The device build defaults to `recomp`, because a stock iPhone allows no JIT.

## 8. Tests

The loader's flags are made for `ctest`: each prints a fixed `... OK` line on success. A useful set for any port:

```cmake
add_test(NAME m1 COMMAND my-game --apk ${DL_APK} --work ${CMAKE_BINARY_DIR}/apk --check)
set_tests_properties(m1 PROPERTIES PASS_REGULAR_EXPRESSION "M1 OK")
add_test(NAME cpu COMMAND my-game --apk ${DL_APK} --work ${CMAKE_BINARY_DIR}/apk --cpu-test)
set_tests_properties(cpu PROPERTIES PASS_REGULAR_EXPRESSION "CPU OK")
add_test(NAME frames COMMAND my-game --apk ${DL_APK} --work ${CMAKE_BINARY_DIR}/apk
         --data ${CMAKE_BINARY_DIR}/userdata --init-test --frames 600 --screenshot ${CMAKE_BINARY_DIR}/f.png)
set_tests_properties(frames PROPERTIES PASS_REGULAR_EXPRESSION "FRAMES OK" ENVIRONMENT "DL_AUDIO=off")
```

To drive menus in tests, use `--touch`. It plays scripted taps at frame numbers or after the game's own signals (`input_signal("name")` from your Java-side code, for example when the game logs an analytics event). [input.h](../core/src/input.h) has the grammar.
