# Contributing to droidlift

Thanks for helping. droidlift becomes more general one game at a time, so the most useful contributions come from trying it on a new game: a missing shim, an instruction the interpreter lacks, a pattern the recompiler doesn't discover, or a porting report.

## Ground rules

- **No game data, ever.** Don't commit, attach or paste game binaries, extracted assets, decompiled game code or recompiled output, and don't link to places that host APKs. Describe problems with offsets, symbol names and short disassembly snippets instead.
- **Keep the toolkit game-neutral.** Anything specific to one game belongs in that game's profile, not in `core/`. If a game needs a behaviour other games won't share, add a hook or an option.
- **Say what you verified.** A pull request says how it was tested, and on which backends.

## Setting up

droidlift is built through a port (see [the template](examples/template)). To work on droidlift itself, use a port as a harness, with the droidlift submodule pointing at your fork or a local checkout:

```sh
cp -R examples/template ../my-port && cd ../my-port
ln -s ../droidlift droidlift            # or a submodule of your fork
droidlift/scripts/fetch-deps.sh && droidlift/scripts/fetch-angle.sh
cmake -S . -B build -G Ninja -DDL_RECOMP=OFF && cmake --build build
```

The CI workflow (`.github/workflows/ci.yml`) builds the template the same way, without a game.

## Common contributions

### A missing import

`--check` names it. Add a `STUB("libc.so", "name")` entry to the table in `core/src/dynlib.c` (sorted within each library) and a host implementation to the matching `reimpl/*.c` table:

```c
static void fn_strnlen(cpu *c) {
  const char *s = g2h(cpu_arg(c, 0));
  cpu_ret(c, (uint32_t)strnlen(s, cpu_arg(c, 1)));
}
```

Mind bionic's 32-bit ABI:
- **Struct layouts** differ from the host's; convert them field by field.
- **errno values** differ; translate them.
- **Floats and doubles** arrive in core registers (softfp).
- **Guest pointers** must go through `g2h`.

If the function can be compared with the host's, add a case to `cpu_selftest.c` or `fmt_selftest.c`.

### An instruction or a discovery pattern

- **Interpreter** (`core/src/interp/`): `--interp-test` compares it with dynarmic on random instructions of each class, and in lockstep on real functions. Add the encoding, then run `--interp-test` with a seed (`DL_INTERP_SEED`) that exercises it.
- **Recompiler** (`recomp/src/`): add the decoder row and emitter case ([docs/recompiler.md](docs/recompiler.md)), then add the address as a regression case with `droidlift_add_recomp_sampletest(... ALSO <lib> <addr>)` in your port's tests.

### A porting report

Open an issue with the "Porting report" template. Say how far the game got, which imports were missing, which Java methods it called, and which backends work. Reports on games that *didn't* work are just as useful: they show where droidlift needs to grow.

## Style

- **C11** for the core, **C++20** for the recompiler. Everything builds with `-Wall -Wextra -Werror`.
- **Comments** explain *why* in plain sentences, and each file starts with what it is for. Name the Android behaviour being reproduced when that is the reason.
- **Environment variables** for debugging start with `DL_`.
- **Commits** are small and have imperative subjects ("Add strnlen", "Discover jump tables built with tbb").

## Licence

Contributions are accepted under the [MIT licence](LICENSE).
