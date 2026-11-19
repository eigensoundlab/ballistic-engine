# Instructions for coding agents

This file is for AI coding agents and for humans who want the short version. Read it before changing anything.

## What this is

Ballistic Engine is the sound engine of [Ballistic Foley Lab](https://github.com/eigensoundlab/ballistic) in C, for
games and tools that render its sounds at runtime. The JavaScript engine (`reference/engine.js`, a pinned copy of
Ballistic's `src/engine.js`) is the reference: this engine renders the same sounds, sample for sample within rounding.

## Commands

| Task | Command |
|---|---|
| Build | `cmake -S . -B build && cmake --build build` |
| C tests | `ctest --test-dir build` |
| Against the JavaScript engine | `node tests/conformance.mjs` (add `--only ID --verbose` for one preset) |
| Python binding | `python -m unittest tests.test_python` |
| Tables | `node tools/gen_tables.mjs` (after updating `reference/engine.js`); `--check` in CI |

## Rules

1. **The JavaScript is the reference.** A change to the sound is made in Ballistic first, then ported. Port line for
   line: the same order of operations (write `a * b / c` as the JavaScript does), the same random draws in the same
   order (one `rnd()` per statement: C does not fix the order of operands), doubles where it computes and floats where
   it stores a `Float32Array`, `js_round` for `Math.round`, `js_hypot` for `Math.hypot`.
2. **Every ported kind conforms.** `tests/conformance.mjs` must pass with its tolerance unchanged; add a kind's
   presets to the supported list only when they do. Kinds not ported answer `BE_ERR_KIND`.
3. **Data comes from the reference.** Parameters, presets, materials and rooms are generated into `src/tables.h`;
   never edit it by hand.
4. **Portable C99, no dependencies,** no global mutable state (the engine is thread-safe), no `-ffast-math`, and
   `-ffp-contract=off` (MSVC `/fp:precise`).
5. **The public API is `include/ballistic.h`.** Bindings (`bindings/`) follow it; a change there changes them too.
6. **Every change has a test** in `tests/`.
7. **Licence:** Apache-2.0. Every source file starts with an SPDX line.
8. **Commits:** author `eigensoundlab <336850930+eigensoundlab@users.noreply.github.com>`. No AI attribution in commit
   messages or pull request descriptions: no `Co-Authored-By` trailers, no session links, no "generated with" lines.

## Porting the next kind

Read the kind's `plan...` and `...Layers` functions in `reference/engine.js` and what they call, port them into
`src/engine.c` next to `explosion_layers`, add the kind to `kind_of()` and `plan_rep()` (with its jitter and pan) and
to `mix_scene()`, then run the conformance with `--only` for each of its presets until they pass.
