/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Daniel Sandner */
#ifndef BE_INTERNAL_H
#define BE_INTERNAL_H

#include "ballistic.h"

#define BE_VERSION "0.3.0"
#define BE_REFERENCE "Ballistic Foley Lab engine.js 0.1.0 (45a3f91)"
#define BE_TEXT_MAX 32

enum { BE_NUMBER = 0, BE_FLAG = 1, BE_TEXT = 2 };

typedef struct { const char *name; int type; double num; const char *text; } be_key;
typedef struct { int key; double num; const char *text; } be_kv;
typedef struct { const char *id, *name; const be_kv *kv; int n; } be_preset;
typedef struct { double dur, hp, lp, gain; } be_noise_spec;
typedef struct { int count; double spread; int ping; double f[2]; double gain; } be_grain_spec;
typedef struct {
    const char *id;
    double f0;
    int n_ratios;
    double ratios[12];
    double t60, slope, contact, eff;
    be_noise_spec noise;
    int has_grains;
    be_grain_spec grains;
} be_material;
typedef struct { double ms, g, pan; } be_tap;
typedef struct {
    const char *id, *name;
    int ir;
    double rt60;
    int n_delays;
    double delays[8];
    double damp, wet;
    int n_taps;
    be_tap taps[3];
} be_env;

typedef struct { const char *id; int nf; double f[8]; double q; } be_body;
typedef struct { const char *id; double E, rho, nu, eta; } be_plate;
typedef struct { const char *id; double frac, hiss; const char *spatter; } be_ablation;
typedef struct { const char *id; double e, brittle; const char *groan; int fine, rubble; double shell_density; } be_break;
typedef struct { const char *id; int n; double r[24]; } be_field;

#include "tables.h"

struct be_params {
    double v[K_COUNT];
    char s[K_COUNT][BE_TEXT_MAX];
};

#define PV(p, k) ((p)->v[K_##k])
#define PS(p, k) ((p)->s[K_##k])

const be_material *be_material_find(const char *id);
const be_env *be_env_find(const char *id);

#endif
