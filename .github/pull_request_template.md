## What and why

<!-- One or two sentences. Link the issue or porting report if there is one. -->

## How it was tested

<!-- Which self-tests or ports, on which backends (dynarmic / interp / recomp) and platforms. -->

- [ ] Builds with `-Wall -Wextra -Werror`
- [ ] The relevant self-test passes (`--cpu-test`, `--interp-test`, `--fmt-test`, `--jni-test`, ...)
- [ ] Nothing game-specific in `core/` (game behaviour belongs in a profile)
- [ ] No game data in the diff, the description or attachments
