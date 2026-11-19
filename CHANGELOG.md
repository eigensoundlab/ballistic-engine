# Changelog

## 0.4.0 - 2026-11-19

- **Jet and rocket**: thrust, mass and drag along the path; jet mixing noise with its Strouhal peak, supersonic
  crackle, chug, turbine whine, ground effect; ignition and cut-off; launch, fly-by, landing with a braking burn
  that reaches touchdown speed at the pad, and a static burn as a seamless loop. Payloads and impacts. Presets
  launcher, incoming, salvo, booster, rlanding, jet, thruster.
- **Rotor**: rotor speed from weight, size and blade count; blade-pass tones, vortex noise, rotors beating against
  each other, blade slap, motor whine; hover as a seamless loop, fly-by, take-off, landing. Presets quad, quadpass,
  swarm, heavylift, quadland, heli, helihover.
- Every kind Ballistic models is ported: 73 of its 83 presets render; the other 10 need a recording.

## 0.3.0 - 2026-11-19

- **Discharge**: thunder from a crooked channel split into stretches (Few), branches, return strokes, sustained arcs
  with plasma hiss, charger whine, coil kick and LC ring. Presets arcgun, coilgun, tesla, stun, spark, lightning,
  closestrike.
- **Implosion**: Rayleigh-Plesset collapse of a cavity in air or water, the shell's groan before it fails, its
  fracture and the pieces falling. Presets tube, vactank, bubble, hull.
- **Destruction**: pieces by a power law, free fall and bounces by restitution, cracks, stick-slip creaks and groans
  before the break, fine debris, concrete rubble, gravel grains. Presets window, crate, wall, wallgravel, gantry
  (potshatter, potdrop and knockglass need a recording).
- **Forcefield**: a driven sheet of modes, or a reduced plasma model - shear, fast and slow waves, reconnection
  crashes, chorus, solitons, kink disruption - with impact, power-up and collapse. Hums render as seamless 4 s loops,
  their filters and room wrapped round the seam: `be_is_loop()`, and `be_finish_loop()` finishes a loop as a loop.
  Presets shield, shieldhit, shieldup, shielddown, reactor, barrier, plasma, plasmahit, kink, tokamak, chorus.
- 59 of Ballistic's 83 presets render; rockets and rotors are next. Conformance: 608 sounds, largest difference 4e-5.
- Releases: GitHub Actions builds the library for Linux (x64, arm64), macOS (universal), Windows (x64, arm64) and
  Android (three ABIs) on every version tag, with a Unity package.
- Python binding: `Sound.loop`; C#: `BallisticSound.Loop`.

## 0.2.0 - 2026-11-19

- **Blaster**: the weapon chain. Launchers: a struck dispersive line with echoes, thump and body (can, tube, plate),
  a rail gun (charge whine, capacitor dump, plasma flash, both rails ringing with bending waves), a coil gun (stages
  firing closer together), a gas nail gun (valve, Helmholtz chamber, quarter-wave barrel, dispersive return spring).
  Bolts: plasma bolts and beams heating a channel of air (N-waves per stretch, the whump of the core), pulsed beams,
  the bolt's own voice with Doppler, slugs with crack, wake and plasma sheath. Targets: ablation, plates of steel,
  aluminium, glass, ice and wood ringing with bending waves, concrete, water with splash and collapsing steam
  bubbles, spatter, and the hot spot sizzling and ticking as it cools. Presets pew, pewecho, pulse, cannon, wire,
  plasmabolt, plasmarifle, laser, pulselaser, railgun, raildown, coilslug, nailgun, thundersheet, icesheet,
  boltwater (knockfire needs a recording).
- `be_check()` and `be_preset_supported()`: what a render would answer, without rendering. `--list` marks presets
  that need a recording; the bindings use it.
- The blaster's tables (bodies, plates, ablation) and Ballistic's 8192-point sine table are generated from the
  reference too.
- Conformance: 344 sounds, largest difference 2e-5. Blasters render 5 to 8 times faster than on V8.

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
