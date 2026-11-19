# Changelog

## 0.1.0 - 2026-11-19

The first release: Ballistic Foley Lab's engine (0.1.0, 45a3f91) in C, for the sounds games fire most.

- **Projectile**: drag-based flight, Mach cone timing, N-wave crack (Whitham), muzzle blast (Friedlander) with
  propellant jet, wake with Doppler and tumble, ricochet and bowstring launches, impacts on Ballistic's six
  materials, explosive payloads, bursts. Presets rifle, ear, burst, pistol, sniper, tank, mortar, arrow, ricochet,
  swing, flyover.
- **Explosion**: cube-root blast scaling, re-collapse shock, rumble, ground shock ahead of the air blast, fragments
  flying past, debris landing as modal hits or as grains of Ballistic's synthetic gravel. Presets charge, fragstorm,
  bigblast, gravelblast, barrage.
- Rooms: Ballistic's eight-line delay network with its seven environments.
- Ballistic's finishing for action, its four presets and nine parameters.
- Every parameter and preset of Ballistic by name, generated from its engine (`tools/gen_tables.mjs`); kinds not
  ported yet answer `BE_ERR_KIND`.
- `ballistic-render`, a Python binding (ctypes) and a C# binding for Unity and .NET.
- Conformance with the JavaScript engine, sample for sample, on every supported preset: 176 sounds, largest
  difference 2e-5. 5 to 14 times faster than the JavaScript on V8.
