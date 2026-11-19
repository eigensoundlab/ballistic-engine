// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Daniel Sandner
/* The C engine against the JavaScript one (reference/engine.js): every preset of every kind the C engine renders,
   several seeds, the physics and Ballistic's game finishing, sample for sample.

     node tests/conformance.mjs [path/to/ballistic-render] [--verbose] [--only ID] [--seeds 1,7]

   A sound conforms when it has the same length and events and no sample differs by more than TOLERANCE (the
   sounds peak at 1). The engines compute in the same order, so differences are rounding in the maths libraries
   (V8's and the C library's exp, pow, sin...), far below anything audible. Exits 1 on any difference. */
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const args = process.argv.slice(2);
const bin = args.find((a) => !a.startsWith('--') && !['--only', '--seeds'].includes(args[args.indexOf(a) - 1])) ||
  path.join(ROOT, 'build', process.platform === 'win32' ? 'ballistic-render.exe' : 'ballistic-render');
const verbose = args.includes('--verbose');
const only = args.includes('--only') ? args[args.indexOf('--only') + 1] : null;
const TOLERANCE = 1e-4;
const SEEDS = args.includes('--seeds') ? args[args.indexOf('--seeds') + 1].split(',').map(Number) : [1, 2, 7, 1234567];
const ACTIONS = [null, 'game'];
const EXTRA = [                                   // the parameters games change most
  ['pistol', { D: 0, target: 'none', env: 'indoor' }],
  ['ear', { v0: 400, cal: 18.5, mass: 0.032, lenRatio: 1, env: 'indoor' }],
  ['burst', { D: 0, miss: 0.6, env: 'indoor', seqCount: 2, seqInterval: 0.08, trimLaunch: 0 }],
  ['ricochet', { D: 2, miss: 3, env: 'indoor' }],
  ['charge', { exW: 0.3, exDist: 8, exSurface: 'concrete', env: 'indoor' }],
  ['rifle', { target: 'steel', targetDist: 320, payload: 0.2, env: 'urban' }],
  ['pew', { seqCount: 3, seqJitter: 0.3, env: 'indoor' }],
  ['plasmabolt', { D: 6, miss: 1.5, targetDist: 12, env: 'indoor' }],
  ['plasmabolt', { D: 3, miss: 0.8, bvTone: 1100, bvSizzle: 0.9, targetDist: 0, tgMat: 'none', env: 'indoor' }],
  ['nailgun', { lnType: 'none', bvSpeed: 12, bvCal: 45, tgThick: 80, tgSize: 0.8, tgEnergy: 40, targetDist: 0.05 }],
  ['railgun', { tgMat: 'aluminium', D: 50, seqCount: 2 }],
  ['tesla', { seqCount: 2, seqJitter: 0.3 }],
  ['spark', { dcEnergy: 2, dcLength: 0.05, dcDist: 3, dcCharge: 0.9, dcChargeHz: 6000, dcSustain: 0.2 }],
  ['tube', { imDist: 3, imGroan: 0, seqCount: 2 }],
  ['crate', { dsMass: 3, dsHeight: 0.3, dsPieces: 6, seqCount: 2 }],
  ['shielddown', { seqCount: 2, ffDist: 2 }],
  ['plasma', { ffDist: 5, env: 'urban' }],
];

const ctx = {};
vm.runInNewContext(fs.readFileSync(path.join(ROOT, 'reference', 'engine.js'), 'utf8'), ctx);
const B = ctx.BFX;
const supported = new Set(execFileSync(bin, ['--list'], { encoding: 'utf8' }).split('\n')
  .filter((l) => l.trim() && !l.includes('   (')).map((l) => l.split(/\s+/)[0]));

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'be-conf-'));
let bad = 0, cases = 0, worst = 0;
const jobs = [];
for (const pr of B.PRESETS) if (supported.has(pr.id)) jobs.push([pr.id, {}]);
for (const e of EXTRA) jobs.push(e);

for (const [id, over] of jobs) {
  if (only && id !== only) continue;
  const p = B.merge(B.merge(B.DEFAULTS, B.PRESETS.find((x) => x.id === id).p), over);
  for (const seed of SEEDS) {
    for (const action of ACTIONS) {
      cases++;
      let r = B.renderRaw(p, seed);
      if (action) r = B.actionize(r, Object.assign({}, B.ACTION_DEFAULTS, B.ACTION_PRESETS.find((x) => x.id === action).a));
      const out = path.join(tmp, 'c.raw');
      const cargs = ['--preset', id, '--seed', String(seed), '--raw', '--events', '-o', out];
      for (const [k, v] of Object.entries(over)) cargs.push('--set', `${k}=${v}`);
      if (action) cargs.push('--action', action);
      const evText = execFileSync(bin, cargs, { encoding: 'utf8' });
      const buf = fs.readFileSync(out), c = new Float32Array(buf.buffer, buf.byteOffset, buf.length / 4);
      const n = c.length / 2;
      const name = `${id}${Object.keys(over).length ? '+' : ''} seed ${seed}${action ? ' ' + action : ''}`;
      const problems = [];
      if (n !== r.L.length) problems.push(`length ${n} in C, ${r.L.length} in JS`);
      let md = 0, at = -1;
      for (let i = 0; i < Math.min(n, r.L.length); i++) {
        const d = Math.max(Math.abs(c[2 * i] - r.L[i]), Math.abs(c[2 * i + 1] - r.R[i]));
        if (!(d <= md)) { md = d; at = i; }
      }
      if (!(md <= TOLERANCE)) problems.push(`samples differ by up to ${md.toExponential(2)} (at ${(at / B.FS).toFixed(4)} s)`);
      const evC = evText.trim() ? evText.trim().split('\n').map((l) => +l.split(' ')[1]) : [];
      const evJ = (r.events || []).map((e) => e.t);
      if (evC.length !== evJ.length || evC.some((t, i) => Math.abs(t - evJ[i]) > 1e-6)) {
        problems.push(`events ${evC.length} in C, ${evJ.length} in JS`);
      }
      if (md > worst && n === r.L.length) worst = md;
      if (problems.length) { bad++; console.log(`DIFF ${name}: ${problems.join('; ')}`); }
      else if (verbose) console.log(`ok   ${name}: ${n} samples, max difference ${md.toExponential(2)}`);
    }
  }
}
fs.rmSync(tmp, { recursive: true, force: true });
console.log(`${cases - bad} of ${cases} sounds conform (presets: ${[...supported].length}; largest difference ${worst.toExponential(2)})`);
process.exit(bad ? 1 : 0);
