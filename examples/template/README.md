# Template port

A starting point for porting a game with droidlift. Copy this folder to a new repository, add droidlift as a submodule, and follow [the porting guide](../../docs/porting.md).

| File | What to change |
|---|---|
| `game/profile.c` | `libs` (System.loadLibrary order), `jni_classes`, then the hooks one at a time |
| `CMakeLists.txt` | `RECOMP` once the game runs, `APP_NAME`, `ICON` and `ORIENTATION` for iOS |
| `droidlift.env` | `DL_APP_NAME` (same as `APP_NAME`) and `DL_APK` |
| `.gitignore` | Keeps the APK and everything derived from it out of git. Leave it as it is |

The CI workflow builds this template without a game, so it always compiles against the current droidlift.
