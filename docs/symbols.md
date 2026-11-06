# Symbolization

## What it does

`host/symbols.py` answers "which function, file and line does this address belong to?" for a sampled `mepc`. It reads both
from the firmware ELF with `pyelftools`.

## Why this design

- **Two sources.** The symbol table gives function names and sizes. DWARF gives file and line. Both exist in a `-g` build.
- **Sorted table plus binary search.** Function ranges don't change, so sorting once makes each lookup fast. Repeat
  addresses are cached, because a tight loop produces the same few addresses again and again.
- **Sizes are checked.** An address past the end of the nearest function is `(unknown)`, not wrongly given to it.
- **Assembly needed help.** Functions in `.S` files only appear with `.type` and `.size` directives, so those were added.

## How it is tested

Against the real firmware ELF, including a comparison with binutils' `addr2line` on more than 500 addresses. That is a
separate implementation, so agreement means more than a test written from the same assumptions.

## What could go wrong

- **Inlining.** The compiler merges small functions, so a hot static function can appear under its caller's name. DWARF's
  line still points at the inlined source, which can look inconsistent.
- **Wrong ELF.** Symbolizing with a different build gives confident wrong answers. The collector records the ELF's SHA-256
  with each run so this can be checked later. It does not detect a mismatch by itself.
- **Sampling bias.** `mepc` is where the timer happened to land. It estimates where time went, it does not measure it.
