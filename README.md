# Ballistic Engine

The sound engine of [Ballistic Foley Lab](https://github.com/eigensoundlab/ballistic) in portable C, for games and
tools that render sounds at runtime: shots, ricochets, blasts and impacts computed from physics, a new take for every
seed. It renders what Ballistic renders, sample for sample: the JavaScript engine is the reference, and every preset
is checked against it on every change.

- **C99, no dependencies** beyond the C library and libm. One header, two source files.
- **Thread-safe**, no global state: render a pool of takes on worker threads while a level loads.
- **5 to 14 times faster** than the JavaScript engine on V8 (below).
- **Bindings**: Python (ctypes, `bindings/python`), C# for Unity and .NET (`bindings/csharp`).
- **Apache-2.0**, as Ballistic.

## What it renders so far

| Kind | Presets | Status |
| --- | --- | --- |
| Projectile: muzzle blast, N-wave crack, wake with Doppler, ricochet tumble, impacts on steel, wood, dirt, concrete, glass; explosive payloads | rifle, ear, burst, pistol, sniper, tank, mortar, arrow, ricochet, swing, flyover | ported, conforms |
| Explosion: blast, rumble, ground shock, fragments, debris (modal or gravel grains) | charge, fragstorm, bigblast, gravelblast, barrage | ported, conforms |
| Blaster, discharge, field, implosion, destruction, rocket, rotor | 56 presets | next: `be_render` answers `BE_ERR_KIND` |
| Recording, recorded rooms, events triggered by a recording | 6 presets | needs your recordings; not planned for the engine |

Ballistic's finishing for action (`"game"`, `"block"`, `"slow"`, `"far"`: punch, weight, drive, bite, tail, swell,
width, loudness) is ported too: `be_finish`.

`ballistic-render --list` shows every preset and whether this version renders it.

## Conformance

`tests/conformance.mjs` renders every supported preset, plus the variations games use most, with four seeds, as
physics and with the game finishing, in both engines, and compares every sample: 176 sounds, the largest difference
2e-5 (the sounds peak at 1), from rounding in the maths libraries. Lengths and event times match exactly. The port
keeps the JavaScript's order of operations, random draws, `Math.round` and `Math.hypot`, and builds with
`-ffp-contract=off`, so that fused multiply-adds do not change the rounding on ARM.

## Speed

One take, rendered on one core of a small cloud machine (2 cores, Node 22 for the JavaScript):

| Preset | JavaScript (V8) | C | |
| --- | ---: | ---: | ---: |
| rifle | 67.2 ms | 6.6 ms | 10x |
| ear (rifle at the shooter) | 14.1 ms | 2.9 ms | 5x |
| burst | 108.0 ms | 10.2 ms | 11x |
| pistol | 35.6 ms | 6.5 ms | 5x |
| sniper | 111.4 ms | 18.7 ms | 6x |
| ricochet | 393.0 ms | 34.0 ms | 12x |
| swing | 22.0 ms | 3.2 ms | 7x |
| charge (explosion) | 605.2 ms | 42.8 ms | 14x |
| fragstorm | 1312.1 ms | 129.3 ms | 10x |
| gravelblast | 264.1 ms | 30.2 ms | 9x |

Most of a long sound's time is its room (the reverb runs over the whole tail); `env: "none"` or `"indoor"` is cheaper
than `"valley"`. A game renders a handful of takes per sound when it loads and plays them in turn, rather than
rendering on the shot.

## Build

```bash
cmake -S . -B build && cmake --build build      # libballistic (shared), libballistic_static, ballistic-render
ctest --test-dir build                          # the C tests
node tests/conformance.mjs                      # against the JavaScript engine (Node.js)
python -m unittest tests.test_python            # the Python binding
```

Without CMake: `cc -std=c99 -O2 -ffp-contract=off -Iinclude -Isrc src/*.c cli/ballistic-render.c -lm`.
Keep `-ffp-contract=off` (MSVC: `/fp:precise`) and never `-ffast-math`.

## Use

C:

```c
#include "ballistic.h"

be_params *p = be_params_new();
be_params_preset(p, "pistol");
be_params_set(p, "D", 0);                     /* metres to the listener: 0 is the shooter */
be_params_set_text(p, "env", "indoor");
be_sound s;
if (be_render(p, 7, &s) == BE_OK) {           /* seed 7; each seed is another take */
    be_finish(&s, "game", 0, 0, 0);           /* optional */
    be_cut(&s, 1.2, 0.04);
    /* s.L, s.R: s.n float samples at 48 kHz; s.events: launch, crack, pass, impact */
    be_sound_free(&s);
}
be_params_free(p);
```

Command line:

```bash
ballistic-render --preset ricochet --set D=2 --set env=indoor --action game --seed 3 --length 1.2 -o ric.wav
```

Python:

```python
import ballistic_engine as be                 # bindings/python, finds build/libballistic
s = be.render("charge", seed=2, exW=0.3, exDist=8, exSurface="concrete", env="indoor", action="game")
s.cut(3).write_wav("blast.wav")
```

Unity and .NET: [bindings/csharp](bindings/csharp/README.md).

The parameters are Ballistic's own (`ballistic-render --keys` lists them; their meaning is in Ballistic's page and
`reference/engine.js`, `DEFAULTS`). The ones games change most: `D` (distance to the listener, m), `miss` (how far
the path passes the listener, m), `env` (`none`, `field`, `forest`, `urban`, `valley`, `indoor`, `water`), `target`
(`none`, `steel`, `wood`, `dirt`, `concrete`, `glass`) and `targetDist`, `v0`, `cal`, `mass` for the round, `exW` (kg
TNT) and `exDist` for blasts, `seqCount` and `seqInterval` for bursts, and `trimCrack`, `trimLaunch`, `trimWake`,
`trimImpact` (dB) to balance the layers.

## Layout

```
include/ballistic.h     the API
src/engine.c            the DSP, a port of reference/engine.js
src/params.c            parameters by name, presets, lists
src/tables.h            generated from reference/engine.js: parameters, presets, materials, rooms
reference/engine.js     Ballistic's engine, the reference (version 0.1.0, 45a3f91)
tools/gen_tables.mjs    writes src/tables.h
cli/ballistic-render.c  the command-line renderer
bindings/               Python, C#
tests/                  C tests, conformance, binding tests
```

## Licence

Apache-2.0. Copyright 2026 Daniel Sandner. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
