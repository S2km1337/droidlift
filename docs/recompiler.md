# The recompiler

`droidlift-recomp` turns an armeabi-v7a ELF shared library into C, ahead of time. clang compiles that C for arm64, and the result runs on an iPhone with no JIT and no executable memory. The style is that of N64Recomp and XenonRecomp, not LLVM lifting: each guest instruction becomes a few lines of C on a guest-state struct and guest memory.

```sh
droidlift-recomp --elf libgame.so --census                                  # coverage only
droidlift-recomp --elf libgame.so --out DIR --prefix game --all --shards 32 # what droidlift_add_game runs
droidlift-recomp --elf libgame.so --out DIR --prefix t --func NAME...       # chosen functions (difftest)
droidlift-recomp --elf libgame.so --out DIR --prefix s --sample N --seed S  # random sample (sampletest)
```

## Decoding

The decoder is table-driven (`src/decode.cpp`). Each row is a 32-character bit pattern copied from the ARM manual's encoding diagram, such as `cccc0000101Shhhhllllmmmm1001nnnn` for UMLAL. Letters name fields, and bits with the same letter are concatenated. The first matching row wins.

The ARM, Thumb-2 and VFP/NEON tables all fill one normalized `Insn`: registers, the final immediate, the operand-2 form and the absolute branch target. The emitter (`src/emit.cpp`) therefore never sees encodings and has one case per kind. Adding an instruction means adding a row and, if needed, an emitter case.

droidlift doesn't use Capstone. The recompiler needs encoding-level facts that a disassembler's operand view blurs or drops: `LSR #0` meaning `#32`, `ROR #0` meaning RRX, UNPREDICTABLE register combinations, and the shifter carry-out.

## Discovery

Function starts come from:

- sized symbols, `.ARM.exidx` and PLT stubs;
- relocated pointers in data (vtables, `init_array`);
- the call graph: BL/BLX targets, constant tail branches, pc-relative addresses computed in code, and the `mov lr, pc; ldr pc, [...]` call idiom.

A range ends at the symbol's size or at the next start. The instruction set comes from symbol bit 0, from pointers and from the reaching BL/BLX. Starts that nothing reaches are decoded both ways, and the decoding with fewer invalid encodings wins.

These patterns are known and handled: GCC's position-independent jump tables (`adr; ldr; add; bx`), Thumb/ARM interworking veneers, and code pointers whose constants are loaded before a call. **Code built by other compilers may use patterns discovery does not know yet.** `--census` reports coverage, and a game run with `--cpu recomp` reports what fell back to the interpreter.

## Generated code

- **Guest state.** `rc_state` (`runtime/rc_runtime.h`) holds `r[16]`, `cpsr`, `ext[64]` and `fpscr`, which is what the CPU-backend interface exposes. Inside a function, registers and NZCV live in C locals so clang can keep them in host registers.
- **Memory.** Guest addresses are offsets into the 4 GiB guest reservation. The library's load address is a runtime variable (`<prefix>_base`).
- **Control flow.** Branches inside a function become `goto`. Direct calls between recompiled functions are C calls. Anything else leaves to `rc_run()`, which looks the target up in the module's sorted address table and falls back to the interpreter on a miss.
- **Calls and unwinding.** Every call's return site is an entry, and calls are checked against SP. If a `longjmp` or C++ exception unwinds past a frame, `rc_call` returns "unwound" and the C function leaves for wherever the pc now is. Host frames are never `longjmp`ed over.
- **Untranslatable instructions.** An instruction with no emitter case exits to the interpreter at its address, and the next instruction becomes a resume entry. Nothing fails at build time.
- **Floating point.** VFP add, sub, mul and div are one arm64 instruction each (inline asm), so NaN sign and payload match ARM. The code builds with `-ffp-contract=off` (multiply-accumulate rounds twice, as on ARM) and `-fno-math-errno`. VFP code assumes FPSCR keeps Android's default rounding and flush modes, and cumulative exception flags aren't tracked.

## The backend at run time

`--cpu recomp` (`core/src/cpu_recomp.c`) makes a cpu an `rc_state` plus a halt flag. The cpu dispatches through `rc_lookup`, which has a per-thread cache, until the return sentinel is reached. The loader's import and JNI thunks are registered with the dispatcher, so shim calls run natively without the interpreter. A shim that halts the cpu deep inside nested recompiled frames unwinds by moving SP above every guest stack instead of using `longjmp`.

Every interpreter fallback is counted by guest address. At exit the top 25 are printed with their module offset and nearest symbol.

- `DL_RECOMP_REPORT=path` writes all of them.
- `DL_RECOMP_TRACE=1` logs each address when it is first entered.
- `DL_RECOMP_MAX_FALLBACK=N` makes a run with more than N fallback entries fail, for tests.

## Testing

dynarmic is the reference CPU.

- `droidlift_add_recomp_difftest` runs chosen functions on random registers, flags and buffers.
- `droidlift_add_recomp_sampletest` runs a stratified random sample (Thumb, ARM, VFP, NEON) of each library, with call closures, on relocated images with pointer-rich random memory.

Registers, flags, extension registers, FPSCR and touched memory must match. Inputs that leave the sample are skipped, not compared.
