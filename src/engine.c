/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Daniel Sandner */
/* The sound engine of Ballistic Foley Lab, in C.

   A port of reference/engine.js, kept line for line where it can be: the same order of operations, the same
   random draws in the same order, doubles where the JavaScript computes and floats where it stores (Float32Array),
   JavaScript's own Math.round and Math.hypot. tests/conformance.mjs renders every preset both ways and compares.
   Keep it that way: a change here is a change in Ballistic first.

   Kinds so far: projectile (shots, arrows, ricochets, swings, with an explosive payload), explosion, and blaster
   (launcher: dispersive line, rail, coil, gas; bolt, beam or slug; a target that ablates, rings, splashes, sizzles). */
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define FS 48000.0
#define FSI 48000
#define C_AIR 343.0
#define P0 101325.0
#define RHO 1.204
#define TRAJ_HZ 4000.0
#define PI 3.141592653589793
#define TAU (PI * 2)
#define KW 14.0
#define FRAG_BOOST_DB 0.0
#define DEBRIS_BOOST_DB 56.0

/* ------------------------------------------------------------------ JavaScript's arithmetic */
static double js_round(double x)
{
    double r = floor(x);
    return x - r >= 0.5 ? r + 1 : r;
}

/* V8's Math.hypot: normalised to the larger value, Kahan-summed */
static double js_hypot(double a, double b)
{
    double max, sum = 0, comp = 0, n, summand, pre;
    a = fabs(a);
    b = fabs(b);
    if (a != a || b != b)
        return (a == INFINITY || b == INFINITY) ? INFINITY : NAN;
    max = a > b ? a : b;
    if (max == INFINITY)
        return INFINITY;
    if (max == 0)
        return 0;
    n = a / max;
    summand = n * n - comp;
    pre = sum + summand;
    comp = (pre - sum) - summand;
    sum = pre;
    n = b / max;
    summand = n * n - comp;
    pre = sum + summand;
    sum = pre;
    return sqrt(sum) * max;
}

static uint32_t js_uint32(double d)
{
    if (d != d || d == INFINITY || d == -INFINITY)
        return 0;
    d = trunc(d);
    d = fmod(d, 4294967296.0);
    if (d < 0)
        d += 4294967296.0;
    return (uint32_t)d;
}

static double clampd(double x, double a, double b) { return x < a ? a : x > b ? b : x; }
static double maxd(double a, double b) { return a > b ? a : b; }   /* Math.max for numbers that are not NaN */
static double mind(double a, double b) { return a < b ? a : b; }
static double db_to_lin(double d) { return pow(10, d / 20); }
static double trim_gain(double d) { return d <= -40 ? 0 : db_to_lin(d); }

/* ------------------------------------------------------------------ randomness: mulberry32 */
typedef struct { uint32_t a; } rng_t;

static rng_t rng_new(double seed)
{
    rng_t r;
    r.a = js_uint32(seed);
    return r;
}

static double rnd(rng_t *r)
{
    uint32_t a, t;
    r->a += 0x6D2B79F5u;
    a = r->a;
    t = (a ^ (a >> 15)) * (1u | a);
    t = (t + (t ^ (t >> 7)) * (61u | t)) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

/* ------------------------------------------------------------------ memory: everything of one render, freed at once */
typedef struct {
    void **ptrs;
    int n, cap;
    jmp_buf fail;
} arena;

static void *ar_alloc(arena *a, size_t bytes)
{
    void *p;
    if (a->n == a->cap) {
        int cap = a->cap ? a->cap * 2 : 256;
        void **np = (void **)realloc(a->ptrs, cap * sizeof *np);
        if (!np)
            longjmp(a->fail, 1);
        a->ptrs = np;
        a->cap = cap;
    }
    p = calloc(1, bytes ? bytes : 1);
    if (!p)
        longjmp(a->fail, 1);
    a->ptrs[a->n++] = p;
    return p;
}

static void ar_free(arena *a)
{
    int i;
    for (i = 0; i < a->n; i++)
        free(a->ptrs[i]);
    free(a->ptrs);
    a->ptrs = 0;
    a->n = a->cap = 0;
}

static float *fnew(arena *a, int n) { return (float *)ar_alloc(a, (size_t)(n > 0 ? n : 1) * sizeof(float)); }
static double *dnew(arena *a, int n) { return (double *)ar_alloc(a, (size_t)(n > 0 ? n : 1) * sizeof(double)); }

static float *fcopy(arena *a, const float *x, int n)
{
    float *y = fnew(a, n);
    memcpy(y, x, (size_t)n * sizeof(float));
    return y;
}

/* three periods of a loop, end to end */
static float *tile3(arena *ar, const float *x, int n)
{
    float *t = fnew(ar, 3 * n);
    memcpy(t, x, (size_t)n * sizeof(float));
    memcpy(t + n, x, (size_t)n * sizeof(float));
    memcpy(t + 2 * n, x, (size_t)n * sizeof(float));
    return t;
}

/* ------------------------------------------------------------------ buffers and filters */
static double peak_of(const float *L, const float *R, int n)
{
    double p = 0, v;
    int i;
    for (i = 0; i < n; i++) {
        v = L[i] < 0 ? -L[i] : L[i];
        if (v > p)
            p = v;
    }
    if (R)
        for (i = 0; i < n; i++) {
            v = R[i] < 0 ? -R[i] : R[i];
            if (v > p)
                p = v;
        }
    return p;
}

static void scale(float *b, int n, double g)
{
    int i;
    for (i = 0; i < n; i++)
        b[i] = (float)(b[i] * g);
}

static void one_pole_lp(float *b, int n, double fc)
{
    double a = exp(-TAU * fc / FS), s = 0;
    int i;
    for (i = 0; i < n; i++) {
        s = b[i] + (s - b[i]) * a;
        b[i] = (float)s;
    }
}

static void one_pole_hp(float *b, int n, double fc)
{
    double a = exp(-TAU * fc / FS), s = 0;
    int i;
    for (i = 0; i < n; i++) {
        s = b[i] + (s - b[i]) * a;
        b[i] = (float)(b[i] - s);
    }
}

enum { BQ_LP, BQ_HP, BQ_PK, BQ_LS, BQ_HS };

static void biquad(float *b, int n, int type, double f, double q, double g)
{
    double w = TAU * f / FS, cs = cos(w), sn = sin(w), al = sn / (2 * q), A = pow(10, g / 40);
    double b0, b1, b2, a0, a1, a2, sq = 2 * sqrt(A) * al, c0, c1, c2, c3, c4, z1 = 0, z2 = 0, x, y;
    int i;
    if (type == BQ_LP) { b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = b0; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; }
    else if (type == BQ_HP) { b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = b0; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; }
    else if (type == BQ_PK) { b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A; }
    else if (type == BQ_LS) {
        b0 = A * ((A + 1) - (A - 1) * cs + sq); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sq);
        a0 = (A + 1) + (A - 1) * cs + sq; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - sq;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cs + sq); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sq);
        a0 = (A + 1) - (A - 1) * cs + sq; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - sq;
    }
    c0 = b0 / a0; c1 = b1 / a0; c2 = b2 / a0; c3 = a1 / a0; c4 = a2 / a0;
    for (i = 0; i < n; i++) {
        x = b[i];
        y = c0 * x + z1;
        z1 = c1 * x - c3 * y + z2;
        z2 = c2 * x - c4 * y;
        b[i] = (float)y;
    }
}

/* Atmospheric absorption: about 1.5e-9 * f^2 dB per metre. Returns the -3 dB frequency after r metres. */
static double air_fc(double r) { return sqrt(3 / (1.5e-9 * maxd(r, 1))); }

static void air_filter(float *b, int n, double r)
{
    double fc = air_fc(r);
    if (fc >= 20000)
        return;
    one_pole_lp(b, n, fc / 0.64);
    one_pole_lp(b, n, fc / 0.64);
}

/* An analytic pressure shape, integrated over each sample period: A (1 - u) e^(-k u), u = t / T; or, for an
   N-wave, A (1 - 2 t / T). */
typedef struct { int nwave; double A, T, k, mul4; } shape_t;   /* mul4: u = t * mul4 * 4 instead */

static double shape_at(const shape_t *s, double t)
{
    double u;
    if (s->nwave)
        return s->A * (1 - 2 * t / s->T);
    u = s->mul4 ? t * s->mul4 * 4 : t / s->T;
    return s->k == 1 ? s->A * (1 - u) * exp(-u) : s->A * (1 - u) * exp(-s->k * u);
}

static void add_shape(float *b, int len, double t0, double dur, const shape_t *s)
{
    int n0 = (int)maxd(0, floor(t0 * FS)), n1 = (int)mind(len - 1, ceil((t0 + dur) * FS)), OS = 8, n, k;
    for (n = n0; n <= n1; n++) {
        double acc = 0, t;
        for (k = 0; k < OS; k++) {
            t = (n + (k + 0.5) / OS) / FS - t0;
            if (t >= 0 && t <= dur)
                acc += shape_at(s, t);
        }
        b[n] = (float)(b[n] + acc / OS);
    }
}

/* Direct path plus one ground bounce (source 1.5 m, ear 1.7 m), then air absorption. */
static void post_layer(arena *ar, float *b, int len, double r, double ground)
{
    double hs = 1.5, hl = 1.7;
    if (ground > 0) {
        double dDir = js_hypot(r, hs - hl), dRef = js_hypot(r, hs + hl);
        double dl = (dRef - dDir) / C_AIR * FS, g = ground * dDir / dRef, fr;
        float *cp = fcopy(ar, b, len);
        int di, i;
        one_pole_lp(cp, len, 5000);
        di = (int)floor(dl);
        fr = dl - di;
        for (i = len - 1; i > di; i--)
            b[i] = (float)(b[i] + g * (cp[i - di] * (1 - fr) + cp[i - di - 1] * fr));
    }
    air_filter(b, len, r);
}

static void to_peak(float *b, int n, double target)
{
    double pk = peak_of(b, 0, n);
    if (pk > 0)
        scale(b, n, target / pk);
}

static float *shaped_noise(arena *ar, int n, rng_t *r, double lpF, double hpF)
{
    float *b = fnew(ar, n);
    double s = 0;
    int i;
    for (i = 0; i < n; i++) {
        s = s * 0.995 + (rnd(r) * 2 - 1) * 0.1;
        b[i] = (float)s;
    }
    if (lpF) { one_pole_lp(b, n, lpF); one_pole_lp(b, n, lpF); }
    if (hpF) one_pole_hp(b, n, hpF);
    return b;
}

/* ------------------------------------------------------------------ ballistics */
enum { LAUNCH_NONE, LAUNCH_BLAST, LAUNCH_STRING, LAUNCH_RIC_STEEL, LAUNCH_RIC_STONE };

typedef struct {
    double v0, cal, mass, lenRatio, cdScale, tumble, tumbleHz, pathLen, D, miss, targetDist, maxT, vStop;
    int swing, launch;
    const be_material *target;      /* NULL: none */
} proj_t;

typedef struct { int n; double *x, *v, *T; int hit; double xEnd; } traj_t;
typedef struct { double t, x, M, tau, peak, dur; } crack_t;
typedef struct { double t, r, peak, T1, jetTau, jetFc; } pblast_t;
typedef struct { double tau, t, x, v, r, E; } hit_t;
typedef struct {
    traj_t tr;
    int passed;
    double passT, passV, mach;
    int has_crack;
    crack_t crack;
    double r0, launchT, muzzleE;
    int has_blast;
    pblast_t blast;
    int has_hit;
    hit_t hit;
    double flight;
} proj_an;

static double cd(double M)
{
    if (M < 0.8)
        return 0.22;
    if (M < 1.1)
        return 0.22 + (M - 0.8) / 0.3 * 0.26;
    return 0.48 * pow(1.1 / M, 0.55);
}

static void trajectory(arena *ar, const proj_t *p, traj_t *tr)
{
    double dt = 1 / TRAJ_HZ, d = p->cal / 1000, A = PI * d * d / 4;
    double cdMul = (p->cdScale ? p->cdScale : 1) * (1 + 6 * p->tumble);
    int hasTarget = p->target && !(p->pathLen > 0);
    double xEnd = p->pathLen > 0 ? p->pathLen : hasTarget ? p->targetDist : p->D + clampd(40 * p->miss, 30, 1500);
    int maxN = (int)js_round(TRAJ_HZ * (p->maxT ? p->maxT : 8)), i, n = 0, hit = 0;
    double x = 0, v = p->swing ? p->v0 * 0.08 : p->v0, vStop = p->vStop ? p->vStop : 6;
    tr->x = dnew(ar, maxN + 1);
    tr->v = dnew(ar, maxN + 1);
    tr->T = dnew(ar, maxN + 1);
    for (i = 0; i <= maxN; i++) {
        tr->x[i] = x;
        tr->v[i] = v;
        tr->T[i] = i * dt + js_hypot(p->D - x, p->miss) / C_AIR;
        n = i + 1;
        if (x >= xEnd) { hit = hasTarget; break; }
        if (!p->swing && v < vStop)
            break;
        if (p->swing)
            v = p->v0 * maxd(0.08, sin(PI * clampd(x / xEnd, 0, 1)));
        else
            v -= RHO * cd(v / C_AIR) * cdMul * A / (2 * p->mass) * v * v * dt;
        x += v * dt;
    }
    tr->n = n;
    tr->hit = hit;
    tr->xEnd = xEnd;
}

static void analyze_proj(arena *ar, const proj_t *p, proj_an *out)
{
    traj_t *tr = &out->tr;
    int n, i, iMin = 0, iPass;
    double d = p->cal / 1000, r0, E, e3;
    memset(out, 0, sizeof *out);
    trajectory(ar, p, tr);
    n = tr->n;
    iPass = n - 1;
    for (i = 1; i < n; i++)
        if (tr->T[i] < tr->T[iMin])
            iMin = i;
    for (i = 0; i < n; i++)
        if (tr->x[i] >= p->D) { iPass = i; break; }
    out->passed = tr->x[n - 1] >= p->D;
    out->passT = tr->T[iPass];
    out->passV = tr->v[iPass];
    out->mach = tr->v[iPass] / C_AIR;
    if (iMin > 0 && iMin < n - 1 && tr->v[iMin] > C_AIR * 1.01) {
        double M = tr->v[iMin] / C_AIR, len = d * (p->lenRatio ? p->lenRatio : 4), b = maxd(p->miss, 0.2), m2 = M * M - 1;
        out->has_crack = 1;
        out->crack.t = tr->T[iMin];
        out->crack.x = tr->x[iMin];
        out->crack.M = M;
        out->crack.tau = iMin / TRAJ_HZ;
        /* Whitham far-field N-wave */
        out->crack.peak = 0.53 * P0 * pow(m2, 0.125) * d / (pow(b, 0.75) * pow(len, 0.25));
        out->crack.dur = 1.82 * M * pow(b, 0.25) * d / (C_AIR * pow(m2, 0.375) * pow(len, 0.25));
    }
    r0 = js_hypot(p->D, p->miss);
    E = 0.5 * p->mass * p->v0 * p->v0;
    e3 = cbrt(E);
    out->r0 = r0;
    out->launchT = r0 / C_AIR;
    out->muzzleE = E;
    if (p->launch == LAUNCH_BLAST) {
        double rb = maxd(r0, 1), cosT = r0 > 0.01 ? p->D / r0 : 0;
        out->has_blast = 1;
        out->blast.t = r0 / C_AIR;
        out->blast.r = rb;
        out->blast.peak = 133 * e3 / rb * db_to_lin(6 * cosT);
        out->blast.T1 = 0.00003 * e3 * (1 + 0.6 * log10(1 + rb));
        out->blast.jetTau = 0.02 * sqrt(e3 / 15);
        out->blast.jetFc = 3000 / sqrt(e3 / 15);
    }
    if (tr->hit) {
        double vh = tr->v[n - 1], xh = tr->x[n - 1], rh = maxd(0.5, js_hypot(p->D - xh, p->miss));
        out->has_hit = 1;
        out->hit.tau = (n - 1) / TRAJ_HZ;
        out->hit.t = (n - 1) / TRAJ_HZ + rh / C_AIR;
        out->hit.x = xh;
        out->hit.v = vh;
        out->hit.r = rh;
        out->hit.E = 0.5 * p->mass * vh * vh;
    }
    out->flight = (n - 1) / TRAJ_HZ;
}

/* ------------------------------------------------------------------ layer synthesis */
static float *blast_buf(arena *ar, const pblast_t *b, rng_t *r, int *len_out)
{
    double T1 = b->T1, T2 = T1 * 6, a, lp = 0, norm, t, w;
    int len = (int)ceil((0.06 + b->jetTau * 7 + T2 * 8) * FS) + 2400, i, s0;
    float *buf = fnew(ar, len);
    shape_t s1 = { 0 }, s2 = { 0 };
    /* Friedlander: instant rise, exponential decay, negative phase */
    s1.nwave = 0; s1.A = b->peak; s1.T = T1; s1.k = 1;
    add_shape(buf, len, 0.0005, T1 * 10, &s1);
    /* slower gas-bubble pulse gives the thump its body */
    s2.nwave = 0; s2.A = 0.35 * b->peak; s2.T = T2; s2.k = 1;
    add_shape(buf, len, 0.0005 + T1 * 0.5, T2 * 10, &s2);
    /* propellant jet noise */
    a = exp(-TAU * b->jetFc / FS);
    norm = 1 / sqrt((1 - a) / (1 + a) / 3);
    s0 = (int)js_round((0.0005 + T1 * 0.3) * FS);
    for (i = s0; i < len - 2400; i++) {
        t = (i - s0) / FS;
        w = rnd(r) * 2 - 1;
        lp = w + (lp - w) * a;
        buf[i] = (float)(buf[i] + lp * norm * 0.16 * b->peak * exp(-t / b->jetTau));
    }
    *len_out = len;
    return buf;
}

static float *crack_buf(arena *ar, const crack_t *c, int *len_out)
{
    int len = (int)ceil((c->dur + 0.02) * FS) + 2400;
    float *buf = fnew(ar, len);
    shape_t s = { 0 };
    s.nwave = 1; s.A = c->peak; s.T = c->dur; s.k = 0;
    add_shape(buf, len, 0.0005, c->dur, &s);
    *len_out = len;
    return buf;
}

/* Modal body hit by an impulse: decaying partials + broadband contact burst + debris grains. Pressure at 1 m.
   maxModes / t60Scale / noGrains give a light version for the many small hits of debris and fragments. */
static float *modal_hit(arena *ar, const be_material *m, double E, double sizeScale, rng_t *r, int maxModes,
                        double t60Scale, int noGrains, int *len_out)
{
    double amp = 6 * m->eff * sqrt(maxd(E, 1e-6)), ts = t60Scale ? t60Scale : 1, rhoT = 1;
    int light = maxModes != 0, useGrains = m->has_grains && !noGrains, slack = light ? 64 : 2400;
    double cut = light ? 0.85 : 1.15, tail, gsum = 0;
    int len, body, k, i, nm, nmodes = 0;
    double mf[12], mg[12], mt[12];
    float *buf, *nb;
    int nbn;
    double np;
    tail = maxd(maxd(maxd(m->t60 * ts * rhoT * cut, useGrains ? m->grains.spread + 0.3 : 0), m->noise.dur * 4 + 0.01), 0.03);
    len = (int)ceil(tail * FS) + slack;
    buf = fnew(ar, len);
    body = len - slack;
    nm = light ? (maxModes < m->n_ratios ? maxModes : m->n_ratios) : m->n_ratios;
    for (k = 0; k < nm; k++) {
        double f = m->f0 * sizeScale * m->ratios[k] * (1 + (rnd(r) - 0.5) * 0.07), g, q;
        if (f > 19000)
            continue;
        g = (0.5 + rnd(r)) / sqrt(1 + k) / (1 + pow(f * m->contact * 3, 2));
        q = rnd(r);
        mf[nmodes] = f;
        mg[nmodes] = g;
        mt[nmodes] = m->t60 * ts / (1 + m->slope * k) * (0.8 + 0.4 * q);
        nmodes++;
        gsum += g;
    }
    for (k = 0; k < nmodes; k++) {
        double w = TAU * mf[k] / FS, R = pow(10, -3 / (mt[k] * FS)), c1 = 2 * R * cos(w), c2 = R * R;
        double y1, y2, y, A = amp * 0.7 * mg[k] / gsum;
        int nn = (int)mind(body, ceil(mt[k] * cut * FS));
        y = sin(w) * R; /* R^n sin(w n), starting from zero */
        buf[1] = (float)(buf[1] + A * y);
        y2 = 0;
        y1 = y;
        for (i = 2; i < nn; i++) {
            y = c1 * y1 - c2 * y2;
            y2 = y1;
            y1 = y;
            buf[i] = (float)(buf[i] + A * y);
        }
    }
    nbn = (int)ceil(m->noise.dur * 4 * FS) + 8;
    nb = fnew(ar, nbn);
    for (i = 0; i < nbn; i++)
        nb[i] = (float)((rnd(r) * 2 - 1) * exp(-i / FS / (m->noise.dur / 3)));
    one_pole_hp(nb, nbn, m->noise.hp);
    one_pole_lp(nb, nbn, m->noise.lp);
    np = peak_of(nb, 0, nbn);
    if (!np)
        np = 1;
    for (i = 0; i < nbn && i < body; i++)
        buf[i] = (float)(buf[i] + nb[i] / np * amp * m->noise.gain);
    if (useGrains) {
        const be_grain_spec *G = &m->grains;
        for (k = 0; k < G->count; k++) {
            double q1 = rnd(r), t0 = 0.01 + G->spread * pow(q1, 1.8), q2 = rnd(r);
            double ga = amp * G->gain * (0.3 + q2) * exp(-t0 / (G->spread * 0.5)), q3 = rnd(r);
            double gf = G->f[0] * pow(G->f[1] / G->f[0], q3), gd;
            int s = (int)floor(t0 * FS), j;
            if (G->ping) {
                double gw;
                gd = 0.03 + 0.12 * rnd(r);
                gw = TAU * gf / FS;
                for (j = 0; j < gd * 3 * FS && s + j < body; j++)
                    buf[s + j] = (float)(buf[s + j] + ga * sin(gw * j) * exp(-j / FS / (gd / 3)));
            } else {
                double st = 0, ca, w2;
                gd = 0.002 + 0.008 * rnd(r);
                ca = exp(-TAU * gf / FS);
                for (j = 0; j < gd * 4 * FS && s + j < body; j++) {
                    w2 = rnd(r) * 2 - 1;
                    st = w2 + (st - w2) * ca;
                    buf[s + j] = (float)(buf[s + j] + ga * (w2 - st * 0.7) * exp(-j / FS / gd));
                }
            }
        }
    }
    *len_out = len;
    return buf;
}

typedef struct { float *L, *R; int n; double t; } stereo_t;

/* Re-time an emission-domain signal to the ear: each path step maps to the stretch of output it arrives in. */
static int retime(arena *ar, const float *src, int ns, const traj_t *tr, double D, double miss, int hasPan, double fixedPan,
                  stereo_t *out)
{
    int n = tr->n, i, outN, nn;
    double Tmin = INFINITY, Tmax = -INFINITY, step = FS / TRAJ_HZ;
    for (i = 0; i < n; i++) {
        if (tr->T[i] < Tmin) Tmin = tr->T[i];
        if (tr->T[i] > Tmax) Tmax = tr->T[i];
    }
    outN = (int)ceil((Tmax - Tmin) * FS) + 4;
    out->L = fnew(ar, outN);
    out->R = fnew(ar, outN);
    out->n = outN;
    out->t = Tmin;
    for (i = 0; i < n - 1; i++) {
        double Ta = tr->T[i], Tb = tr->T[i + 1], lo, hi, xm, pan, gl, gr;
        int n0, n1;
        if (Ta == Tb)
            continue;
        lo = mind(Ta, Tb);
        hi = maxd(Ta, Tb);
        n0 = (int)ceil((lo - Tmin) * FS);
        n1 = (int)ceil((hi - Tmin) * FS) - 1;
        if (n1 < n0)
            continue;
        xm = (tr->x[i] + tr->x[i + 1]) / 2;
        if (hasPan)
            pan = fixedPan;
        else {
            double h = js_hypot(xm - D, miss);
            pan = (xm - D) / (h ? h : 1) * 0.9;
        }
        gl = cos((pan + 1) * PI / 4);
        gr = sin((pan + 1) * PI / 4);
        for (nn = n0; nn <= n1; nn++) {
            double fr = (Tmin + nn / FS - Ta) / (Tb - Ta), ks = (i + fr) * step, sv;
            int k0 = (int)floor(ks);
            if (k0 < 0 || k0 >= ns - 1)
                continue;
            sv = src[k0] + (src[k0 + 1] - src[k0]) * (ks - k0);
            out->L[nn] = (float)(out->L[nn] + sv * gl);
            out->R[nn] = (float)(out->R[nn] + sv * gr);
        }
    }
    return 1;
}

/* Aerodynamic wake, synthesised in emission time and re-timed to the ear (Doppler falls out of the geometry). */
static int wake_layer(arena *ar, const proj_t *p, const traj_t *tr, int cutAtHit, rng_t *r, stereo_t *out)
{
    int n = tr->n, ns, k, i;
    double dur = (n - 1) / TRAJ_HZ, d = p->cal / 1000;
    double lp = 0, bp = 0, br = 0, ph = 0, l1 = 0, l2 = 0, q = 1.1, fadeIn, fadeOut, sizeAmp;
    float *src;
    ns = (int)floor(dur * FS);
    if (ns < 64)
        return 0;
    src = fnew(ar, ns);
    fadeIn = mind(ns * 0.2, 0.003 * FS);
    fadeOut = cutAtHit ? 0.001 * FS : ns * 0.18;
    sizeAmp = pow(d / 0.0114, 1.5) * (1 + 8 * p->tumble);
    for (k = 0; k < ns; k++) {
        double u = k / FS * TRAJ_HZ, f, v, x, dT, rate, rr, w, fc, fk, hp, s, mm, a;
        i = (int)mind(n - 2, floor(u));
        f = u - i;
        v = tr->v[i] + (tr->v[i + 1] - tr->v[i]) * f;
        x = tr->x[i] + (tr->x[i + 1] - tr->x[i]) * f;
        dT = (tr->T[i + 1] - tr->T[i]) * TRAJ_HZ;
        rate = 1 / maxd(fabs(dT), 0.05);
        rr = maxd(0.3, js_hypot(p->D - x, p->miss));
        w = rnd(r) * 2 - 1;
        /* vortex shedding: band centred on Strouhal frequency 0.2 v / d */
        fc = clampd(0.2 * v / d, 40, 7500);
        fk = 2 * sin(PI * fc / FS);
        lp += fk * bp;
        hp = w - lp - q * bp;
        bp += fk * hp;
        br = w + (br - w) * 0.6;
        s = bp * sqrt(1 / fk) * 0.9 + br * 0.5;
        if (p->tumble > 0) {
            ph += TAU * p->tumbleHz * pow(maxd(v, 1) / p->v0, 1.5) / FS;
            s = s * (1 + 0.9 * p->tumble * sin(ph)) +
                p->tumble * 0.8 * (sin(ph) + 0.5 * sin(2 * ph) + 0.33 * sin(3 * ph) + 0.2 * sin(5 * ph));
        }
        mm = v / C_AIR;
        s *= KW * (mm < 1.2 ? mm * mm * mm : 1.44 * mm) * sizeAmp * mind(rate, 4) / rr;
        if (k < fadeIn) s *= k / fadeIn;
        if (k > ns - fadeOut) s *= (ns - k) / fadeOut;
        /* air absorption acts on received frequency, so scale the cutoff by the Doppler ratio */
        a = exp(-TAU * clampd(mind(air_fc(rr), 18000) / rate / 0.64, 30, 20000) / FS);
        l1 = s + (l1 - s) * a;
        l2 = l1 + (l2 - l1) * a;
        src[k] = (float)l2;
    }
    return retime(ar, src, ns, tr, p->D, p->miss, 0, 0, out);
}

/* ------------------------------------------------------------------ reverb: 8-line feedback delay network */
typedef struct { int n_delays; double delays[8]; double rt60, damp, pre; int n_taps; be_tap taps[3]; } fdn_opt;

static void fdn(arena *ar, const float *inL, const float *inR, int N, const fdn_opt *o, float **outL_, float **outR_)
{
    int K = o->n_delays, k, n, len[8], idx[8] = { 0 }, pre;
    float *outL = fnew(ar, N), *outR = fnew(ar, N), *bufs[8];
    double g[8], lp[8] = { 0 }, a = exp(-TAU * o->damp / FS);
    for (k = 0; k < K; k++) {
        len[k] = (int)maxd(8, js_round(o->delays[k] * FS / 1000));
        bufs[k] = fnew(ar, len[k]);
        g[k] = pow(10, -3 * len[k] / (o->rt60 * FS));
    }
    pre = (int)js_round(o->pre * FS / 1000);
    for (n = 0; n < N; n++) {
        int m = n - pre;
        double xl = m >= 0 ? inL[m] : 0, xr = m >= 0 ? inR[m] : 0, sum = 0, ol = 0, orr = 0, v, sg;
        for (k = 0; k < K; k++) {
            v = bufs[k][idx[k]];
            lp[k] = v + (lp[k] - v) * a;
            sum += lp[k];
        }
        sum *= 2.0 / K;
        for (k = 0; k < K; k++) {
            bufs[k][idx[k]] = (float)(((k & 1) ? xr : xl) + (lp[k] - sum) * g[k]);
            if (++idx[k] >= len[k])
                idx[k] = 0;
            sg = (k & 2) ? -lp[k] : lp[k];
            if (k & 1) orr += sg; else ol += sg;
        }
        outL[n] = (float)(ol * 0.5);
        outR[n] = (float)(orr * 0.5);
    }
    for (k = 0; k < o->n_taps; k++) {
        const be_tap *t = &o->taps[k];
        int dl = (int)js_round(t->ms * FS / 1000);
        double gl = t->g * cos((t->pan + 1) * PI / 4), gr = t->g * sin((t->pan + 1) * PI / 4), x;
        for (n = 0; n + dl < N; n++) {
            x = (inL[n] + inR[n]) * 0.5;
            outL[n + dl] = (float)(outL[n + dl] + x * gl);
            outR[n + dl] = (float)(outR[n + dl] + x * gr);
        }
    }
    *outL_ = outL;
    *outR_ = outR;
}

static void env_fdn_opt(const be_env *e, fdn_opt *o)
{
    memset(o, 0, sizeof *o);
    o->n_delays = e->n_delays;
    memcpy(o->delays, e->delays, sizeof o->delays);
    o->rt60 = e->rt60;
    o->damp = e->damp;
    o->pre = 0;
    o->n_taps = e->n_taps;
    memcpy(o->taps, e->taps, sizeof o->taps);
}

/* ------------------------------------------------------------------ the scene: layers and events */
typedef struct {
    float *m, *L, *R;        /* mono with a pan, or a stereo pair */
    int n;
    double t, pan, g;
} layer_t;

typedef struct {
    arena *ar;
    layer_t *layers;
    int n_layers, cap_layers;
    be_event *ev;
    int n_ev, cap_ev;
} scene_t;

static void push_mono(scene_t *sc, float *m, int n, double t, double pan, double g)
{
    layer_t *l;
    if (sc->n_layers == sc->cap_layers) {
        int cap = sc->cap_layers ? sc->cap_layers * 2 : 64;
        layer_t *nl = (layer_t *)ar_alloc(sc->ar, cap * sizeof *nl);
        if (sc->n_layers)
            memcpy(nl, sc->layers, sc->n_layers * sizeof *nl);
        sc->layers = nl;
        sc->cap_layers = cap;
    }
    l = &sc->layers[sc->n_layers++];
    l->m = m; l->L = l->R = 0; l->n = n; l->t = t; l->pan = pan; l->g = g;
}

static void push_stereo(scene_t *sc, float *L, float *R, int n, double t, double g)
{
    push_mono(sc, 0, n, t, 0, g);
    sc->layers[sc->n_layers - 1].L = L;
    sc->layers[sc->n_layers - 1].R = R;
}

static void push_event(scene_t *sc, int kind, double t)
{
    if (sc->n_ev == sc->cap_ev) {
        int cap = sc->cap_ev ? sc->cap_ev * 2 : 32;
        be_event *ne = (be_event *)ar_alloc(sc->ar, cap * sizeof *ne);
        if (sc->n_ev)
            memcpy(ne, sc->ev, sc->n_ev * sizeof *ne);
        sc->ev = ne;
        sc->cap_ev = cap;
    }
    sc->ev[sc->n_ev].kind = kind;
    sc->ev[sc->n_ev].t = t;
    sc->n_ev++;
}

/* ------------------------------------------------------------------ grains: debris that lands like a recording */
/* Ballistic's built-in example "ex:gravel": stones landing on stones, synthesised (seed 83), 6 s. */
static void add_at(float *dst, int dn, const float *src, int sn, double t, double g)
{
    int n0 = (int)js_round(t * FS), i, n = sn < dn - n0 ? sn : dn - n0;
    for (i = n0 < 0 ? -n0 : 0; i < n; i++)
        dst[n0 + i] = (float)(dst[n0 + i] + src[i] * g);
}

static void make_gravel(arena *ar, float **L_, float **R_, int *n_)
{
    rng_t r = rng_new(83);
    int n = 6 * FSI, i, k, len;
    float *L = fnew(ar, n), *R = fnew(ar, n);
    const be_material *concrete = be_material_find("concrete");
    double pk;
    for (i = 0; i < 70; i++) {
        double t = 0.1 + 5.7 * rnd(&r), sc = 1.1 + 3.2 * rnd(&r), q = rnd(&r), E = 0.0004 * pow(q, 2) + 0.00002;
        double pan = rnd(&r) * 2 - 1, gl = cos((pan + 1) * PI / 4), gr = sin((pan + 1) * PI / 4);
        for (k = 0; k < (rnd(&r) < 0.4 ? 2 : 1); k++) {
            float *b = modal_hit(ar, concrete, E * (k ? 0.25 : 1), sc, &r, 3, 0.7, 1, &len);
            double dt = k ? 0.03 + 0.06 * rnd(&r) : 0;
            add_at(L, n, b, len, t + dt, gl);
            add_at(R, n, b, len, t + dt, gr);
        }
    }
    pk = peak_of(L, R, n);
    if (!pk)
        pk = 1;
    scale(L, n, 0.9 / pk);
    scale(R, n, 0.9 / pk);
    *L_ = L;
    *R_ = R;
    *n_ = n;
}

typedef struct { double t, s; } hit_s;

/* Hits: a rise of the short-term level (2.5 ms frames) by more than a threshold within 20 ms, above a floor below
   the loudest frame. Sensitivity lowers both. Strength is each hit's peak against the strongest hit. */
static int detect_hits(arena *ar, const float *x, int xn, double sens, hit_s **out_)
{
    int hop = 120, nf = xn / hop, i, j, gap, cnt = 0;
    float *db = fnew(ar, nf);
    double top = -240, rise, floor_, last = -1e9, amax = 0;
    hit_s *out = (hit_s *)ar_alloc(ar, (size_t)(nf ? nf : 1) * sizeof *out);
    sens = clampd(sens, 0, 1);
    for (i = 0; i < nf; i++) {
        double e = 0, v;
        for (j = 0; j < hop; j++) {
            v = x[i * hop + j];
            e += v * v;
        }
        db[i] = (float)(10 * log10(e / hop + 1e-14));
        if (db[i] > top)
            top = db[i];
    }
    rise = 18 - 12 * sens;
    floor_ = top - (24 + 24 * sens);
    gap = (int)js_round(0.07 * FS / hop);
    for (i = 1; i < nf; i++) {
        double lo = INFINITY, lp = 0;
        int s0, s1, k;
        if (db[i] < floor_ || i - last < gap)
            continue;
        for (j = i - 8 > 0 ? i - 8 : 0; j < i; j++)
            if (db[j] < lo)
                lo = db[j];
        if (db[i] - lo < rise)
            continue;
        s0 = (i - 1) * hop;
        s1 = (int)mind(xn, s0 + js_round(0.03 * FS));
        for (k = s0; k < s1; k++)
            if (fabs(x[k]) > lp)
                lp = fabs(x[k]);
        for (k = s0; k < s1 && fabs(x[k]) < 0.3 * lp; k++)
            ;
        out[cnt].t = maxd(0, k - js_round(0.0005 * FS)) / FS;
        out[cnt].s = lp;
        cnt++;
        last = i;
    }
    for (i = 0; i < cnt; i++)
        if (out[i].s > amax)
            amax = out[i].s;
    for (i = 0; i < cnt; i++)
        out[i].s = out[i].s / (amax ? amax : 1);
    *out_ = out;
    return cnt;
}

typedef struct { float *b; int n; double c, s; } grain_t;
typedef struct { grain_t *g; int n; } grains_t;

/* Grains for debris: the hits of a recording, each cut until the next hit (at most 350 ms), ordered from dark to
   bright. A recording without clear hits is cut into 150 ms slices. */
static int cut_grains(arena *ar, const float *L, const float *R, int xn, grains_t *bank)
{
    float *x = fnew(ar, xn);
    hit_s *hits;
    int nh, i, j, cnt = 0, keep;
    grain_t *g;
    for (i = 0; i < xn; i++)
        x[i] = (float)(0.5 * (L[i] + R[i]));
    nh = detect_hits(ar, x, xn, 0.7, &hits);
    if (nh < 3) {
        double t;
        int m = 0;
        for (t = 0; t + 0.05 < xn / FS; t += 0.15)
            m++;
        hits = (hit_s *)ar_alloc(ar, (size_t)(m ? m : 1) * sizeof *hits);
        nh = 0;
        for (t = 0; t + 0.05 < xn / FS; t += 0.15) {
            hits[nh].t = t;
            hits[nh].s = 1;
            nh++;
        }
    }
    g = (grain_t *)ar_alloc(ar, (size_t)(nh ? nh : 1) * sizeof *g);
    for (i = 0; i < nh; i++) {
        int s0 = (int)js_round(hits[i].t * FS), s1, bn, fo, zc = 0, m;
        double pk, e1 = s0 + js_round(0.35 * FS), e2 = i + 1 < nh ? js_round(hits[i + 1].t * FS) - 48 : xn;
        float *b;
        s1 = (int)mind(mind(xn, e1), e2);
        if (s1 - s0 < 0.012 * FS)
            continue;
        bn = s1 - s0;
        b = fcopy(ar, x + s0, bn);
        pk = peak_of(b, 0, bn);
        if (!pk)
            continue;
        fo = (int)mind(bn >> 1, js_round(0.006 * FS));
        for (j = 0; j < fo; j++)
            b[bn - 1 - j] = (float)(b[bn - 1 - j] * ((double)j / fo));
        scale(b, bn, 1 / pk);
        m = (int)mind(bn, js_round(0.04 * FS));
        for (j = 1; j < m; j++)
            if ((b[j] >= 0) != (b[j - 1] >= 0))
                zc++;
        g[cnt].b = b;
        g[cnt].n = bn;
        g[cnt].c = zc * FS / (2 * m);
        g[cnt].s = hits[i].s;
        cnt++;
    }
    for (i = 1; i < cnt; i++) {               /* strongest first, stable */
        grain_t q = g[i];
        for (j = i - 1; j >= 0 && g[j].s < q.s; j--)
            g[j + 1] = g[j];
        g[j + 1] = q;
    }
    keep = cnt < 64 ? cnt : 64;
    for (i = 1; i < keep; i++) {              /* then dark to bright */
        grain_t q = g[i];
        for (j = i - 1; j >= 0 && g[j].c > q.c; j--)
            g[j + 1] = g[j];
        g[j + 1] = q;
    }
    bank->g = g;
    bank->n = keep;
    return keep > 0;
}

/* A piece takes a grain by its size (small pieces brighter), re-pitched by its size, at the level its landing
   energy gives. */
static float *grain_hit(arena *ar, const grains_t *bank, double E, double sc, double scRef, rng_t *r, int *len_out)
{
    int n = bank->n, len, i;
    double q1 = rnd(r), u = clampd(0.5 + 0.3 * log2(sc / scRef) + 0.3 * (q1 - 0.5), 0, 1), pr, amp;
    const grain_t *gr = &bank->g[(int)js_round(u * (n - 1))];
    float *out;
    pr = clampd(pow(sc / scRef, 0.35), 0.6, 1.7) * (0.94 + 0.12 * rnd(r));
    len = (int)maxd(1, floor((gr->n - 1) / pr));
    out = fnew(ar, len + 64);
    amp = 3.4 * sqrt(maxd(E, 1e-6));
    for (i = 0; i < len; i++) {
        double q = i * pr, f;
        int j = (int)q;
        f = q - j;
        out[i] = (float)(amp * (gr->b[j] + (gr->b[j + 1] - gr->b[j]) * f));
    }
    *len_out = len;
    return out;
}

/* ------------------------------------------------------------------ explosion */
typedef struct { double Z, peak, td; } xblast_t;
typedef struct { proj_t p; proj_an an; int flip; } frag_t;
typedef struct { double t, dx, dy, r, pan, m, E; } debris_t;
typedef struct {
    xblast_t blast;
    double t;
    frag_t *frags;
    int nf;
    debris_t *debris;
    int nd;
    double span, tGround, tDebris;
} xplan_t;

/* Free-air blast by cube-root (Hopkinson-Cranz) scaling, W in kg TNT. Overpressure fit after Mills (1987). */
static xblast_t blast_params(double W, double r)
{
    xblast_t b;
    double w3 = cbrt(W), Z = maxd(r, 0.3) / w3, Zc = maxd(Z, 0.6);
    double kPa = 1772 / (Zc * Zc * Zc) - 114 / (Zc * Zc) + 108 / Zc;
    if (Zc > 40)
        kPa *= sqrt(log(40) / log(Zc));
    b.Z = Z;
    b.peak = kPa * 1000;
    b.td = w3 * (1.2 + 2.2 * log10(1 + Zc)) / 1000;
    return b;
}

static void plan_explosion(arena *ar, double W, double r, int nFrag, int nDebris, rng_t *g, xplan_t *pl)
{
    double w3 = cbrt(W), span, rho, tFirst = INFINITY;
    int i;
    pl->blast = blast_params(W, r);
    pl->nf = nFrag > 0 ? nFrag : 0;
    pl->frags = (frag_t *)ar_alloc(ar, (pl->nf ? pl->nf : 1) * sizeof(frag_t));
    for (i = 0; i < pl->nf; i++) {
        frag_t *f = &pl->frags[i];
        double q = rnd(g), miss = mind(0.4 * pow(60, q), r * 0.95), cal = 4 + 14 * rnd(g), d = cal / 1000;
        proj_t *fp = &f->p;
        memset(fp, 0, sizeof *fp);
        fp->v0 = 250 + 1100 * pow(rnd(g), 1.5);
        fp->cal = cal;
        fp->mass = 7800 * PI / 6 * d * d * d * 2;
        fp->lenRatio = 1.5;
        fp->cdScale = 1.3;
        fp->tumble = 0.5 + 0.5 * rnd(g);
        fp->tumbleHz = 150 + 550 * rnd(g);
        fp->swing = 0;
        fp->pathLen = 0;
        fp->D = sqrt(r * r - miss * miss);
        fp->miss = miss;
        fp->launch = LAUNCH_NONE;
        fp->target = 0;
        fp->targetDist = 0;
        fp->maxT = 2.5;
        fp->vStop = 25;
        analyze_proj(ar, fp, &f->an);
        f->flip = rnd(g) < 0.5;
    }
    span = clampd(2.2 * pow(W, 0.18), 1, 7);
    rho = 8 * w3;
    pl->nd = nDebris > 0 ? nDebris : 0;
    pl->debris = (debris_t *)ar_alloc(ar, (pl->nd ? pl->nd : 1) * sizeof(debris_t));
    for (i = 0; i < pl->nd; i++) {
        debris_t *d = &pl->debris[i];
        double t = 0.35 + span * pow(rnd(g), 1.4), q = rnd(g), dd = rho * pow(q, 0.7) * (0.3 + t / span), phi = TAU * rnd(g);
        double dx = dd * cos(phi), dy = dd * sin(phi), rj = maxd(1, js_hypot(r - dx, dy));
        double m = 0.005 * pow(1 / (rnd(g) + 0.02), 1.2), vl = 9.81 * t / 2;
        d->t = t; d->dx = dx; d->dy = dy; d->r = rj; d->pan = clampd(dy / rj, -1, 1) * 0.9; d->m = m; d->E = 0.5 * m * vl * vl;
        if (t + rj / C_AIR < tFirst)
            tFirst = t + rj / C_AIR;
    }
    pl->t = r / C_AIR;
    pl->span = span;
    pl->tGround = r / 1500;
    pl->tDebris = tFirst;
}

typedef struct {
    double W, r, t0, pan, ground, gBlast, gRumble, gFrag, gDebris;
    const be_material *surface;
    const grains_t *grains;     /* debris from a recording's grains, or NULL */
} xopt_t;

/* Fragments and debris are far below the blast physically; FRAG_BOOST_DB and DEBRIS_BOOST_DB make them usable. */
static void explosion_layers(scene_t *sc, const xplan_t *pl, const xopt_t *o, rng_t *r)
{
    arena *ar = sc->ar;
    const xblast_t *b = &pl->blast;
    double W = o->W, tB = o->t0 + pl->t;
    float *buf;
    int n, i, len;
    if (o->gBlast > 0) {
        shape_t s = { 0 };
        double t2;
        len = (int)ceil((b->td * 14 + 0.02) * FS) + 2400;
        buf = fnew(ar, len);
        s.nwave = 0; s.A = b->peak; s.T = b->td; s.k = 1.2;
        add_shape(buf, len, 0.0005, b->td * 9, &s);
        t2 = b->td * 0.7;
        /* weaker second shock from the fireball re-collapsing */
        s.A = 0.18 * b->peak; s.T = t2;
        add_shape(buf, len, 0.0005 + b->td * 3.5, t2 * 9, &s);
        post_layer(ar, buf, len, o->r, o->ground);
        push_mono(sc, buf, len, tB, o->pan, o->gBlast);
    }
    push_event(sc, BE_EVENT_LAUNCH, tB);
    if (o->gRumble > 0) {
        double tau = 0.18 * pow(W, 0.25), fc = clampd(220 / pow(W, 1.0 / 6), 35, 400), lead;
        n = (int)ceil(tau * 6 * FS);
        buf = shaped_noise(ar, n, r, fc, 18);
        for (i = 0; i < n; i++) {
            double t = i / FS;
            buf[i] = (float)(buf[i] * ((1 - exp(-t / 0.015)) * exp(-t / tau)));
        }
        to_peak(buf, n, 0.3 * b->peak);
        air_filter(buf, n, o->r);
        push_mono(sc, buf, n, tB + b->td, o->pan, o->gRumble);
        /* the ground carries the shock about four times faster than the air */
        lead = pl->t - pl->tGround;
        if (lead > 0.06) {
            double gl = mind(lead, 0.9);
            n = (int)ceil((gl + 0.35) * FS);
            buf = shaped_noise(ar, n, r, 45, 14);
            for (i = 0; i < n; i++) {
                double tt = i / FS;
                buf[i] = (float)(buf[i] * (tt < gl ? pow(tt / gl, 2) : exp(-(tt - gl) / 0.1)));
            }
            to_peak(buf, n, 0.1 * b->peak);
            push_mono(sc, buf, n, tB - gl, o->pan, o->gRumble);
            push_event(sc, BE_EVENT_PASS, tB - gl);
        }
    }
    if (o->gFrag > 0) {
        double gF = o->gFrag * db_to_lin(FRAG_BOOST_DB);
        for (i = 0; i < pl->nf; i++) {
            const frag_t *f = &pl->frags[i];
            const proj_an *a = &f->an;
            double sgn = f->flip ? -1 : 1;
            stereo_t wk;
            if (!a->passed)
                continue;
            if (a->has_crack) {
                double rc = js_hypot(f->p.D - a->crack.x, f->p.miss);
                float *cb = crack_buf(ar, &a->crack, &len);
                post_layer(ar, cb, len, rc, o->ground);
                push_mono(sc, cb, len, o->t0 + a->crack.t, sgn * (a->crack.x - f->p.D) / rc * 0.9, gF);
            }
            if (wake_layer(ar, &f->p, &a->tr, 0, r, &wk))
                push_stereo(sc, f->flip ? wk.R : wk.L, f->flip ? wk.L : wk.R, wk.n, o->t0 + wk.t, gF);
        }
    }
    if (o->gDebris > 0 && pl->nd) {
        double gD = o->gDebris * db_to_lin(DEBRIS_BOOST_DB);
        const be_material *mat = o->surface ? o->surface : be_material_find("dirt");
        for (i = 0; i < pl->nd; i++) {
            const debris_t *d = &pl->debris[i];
            double sc_ = clampd(pow(0.05 / d->m, 1.0 / 3), 0.5, 3);
            float *hb = o->grains ? grain_hit(ar, o->grains, d->E, sc_, 1, r, &len)
                                  : modal_hit(ar, mat, d->E, sc_, r, 4, clampd(1 / sc_, 0.3, 1.5), 1, &len);
            scale(hb, len, 1 / d->r);
            air_filter(hb, len, d->r);
            push_mono(sc, hb, len, o->t0 + d->t + d->r / C_AIR, clampd(d->pan + o->pan, -1, 1), gD);
        }
        push_event(sc, BE_EVENT_IMPACT, o->t0 + pl->tDebris);
    }
}

/* the four layer groups' gains of one repetition, when it starts, its pan */
typedef struct { double g1, g2, g3, g4, t0, pan, ground; } gains_t;

/* ------------------------------------------------------------------ blaster: launcher, bolt in flight, target */
static float fsin(double cyc) { return SINT[(int)((cyc - floor(cyc)) * 8192) & 8191]; }

static double js_hypot3(double a, double b, double c)
{
    double v[3], max = 0, sum = 0, comp = 0, n, summand, pre;
    int i;
    v[0] = fabs(a); v[1] = fabs(b); v[2] = fabs(c);
    for (i = 0; i < 3; i++)
        if (v[i] > max)
            max = v[i];
    if (max == 0)
        return 0;
    for (i = 0; i < 3; i++) {
        n = v[i] / max;
        summand = n * n - comp;
        pre = sum + summand;
        comp = (pre - sum) - summand;
        sum = pre;
    }
    return sqrt(sum) * max;
}

static float *stretch(arena *ar, const float *x, int M, double s, int *n_out);

enum { LN_NONE, LN_LINE, LN_RAIL, LN_COIL, LN_GAS };
enum { BV_NONE, BV_BOLT, BV_BEAM, BV_SLUG };

typedef struct { double f, a, t60; } bmode_t;
typedef struct { double d, g; } bpath_t;
typedef struct { double kap; bpath_t *paths; int np; bmode_t *modes; int nm; double fLo, eta, tailMax, tail; } bend_t;
typedef struct { double Lw; double *f, *tau; int K; double *em, *eg; int ne; double tFirst, tEnd, tThump, fTrans, fLo; } line_t;
typedef struct { double t, v, d; } stage_t;
typedef struct {
    int type;
    double tC, transit;
    line_t line;
    bend_t rail, rail2;
    stage_t *stages;
    int ns;
    double Lb, pipe, helm;
} launcher_t;
typedef struct { double x, r, ta, pan, pk; } seg_t;
typedef struct { double Rc, Tn; seg_t *segs; int n; double ds; } channel_t;
typedef struct {
    int type;
    double xEnd, flight;
    int pulses;
    double period, v;
    channel_t ch;
    int has_tr;
    traj_t tr;
    proj_t fp;
    proj_an an;
    double mass, vHit;
    int reached;
} bolt_t;
typedef struct { double te, E; } thit_t;
typedef struct {
    const be_ablation *A;
    const char *mat;
    double x, rT, pan, E;
    thit_t *hits;
    int nh;
    double abl;
    int has_plate;
    bend_t plate;
} target_t;
typedef struct { launcher_t ln; bolt_t bo; int has_tg; target_t tg; double exitT, r0, pan0; } weapon_t;

static int launcher_of(const char *s)
{
    if (!strcmp(s, "none")) return LN_NONE;
    if (!strcmp(s, "line")) return LN_LINE;
    if (!strcmp(s, "rail")) return LN_RAIL;
    if (!strcmp(s, "coil")) return LN_COIL;
    if (!strcmp(s, "gas")) return LN_GAS;
    return -1;
}

static int bolt_of(const char *s)
{
    if (!strcmp(s, "none")) return BV_NONE;
    if (!strcmp(s, "bolt")) return BV_BOLT;
    if (!strcmp(s, "beam")) return BV_BEAM;
    if (!strcmp(s, "slug")) return BV_SLUG;
    return -1;
}

static const be_body *body_find(const char *id)
{
    int i;
    for (i = 0; i < BODY_COUNT; i++)
        if (!strcmp(BODIES[i].id, id))
            return &BODIES[i];
    return 0;
}

static const be_plate *plate_find(const char *id)
{
    int i;
    for (i = 0; i < PLATE_COUNT; i++)
        if (!strcmp(PLATES[i].id, id))
            return &PLATES[i];
    return 0;
}

static const be_ablation *ablation_find(const char *id)
{
    int i;
    for (i = 0; i < ABLATION_COUNT; i++)
        if (!strcmp(ABLATION[i].id, id))
            return &ABLATION[i];
    return 0;
}

/* the struck dispersive line: its sweep (frequency against arrival time) and its echoes */
static void plan_line(arena *ar, const be_params *p, line_t *pl)
{
    double Lw = PV(p, bpLength), kap = PV(p, bpStiff), cT = PV(p, bpTension) < 2 ? 0 : PV(p, bpTension);
    int i, j, K = 0, ne = 1, maxe = (int)js_round(PV(p, bpEchoes));
    pl->f = dnew(ar, 321);
    pl->tau = dnew(ar, 321);
    for (i = 0; i <= 320; i++) {
        double k = 1e-3 * pow(1e7, i / 320.0), w = k * sqrt(cT * cT + kap * kap * k * k), fq = w / TAU;
        if (fq < 40 || fq > 18000)
            continue;
        pl->f[K] = fq;
        pl->tau[K] = Lw * w / (cT * cT * k + 2 * kap * kap * k * k * k);
        K++;
    }
    pl->em = dnew(ar, (maxe > 0 ? maxe : 0) + 1);
    pl->eg = dnew(ar, (maxe > 0 ? maxe : 0) + 1);
    pl->em[0] = 1;
    pl->eg[0] = 1;
    for (j = 1; j <= maxe; j++) {
        if ((2 * j + 1) * pl->tau[0] > 6)
            break;
        pl->em[ne] = 2 * j + 1;
        pl->eg[ne] = pow(PV(p, bpRefl), j);
        ne++;
    }
    pl->Lw = Lw;
    pl->K = K;
    pl->ne = ne;
    pl->tFirst = pl->tau[K - 1];
    pl->tEnd = pl->em[ne - 1] * pl->tau[0];
    pl->tThump = cT ? Lw / cT : 0;
    pl->fTrans = cT ? sqrt(2.0) * cT * cT / (TAU * kap) : 0;
    pl->fLo = pl->f[0];
}

static void chirp_into(float *buf, int len, const line_t *pl, const be_params *p, double m, double gain)
{
    const double *f = pl->f, *tau = pl->tau;
    int j = pl->K - 1, n0 = (int)ceil(m * tau[j] * FS), n1 = (int)mind(len - 1, floor(m * tau[0] * FS)), n;
    double ph = 0, fh = PV(p, bpHard), dk = PV(p, bpDamp) * 0.02 * m * pl->Lw;
    for (n = n0; n <= n1; n++) {
        double tm = n / FS / m, w, fq, dfdt;
        while (j > 0 && tau[j - 1] < tm)
            j--;
        if (j == 0)
            break;
        w = (tm - tau[j]) / (tau[j - 1] - tau[j]);
        fq = f[j] * pow(f[j - 1] / f[j], w);
        /* stationary phase: amplitude follows the square root of the sweep rate */
        dfdt = mind(3e6, (f[j] - f[j - 1]) / (m * (tau[j - 1] - tau[j])));
        ph += fq / FS;
        buf[n] = (float)(buf[n] + gain * sqrt(dfdt) / (1 + (fq / fh) * (fq / fh)) * exp(-dk * fq / 1000) *
                         mind(1, (n - n0) / 32.0) * fsin(ph));
    }
}

static void damped_sine(float *buf, int len, double t0, double f, double q, double amp)
{
    int n0 = (int)js_round(t0 * FS), n, i;
    double tau = q / (PI * f);
    n = (int)mind(len - n0, ceil(tau * 7 * FS));
    for (i = 0; i < n; i++) {
        double t = i / FS;
        buf[n0 + i] = (float)(buf[n0 + i] + amp * exp(-t / tau) * fsin(f * t) * mind(1, i / 24.0));
    }
}

static void noise_burst(arena *ar, float *buf, int len, double t0, double dur, double hp, double lp, double amp, rng_t *r,
                        double attack)
{
    int n0 = (int)js_round(t0 * FS), n = (int)mind(len - n0, ceil(dur * 5 * FS)), i;
    float *b;
    double pk;
    if (n < 8)
        return;
    b = fnew(ar, n);
    for (i = 0; i < n; i++)
        b[i] = (float)(rnd(r) * 2 - 1);
    if (lp) { one_pole_lp(b, n, lp); one_pole_lp(b, n, lp); }
    if (hp) one_pole_hp(b, n, hp);
    pk = peak_of(b, 0, n);
    if (!pk)
        pk = 1;
    for (i = 0; i < n; i++) {
        double t = i / FS;
        buf[n0 + i] = (float)(buf[n0 + i] + amp * b[i] / pk * exp(-t / dur) * mind(1, t / (attack ? attack : 0.0005)));
    }
}

static void add_n(float *buf, int len, double t0, double dur, double peak)
{
    int n0 = (int)maxd(0, floor(t0 * FS)), n1 = (int)mind(len - 1, ceil((t0 + dur) * FS)), n, k;
    for (n = n0; n <= n1; n++) {
        double acc = 0, t;
        for (k = 0; k < 4; k++) {
            t = (n + (k + 0.5) / 4) / FS - t0;
            if (t >= 0 && t <= dur)
                acc += 1 - 2 * t / dur;
        }
        buf[n] = (float)(buf[n] + peak * acc / 4);
    }
}

/* charger whine: converter pitch climbs along the capacitor charge curve, then stops dead at the discharge */
static float *charge_whine(arena *ar, double dur, double fEnd, int *len_out)
{
    int cn = (int)ceil(dur * FS), i;
    float *cb = fnew(ar, cn + 2400);
    double ph = 0, k1 = 1 - exp(-3);
    for (i = 0; i < cn; i++) {
        double u = (double)i / cn, f = 1500 + (fEnd - 1500) * (1 - exp(-3 * u)) / k1;
        ph += f / FS;
        cb[i] = (float)((fsin(ph) + 0.3 * fsin(2 * ph) + 0.15 * fsin(0.5 * ph)) * (0.15 + 0.85 * u) * mind(1, i / 480.0) *
                        mind(1, (cn - i) / 96.0));
    }
    *len_out = cn + 2400;
    return cb;
}

/* Bending waves: after a distance d, frequency f arrives at t = d / (2 sqrt(2 pi kappa f)), a falling sweep. */
static void bend_chirp(float *buf, int len, double t0, double d, double kap, double gain, double fHi, double fLo, double eta,
                       double hard, double ph0)
{
    double K = d * d / (8 * PI * kap), ta = sqrt(K / fHi), tb = sqrt(K / maxd(fLo, 1));
    int n0 = (int)ceil((t0 + ta) * FS), n1 = (int)mind(len - 1, floor((t0 + tb) * FS)), fade, n, q = 0;
    fade = (int)maxd(8, mind(512, (n1 - n0) >> 2));
    for (n = n0; n <= n1; n++) {
        double t = n / FS - t0, f = K / (t * t), sw = 2 * K / (t * t * t);
        double a = gain * sqrt(sw) / (1 + (f / hard) * (f / hard)) * exp(-PI * eta * f * t);
        if (q < 24) a *= ++q / 24.0;
        if (n1 - n < fade) a *= (double)(n1 - n) / fade;
        buf[n] = (float)(buf[n] + a * fsin(ph0 + K * (1 / ta - 1 / t)));
    }
}

/* The struck body as an impulse response: dispersive sweeps along the strongest paths, then its modes ring on. Peak 1. */
static float *bend_ir(arena *ar, bend_t *s, double hard, rng_t *r, int *len_out)
{
    double tEnd = 0, tTail = 0, pc, pt;
    int i, n, k;
    float *ir, *tl;
    for (k = 0; k < s->np; k++) {
        double tb = s->paths[k].d / (2 * sqrt(TAU * s->kap * s->fLo));
        if (tb > tEnd) tEnd = tb;
    }
    for (k = 0; k < s->nm; k++) {
        s->modes[k].t60 = mind(s->tailMax, 2.2 / (s->eta * s->modes[k].f));
        if (s->modes[k].t60 > tTail) tTail = s->modes[k].t60;
    }
    n = (int)ceil((mind(maxd(tEnd, tTail), 6) + 0.05) * FS);
    ir = fnew(ar, n);
    tl = fnew(ar, n);
    for (k = 0; k < s->np; k++) {
        double ph0 = rnd(r);
        bend_chirp(ir, n, 0, s->paths[k].d, s->kap, s->paths[k].g, mind(16000, hard * 2.5), s->fLo, s->eta, hard, ph0);
    }
    for (k = 0; k < s->nm; k++) {
        const bmode_t *md = &s->modes[k];
        double R = pow(10, -3 / (md->t60 * FS)), w = TAU * md->f / FS, c1 = 2 * R * cos(w), c2 = R * R, y1 = R * sin(w), y2 = 0, y;
        double A = md->a / (1 + (md->f / hard) * (md->f / hard));
        int nn = (int)mind(n, ceil(md->t60 * FS));
        tl[1] = (float)(tl[1] + A * y1);
        for (i = 2; i < nn; i++) {
            y = c1 * y1 - c2 * y2;
            y2 = y1;
            y1 = y;
            tl[i] = (float)(tl[i] + A * y);
        }
    }
    pc = peak_of(ir, 0, n);
    if (!pc) pc = 1;
    pt = peak_of(tl, 0, n);
    if (!pt) pt = 1;
    for (i = 0; i < n; i++)
        ir[i] = (float)(ir[i] / pc + s->tail * tl[i] / pt);
    to_peak(ir, n, 1);
    *len_out = n;
    return ir;
}

static void sort_paths(bpath_t *p, int n)      /* loudest first, stable */
{
    int i, j;
    for (i = 1; i < n; i++) {
        bpath_t x = p[i];
        for (j = i - 1; j >= 0 && p[j].g < x.g; j--)
            p[j + 1] = p[j];
        p[j + 1] = x;
    }
}

/* A rectangular plate: edge reflections as image sources, simply supported modes for the long ring. */
static void plan_plate(arena *ar, const be_plate *m, double h, double size, bend_t *out)
{
    double kap = h * sqrt(m->E / (12 * m->rho * (1 - m->nu * m->nu))), a = size, b = size * 0.72, x0 = 0.37 * a, y0 = 0.41 * b;
    double xo = 0.63 * a, yo = 0.57 * b, f11;
    int i, j, sx, sy, np = 0, nm = 0;
    bpath_t *paths = (bpath_t *)ar_alloc(ar, 484 * sizeof *paths);
    bmode_t *modes = (bmode_t *)ar_alloc(ar, 900 * sizeof *modes);
    for (i = -5; i <= 5; i++)
        for (j = -5; j <= 5; j++)
            for (sx = 0; sx < 2; sx++)
                for (sy = 0; sy < 2; sy++) {
                    double X = 2 * i * a + (sx ? -x0 : x0), Y = 2 * j * b + (sy ? -y0 : y0), d = maxd(0.02, js_hypot(X - xo, Y - yo));
                    paths[np].d = d;
                    paths[np].g = pow(0.9, abs(2 * i - sx) + abs(2 * j - sy)) / sqrt(d);
                    np++;
                }
    sort_paths(paths, np);
    f11 = PI / 2 * kap * (1 / (a * a) + 1 / (b * b));
    for (i = 1; i <= 30; i++)
        for (j = 1; j <= 30; j++) {
            double f = PI / 2 * kap * (i * i / (a * a) + j * j / (b * b)), w;
            if (f < 20 || f > 10000)
                continue;
            w = sin(i * PI * x0 / a) * sin(j * PI * y0 / b) * sin(i * PI * xo / a) * sin(j * PI * yo / b);
            if (fabs(w) > 0.05) {
                modes[nm].f = f;
                modes[nm].a = w;
                nm++;
            }
        }
    for (i = 1; i < nm; i++) {                 /* by frequency, stable */
        bmode_t x = modes[i];
        for (j = i - 1; j >= 0 && modes[j].f > x.f; j--)
            modes[j + 1] = modes[j];
        modes[j + 1] = x;
    }
    if (nm > 64) {
        /* keep 64 spread over the band; each stands for the ones skipped around it */
        double st = nm / 64.0;
        bmode_t *kept = (bmode_t *)ar_alloc(ar, 64 * sizeof *kept);
        for (i = 0; i < 64; i++) {
            const bmode_t *md = &modes[(int)floor(i * st)];
            kept[i].f = md->f;
            kept[i].a = md->a * sqrt(st);
        }
        modes = kept;
        nm = 64;
    }
    out->kap = kap;
    out->paths = paths;
    out->np = np < 40 ? np : 40;
    out->modes = modes;
    out->nm = nm;
    out->fLo = maxd(f11, 25);
    out->eta = m->eta;
    out->tailMax = 3;
    out->tail = 0.4;
}

/* A rail: a free-free steel bar 4 cm deep, struck near the breech and heard from its middle. */
static void plan_rail(arena *ar, double L, bend_t *out)
{
    static const double beta[5] = { 4.730, 7.853, 10.996, 14.137, 17.279 };
    double kap = 0.04 / sqrt(12) * sqrt(200e9 / 7850), xs = 0.05 * L, xo = 0.5 * L;
    int k, np = 0, nm = 0;
    bpath_t *paths = (bpath_t *)ar_alloc(ar, 26 * sizeof *paths);
    bmode_t *modes = (bmode_t *)ar_alloc(ar, 60 * sizeof *modes);
    for (k = -6; k <= 6; k++) {
        paths[np].d = maxd(0.05, fabs(2 * k * L + xs - xo));
        paths[np].g = pow(0.85, abs(2 * k));
        np++;
        paths[np].d = maxd(0.05, fabs(2 * k * L - xs - xo));
        paths[np].g = pow(0.85, abs(2 * k - 1));
        np++;
    }
    sort_paths(paths, np);
    for (k = 0; k < 60; k++) {
        double bl = k < 5 ? beta[k] : (k + 1.5) * PI, f = bl * bl * kap / (TAU * L * L);
        if (f > 9000)
            break;
        if (f > 20) {
            modes[nm].f = f;
            modes[nm].a = (k & 1 ? -1 : 1) / sqrt(1 + k);
            nm++;
        }
    }
    out->kap = kap;
    out->paths = paths;
    out->np = 16;
    out->modes = modes;
    out->nm = nm;
    out->fLo = 30;
    out->eta = 0.008;
    out->tailMax = 2.5;
    out->tail = 0.5;
}

/* Line launcher: the struck dispersive line with its echoes, thump and body, all on the launcher level. */
static void line_launcher(scene_t *sc, const line_t *pl, const be_params *p, double gL, double tA, double pan, double r)
{
    arena *ar = sc->ar;
    int nD, nE, i, j, k;
    float *dir, *ech;
    double amp1, scl, pkd;
    push_event(sc, BE_EVENT_CRACK, tA + pl->tFirst);
    if (gL <= 0)
        return;
    nD = (int)ceil((pl->tau[0] + 0.06) * FS);
    nE = (int)ceil((pl->tEnd + 0.06) * FS);
    dir = fnew(ar, nD + 64);
    ech = fnew(ar, nE + 64);
    amp1 = 3 * sqrt(PV(p, bpEnergy));
    chirp_into(dir, nD + 64, pl, p, 1, 1);
    for (j = 1; j < pl->ne; j++)
        chirp_into(ech, nE + 64, pl, p, pl->em[j], pl->eg[j]);
    pkd = peak_of(dir, 0, nD + 64);
    scl = amp1 / (pkd ? pkd : 1);
    scale(dir, nD + 64, scl);
    scale(ech, nE + 64, scl);
    if (strcmp(PS(p, bpBody), "none")) {
        const be_body *bd = body_find(PS(p, bpBody));
        int bn = nE + (int)ceil(0.5 * FS);
        float *body = fnew(ar, bn);
        for (k = 0; k < bd->nf; k++) {
            double bf = bd->f[k], R = exp(-PI * bf / (bd->q * FS)), c1 = 2 * R * cos(TAU * bf / FS), c2 = R * R, y1 = 0, y2 = 0, y;
            double g = 1 / (1 + k * 0.5);
            for (i = 0; i < bn; i++) {
                y = c1 * y1 - c2 * y2 + (i < nD ? dir[i] : 0) + (i < nE ? ech[i] : 0);
                y2 = y1;
                y1 = y;
                body[i] = (float)(body[i] + g * y);
            }
        }
        to_peak(body, bn, 0.8 * amp1);
        scale(body, bn, 1 / r); air_filter(body, bn, r); push_mono(sc, body, bn, tA, pan, gL);
    }
    if (pl->tThump > 0) {
        double fL = clampd(pl->fTrans * 0.5, 40, 400);
        int tn = nE + (int)ceil(0.2 * FS);
        float *th = fnew(ar, tn);
        for (k = 0; k < pl->ne; k++) {
            shape_t s = { 0 };
            s.A = 0.8 * amp1 * pl->eg[k];
            s.k = 1;
            s.mul4 = fL;
            add_shape(th, tn, pl->em[k] * pl->tThump, 3 / fL, &s);
        }
        scale(th, tn, 1 / r); air_filter(th, tn, r); push_mono(sc, th, tn, tA, pan, gL);
        push_event(sc, BE_EVENT_IMPACT, tA + pl->tThump);
    }
    scale(dir, nD + 64, 1 / r); air_filter(dir, nD + 64, r); push_mono(sc, dir, nD + 64, tA, pan, gL);
    if (pl->ne > 1) {
        scale(ech, nE + 64, 1 / r); air_filter(ech, nE + 64, r); push_mono(sc, ech, nE + 64, tA, pan, gL);
    }
}

static void plan_launcher(arena *ar, const be_params *q, double vExit, launcher_t *out)
{
    int t = launcher_of(PS(q, lnType)), i;
    memset(out, 0, sizeof *out);
    out->type = t;
    if ((t == LN_RAIL || t == LN_COIL) && PV(q, lnCharge) > 0.05)
        out->tC = PV(q, lnCharge);
    if (t == LN_LINE)
        plan_line(ar, q, &out->line);
    else if (t == LN_RAIL) {
        plan_rail(ar, PV(q, lnLength), &out->rail);
        plan_rail(ar, PV(q, lnLength) * 1.013, &out->rail2);
        out->transit = PV(q, lnLength) / maxd(vExit / 2, 50);
    } else if (t == LN_COIL) {
        /* each stage adds the same energy: the stages fire closer and closer together, each pull shorter */
        int N = (int)clampd(js_round(PV(q, lnStages)), 2, 24);
        double s = PV(q, lnLength) / N, tt = 0, vPrev = 0;
        out->stages = (stage_t *)ar_alloc(ar, N * sizeof(stage_t));
        for (i = 1; i <= N; i++) {
            double vi = maxd(vExit, 5) * sqrt((double)i / N), dt = s / maxd((vPrev + vi) / 2, 2.5);
            out->stages[i - 1].t = tt;
            out->stages[i - 1].v = vi;
            out->stages[i - 1].d = clampd(dt / 2, 0.0001, 0.08);
            tt += dt;
            vPrev = vi;
        }
        out->ns = N;
        out->transit = tt;
    } else if (t == LN_GAS) {
        double Lb = clampd(PV(q, lnLength), 0.05, 2);
        out->Lb = Lb;
        out->pipe = C_AIR / (4 * (Lb + 0.006));                                        /* barrel: a quarter-wave pipe */
        out->helm = C_AIR / TAU * sqrt(2.83e-5 / (PV(q, lnChamber) * 1e-6 * 0.012));   /* chamber behind a 6 mm port */
        out->transit = 0.007;
    }
}

static void put_at(scene_t *sc, float *b, int n, double t, double pan, double g, double r)
{
    scale(b, n, 1 / r);
    air_filter(b, n, r);
    push_mono(sc, b, n, t, pan, g);
}

static void launcher_layers(scene_t *sc, launcher_t *ln, const be_params *p, double gL, double t0, double r0, double pan0,
                            rng_t *r)
{
    arena *ar = sc->ar;
    double tA, tF, E = PV(p, lnEnergy), lvl;
    float *buf;
    int len, i, k;
    if (ln->type == LN_NONE)
        return;
    tA = t0 + r0 / C_AIR;
    tF = tA + ln->tC;
    lvl = 3 * sqrt(maxd(E, 1));   /* Pa at 1 m for the launcher's loudest part: set against the bolt, not measured */
    if (ln->tC > 0) {
        push_event(sc, BE_EVENT_PASS, tA);
        if (gL > 0) {
            buf = charge_whine(ar, ln->tC, PV(p, lnChargeHz), &len);
            to_peak(buf, len, 0.05 * lvl);
            put_at(sc, buf, len, tA, pan0, gL, r0);
        }
    }
    if (ln->type == LN_LINE) {
        line_launcher(sc, &ln->line, p, gL, tF, pan0, r0);
        return;
    }
    push_event(sc, BE_EVENT_LAUNCH, tF);
    if (gL <= 0)
        return;
    if (ln->type == LN_RAIL) {
        /* capacitor dump: switch crack and LC ring, then the armature arc along the rails and a plasma flash */
        xblast_t b;
        shape_t s = { 0 };
        float *fb, *ir1, *ir2;
        int fn, n1, n2;
        len = (int)ceil(1.2 * FS);
        buf = fnew(ar, len);
        add_n(buf, len, 0.0005, 0.00012, 0.5 * lvl);
        damped_sine(buf, len, 0.0005, PV(p, lnRingHz), 9, 0.35 * lvl);
        noise_burst(ar, buf, len, 0.0005, maxd(ln->transit, 0.004) * 0.6, 900, 9000, 0.45 * lvl, r, 0.0003);
        put_at(sc, buf, len, tF, pan0, gL, r0);
        b = blast_params(clampd(E * 0.02 / 4.184e6, 1e-7, 50), maxd(r0, 0.5));
        fn = (int)ceil((b.td * 14 + 0.02) * FS) + 2400;
        fb = fnew(ar, fn);
        s.A = b.peak; s.T = b.td; s.k = 1.2;
        add_shape(fb, fn, 0.0005, b.td * 9, &s);
        air_filter(fb, fn, r0);
        push_mono(sc, fb, fn, tF + ln->transit, pan0, gL);
        /* the rails ring: bending waves from the breech, reflected at the free ends, then the bar modes */
        ir1 = bend_ir(ar, &ln->rail, 8000, r, &n1);
        ir2 = bend_ir(ar, &ln->rail2, 8000, r, &n2);
        to_peak(ir1, n1, 0.5 * lvl);
        to_peak(ir2, n2, 0.4 * lvl);
        put_at(sc, ir1, n1, tF, clampd(pan0 - 0.25, -1, 1), gL, r0);
        put_at(sc, ir2, n2, tF + 0.0003, clampd(pan0 + 0.25, -1, 1), gL, r0);
        buf = modal_hit(ar, be_material_find("steel"), E * 0.001, clampd(2 / PV(p, lnLength), 0.2, 2), r, 0, 0, 0, &len);
        to_peak(buf, len, 0.4 * lvl);
        put_at(sc, buf, len, tF + ln->transit, pan0, gL, r0);
    } else if (ln->type == LN_COIL) {
        len = (int)ceil((ln->transit + 0.8) * FS);
        buf = fnew(ar, len);
        for (k = 0; k < ln->ns; k++) {
            const stage_t *s = &ln->stages[k];
            int n0, nn;
            double a;
            add_n(buf, len, s->t + 0.0003, 0.00008, 0.25 * lvl);   /* thyristor switch click */
            /* the coil pulls while the slug crosses it: one cycle of force, shorter at every stage */
            n0 = (int)js_round((s->t + 0.0003) * FS);
            nn = (int)js_round(2 * s->d * FS);
            a = 0.6 * lvl * sqrt((double)(k + 1) / ln->ns);
            for (i = 0; i < nn && n0 + i < len; i++) {
                double u = (double)i / nn;
                buf[n0 + i] = (float)(buf[n0 + i] + a * sin(PI * u) * sin(TAU * u));
            }
            damped_sine(buf, len, s->t + 0.0003, clampd(1 / s->d, 200, 6000), 6, 0.12 * lvl);
            if (k)
                push_event(sc, BE_EVENT_PASS, tF + s->t);
        }
        damped_sine(buf, len, ln->transit, PV(p, lnRingHz), 9, 0.25 * lvl);
        put_at(sc, buf, len, tF, pan0, gL, r0);
    } else if (ln->type == LN_GAS) {
        float *ph, *ck;
        int pn, cn;
        const be_material *steel = be_material_find("steel");
        len = (int)ceil(1.0 * FS);
        buf = fnew(ar, len);
        noise_burst(ar, buf, len, 0, 0.02, 1500, 9000, 0.5 * lvl, r, 0.0004);           /* valve opens */
        damped_sine(buf, len, 0.002, ln->helm, 8, 0.35 * lvl);                          /* chamber pops (Helmholtz) */
        ph = modal_hit(ar, steel, 2, 1.6, r, 6, 0.5, 1, &pn);
        to_peak(ph, pn, 0.45 * lvl);
        add_at(buf, len, ph, pn, 0.004, 1);                                             /* driver blade strikes */
        for (k = 1; k <= 7; k += 2)
            damped_sine(buf, len, ln->transit, ln->pipe * k, 30, 0.3 * lvl / k);         /* barrel rings */
        if (PV(p, lnSpring) > 0.01) {
            /* the return spring twangs: waves on a helix are dispersive too */
            int sn = (int)ceil(0.6 * FS);
            float *sp = fnew(ar, sn);
            for (k = 0; k < 6; k++) {
                double ph0 = rnd(r);
                bend_chirp(sp, sn, 0, 0.6 * (2 * k + 1), 0.15, pow(0.8, k), 9000, 60, 0.01, 5000, ph0);
            }
            to_peak(sp, sn, 0.3 * lvl * PV(p, lnSpring));
            add_at(buf, len, sp, sn, 0.022, 1);
        }
        ck = modal_hit(ar, steel, 0.6, 2.2, r, 4, 0.3, 1, &cn);
        to_peak(ck, cn, 0.3 * lvl);
        add_at(buf, len, ck, cn, 0.045, 1);                                             /* driver returns */
        noise_burst(ar, buf, len, 0.06, 0.12, 400, 6000, 0.18 * lvl, r, 0.004);        /* exhaust */
        put_at(sc, buf, len, tF, pan0, gL, r0);
    }
}

/* A straight heated channel: each stretch sends an N-wave about 2.6 Rc / c long, Rc = sqrt(E per metre / (pi P0)). */
static void plan_channel(arena *ar, double L, double El, double D, double miss, double v, rng_t *r, channel_t *out)
{
    double Rc = sqrt(El / (PI * P0)), ds, r0;
    int n = (int)maxd(2, ceil(L / maxd(4 * Rc, L / 240))), i;
    ds = L / n;
    r0 = maxd(ds / 2, Rc);
    out->segs = (seg_t *)ar_alloc(ar, n * sizeof(seg_t));
    for (i = 0; i < n; i++) {
        seg_t *s = &out->segs[i];
        double x = (i + 0.5) * ds, q1 = rnd(r), y = (q1 - 0.5) * 0.15 * ds, q2 = rnd(r), z = (q2 - 0.5) * 0.15 * ds;
        double rr = maxd(maxd(js_hypot3(D - x, miss - y, z), Rc), 0.05);
        s->x = x;
        s->r = rr;
        s->ta = x / v + rr / C_AIR;
        s->pan = clampd((x - D) / rr, -1, 1) * 0.9;
        s->pk = 0.2 * P0 * pow(Rc / r0, 0.75) * r0 / maxd(rr, r0) * mind(1, ds / (2 * r0));
    }
    out->Rc = Rc;
    out->Tn = 2.6 * Rc / C_AIR;
    out->n = n;
    out->ds = ds;
}

typedef double (*xfn_t)(const void *st, double t);
static double x_linear(const void *st, double t) { return *(const double *)st * t; }

static void traj_kin_fn(arena *ar, double dur, double D, double miss, xfn_t xf, const void *st, traj_t *tr)
{
    int n = (int)js_round(dur * TRAJ_HZ) + 1, i;
    tr->x = dnew(ar, n);
    tr->v = dnew(ar, n);
    tr->T = dnew(ar, n);
    for (i = 0; i < n; i++) {
        double t = i / TRAJ_HZ;
        tr->x[i] = xf(st, t);
        tr->T[i] = t + js_hypot(D - tr->x[i], miss) / C_AIR;
        tr->v[i] = i ? (tr->x[i] - tr->x[i - 1]) * TRAJ_HZ : 0;
    }
    if (n > 1)
        tr->v[0] = tr->v[1];
    tr->n = n;
    tr->hit = 0;
    tr->xEnd = 0;
}

static void traj_kin(arena *ar, double dur, double D, double miss, double v, traj_t *tr)
{
    traj_kin_fn(ar, dur, D, miss, x_linear, &v, tr);
}

static void plan_bolt(arena *ar, const be_params *q, rng_t *r, bolt_t *out)
{
    int t = bolt_of(PS(q, bvType));
    int target = strcmp(PS(q, tgMat), "none") != 0;
    double xEnd = target ? PV(q, targetDist) : PV(q, D) + clampd(20 * PV(q, miss), 20, 400);
    memset(out, 0, sizeof *out);
    out->type = t;
    out->xEnd = maxd(0.1, xEnd);
    out->pulses = 1;
    if (t == BV_BOLT || t == BV_BEAM) {
        double v = t == BV_BEAM ? 3e8 : maxd(PV(q, bvSpeed), 5);
        out->v = v;
        plan_channel(ar, out->xEnd, PV(q, bvChannel), PV(q, D), PV(q, miss), v, r, &out->ch);
        out->flight = out->xEnd / v;
        if (t == BV_BEAM) {
            out->pulses = (int)clampd(js_round(PV(q, bvPulses)), 1, 60);
            out->period = 1 / maxd(PV(q, bvRate), 1);
        } else {
            traj_kin(ar, out->flight, PV(q, D), PV(q, miss), v, &out->tr);
            out->has_tr = 1;
        }
    } else if (t == BV_SLUG) {
        double d = PV(q, bvCal) / 1000, lr = !strcmp(PS(q, lnType), "gas") ? 20 : 3;
        proj_t *fp = &out->fp;
        memset(fp, 0, sizeof *fp);
        fp->v0 = PV(q, bvSpeed);
        fp->cal = PV(q, bvCal);
        fp->mass = 7800 * PI / 4 * d * d * lr * d;
        fp->lenRatio = lr;
        fp->cdScale = 1;
        fp->tumbleHz = 200;
        fp->pathLen = target ? maxd(0.05, PV(q, targetDist)) : 0;
        fp->D = PV(q, D);
        fp->miss = PV(q, miss);
        fp->launch = LAUNCH_NONE;
        fp->maxT = 6;
        analyze_proj(ar, fp, &out->an);
        out->v = PV(q, bvSpeed);
        out->mass = fp->mass;
        out->tr = out->an.tr;
        out->has_tr = 1;
        out->flight = (out->tr.n - 1) / TRAJ_HZ;
        out->vHit = out->tr.v[out->tr.n - 1];
        out->reached = target && out->tr.x[out->tr.n - 1] >= fp->pathLen * 0.995;
    }
}

/* a sustained source on a path: synthesised in emission time, then re-timed to the ear */
typedef double (*gen_fn)(void *st, int k, int i, double f, double x, double r);

static int moving_source(arena *ar, const traj_t *tr, double D, double miss, gen_fn gen, void *st, int hasPan, double fixedPan,
                         stereo_t *out)
{
    int n = tr->n, ns = (int)floor((n - 1) / TRAJ_HZ * FS), k, i;
    double l1 = 0, l2 = 0;
    float *src;
    if (ns < 64)
        return 0;
    src = fnew(ar, ns);
    for (k = 0; k < ns; k++) {
        double u = k / FS * TRAJ_HZ, f, x, dT, rate, rr, s, a;
        i = (int)mind(n - 2, floor(u));
        f = u - i;
        x = tr->x[i] + (tr->x[i + 1] - tr->x[i]) * f;
        dT = (tr->T[i + 1] - tr->T[i]) * TRAJ_HZ;
        rate = 1 / maxd(fabs(dT), 0.05);
        rr = maxd(1, js_hypot(D - x, miss));
        s = gen(st, k, i, f, x, rr) / rr;
        a = exp(-TAU * clampd(mind(air_fc(rr), 18000) / rate / 0.64, 30, 20000) / FS);
        l1 = s + (l1 - s) * a;
        l2 = l1 + (l2 - l1) * a;
        src[k] = (float)l2;
    }
    return retime(ar, src, ns, tr, D, miss, hasPan, fixedPan, out);
}

typedef struct { rng_t *r; double lp0, sh; } sheath_st;
static double sheath_gen(void *v, int k, int i_, double f_, double x_, double r_)
{
    sheath_st *s = (sheath_st *)v;
    double w = rnd(s->r) * 2 - 1;
    s->lp0 += (w - s->lp0) * 0.3;
    return 25 * s->sh * (w - s->lp0);
}

typedef struct { rng_t *r; double ph, lp, ck, hum, sizzle; } voice_st;
static double voice_gen(void *v, int kk, int i_, double f_, double x_, double r_)
{
    voice_st *s = (voice_st *)v;
    double w;
    s->ph += s->hum * (1 + 0.03 * fsin(kk * 6 / FS)) / FS;
    w = rnd(s->r) * 2 - 1;
    s->lp += (w - s->lp) * 0.35;
    s->ck *= 0.97;
    if (rnd(s->r) < 0.004)
        s->ck += (rnd(s->r) - 0.5) * 4;
    return 8 * s->sizzle * ((fsin(s->ph) + 0.4 * fsin(2 * s->ph + 0.1) + 0.25 * fsin(3 * s->ph + 0.3)) * 0.5 + 0.6 * (w - s->lp) + s->ck);
}

static void bolt_layers(scene_t *sc, bolt_t *bo, const be_params *p, double gB, double t0, rng_t *r)
{
    arena *ar = sc->ar;
    if (bo->type == BV_NONE)
        return;
    if (bo->type == BV_SLUG) {
        const proj_an *a = &bo->an;
        const proj_t *fp = &bo->fp;
        stereo_t wk, mv;
        double sh;
        if (a->has_crack)
            push_event(sc, BE_EVENT_CRACK, t0 + a->crack.t);
        else if (a->passed)
            push_event(sc, BE_EVENT_PASS, t0 + a->passT);
        if (gB <= 0)
            return;
        if (a->has_crack) {
            int len;
            double rc = js_hypot(fp->D - a->crack.x, fp->miss);
            float *cb = crack_buf(ar, &a->crack, &len);
            post_layer(ar, cb, len, rc, PV(p, ground));
            push_mono(sc, cb, len, t0 + a->crack.t, (a->crack.x - fp->D) / rc * 0.9, gB);
        }
        if (wake_layer(ar, fp, &a->tr, bo->reached, r, &wk))
            push_stereo(sc, wk.L, wk.R, wk.n, t0 + wk.t, gB);
        /* plasma sheath: above about Mach 4 the air behind the bow shock ionises and sizzles */
        sh = PV(p, bvSizzle) * clampd((bo->v / C_AIR - 3.5) / 3, 0, 1);
        if (sh > 0.01) {
            sheath_st st;
            st.r = r; st.lp0 = 0; st.sh = sh;
            if (moving_source(ar, &a->tr, fp->D, fp->miss, sheath_gen, &st, 0, 0, &mv))
                push_stereo(sc, mv.L, mv.R, mv.n, t0 + mv.t, gB);
        }
        return;
    }
    {
        const channel_t *ch = &bo->ch;
        const seg_t *segs = ch->segs, *close = &segs[0];
        double tMin = INFINITY, tMax = 0, rMin = INFINITY, rMax = 0, lr, pkAll = 0;
        int NB = 6, len, k, i, b;
        float *bL[6] = { 0 }, *bR[6] = { 0 }, *body;
        double br[6] = { 0 };
        int bnum[6] = { 0 };
        for (i = 0; i < ch->n; i++) {
            if (segs[i].ta < tMin) tMin = segs[i].ta;
            if (segs[i].ta > tMax) tMax = segs[i].ta;
            if (segs[i].r < rMin) rMin = segs[i].r;
            if (segs[i].r > rMax) rMax = segs[i].r;
        }
        push_event(sc, BE_EVENT_CRACK, t0 + tMin);
        if (gB <= 0)
            return;
        lr = log(rMax / rMin);
        if (!lr) lr = 1;
        len = (int)ceil((tMax - tMin + (bo->pulses - 1) * bo->period * 1.05 + ch->Tn * 4 + 0.12) * FS);
        for (k = 0; k < bo->pulses; k++) {
            double q = rnd(r), tp = k * bo->period * (1 + 0.04 * (q - 0.5)), gk = 1;
            if (k)
                gk = 0.75 + 0.3 * rnd(r);
            for (i = 0; i < ch->n; i++) {
                const seg_t *s = &segs[i];
                int bi = (int)mind(NB - 1, floor(log(s->r / rMin) / lr * NB));
                double q1, pk, q2, T, ts;
                if (!bL[bi]) { bL[bi] = fnew(ar, len); bR[bi] = fnew(ar, len); }
                q1 = rnd(r);
                pk = s->pk * gk * (0.6 + 0.8 * q1);
                q2 = rnd(r);
                T = ch->Tn * (0.8 + 0.45 * q2);
                ts = s->ta - tMin + tp + 0.001;
                if (!k) { br[bi] += s->r; bnum[bi]++; }
                add_n(bL[bi], len, ts, T, pk * cos((s->pan + 1) * PI / 4));
                add_n(bR[bi], len, ts, T, pk * sin((s->pan + 1) * PI / 4));
            }
        }
        for (i = 0; i < ch->n; i++)
            if (segs[i].r < close->r)
                close = &segs[i];
        body = fnew(ar, len);
        for (b = 0; b < NB; b++) {
            double pk;
            if (!bL[b])
                continue;
            pk = peak_of(bL[b], bR[b], len);
            if (pk > pkAll) pkAll = pk;
            for (i = 0; i < len; i++)
                body[i] = (float)(body[i] + (bL[b][i] + bR[b][i]) * 0.5);
            air_filter(bL[b], len, br[b] / bnum[b]);
            air_filter(bR[b], len, br[b] / bnum[b]);
            push_stereo(sc, bL[b], bR[b], len, t0 + tMin - 0.001, gB);
        }
        /* the heated core expands and the air rushes back in behind it: a low whump under the crack */
        one_pole_lp(body, len, 160); one_pole_lp(body, len, 160); one_pole_lp(body, len, 160);
        to_peak(body, len, 0.35 * pkAll);
        push_mono(sc, body, len, t0 + tMin - 0.001, close->pan, gB);
        if (bo->type == BV_BOLT && PV(p, bvSizzle) > 0.01 && bo->has_tr) {
            /* the bolt's own voice: a hot plasma ball hums, hisses and spits as it flies past, with Doppler */
            voice_st st;
            stereo_t mv;
            st.r = r; st.ph = 0; st.lp = 0; st.ck = 0; st.hum = PV(p, bvTone); st.sizzle = PV(p, bvSizzle);
            if (moving_source(ar, &bo->tr, PV(p, D), PV(p, miss), voice_gen, &st, 0, 0, &mv))
                push_stereo(sc, mv.L, mv.R, mv.n, t0 + mv.t, gB);
        }
    }
}

static void plan_target(arena *ar, const be_params *q, const bolt_t *bo, double exitT, target_t *tg, int *has)
{
    double E, x, P;
    int k, n;
    const be_plate *pm;
    *has = 0;
    if (!strcmp(PS(q, tgMat), "none"))
        return;
    if (bo->type == BV_SLUG && !bo->reached)
        return;
    memset(tg, 0, sizeof *tg);
    E = bo->type == BV_SLUG ? 0.5 * bo->mass * bo->vHit * bo->vHit : PV(q, tgEnergy);
    n = bo->pulses ? bo->pulses : 1;
    P = n;
    x = maxd(0, PV(q, targetDist));
    tg->hits = (thit_t *)ar_alloc(ar, n * sizeof(thit_t));
    for (k = 0; k < n; k++) {
        tg->hits[k].te = exitT + bo->flight + k * bo->period;
        tg->hits[k].E = E / P;
    }
    tg->nh = n;
    tg->mat = PS(q, tgMat);
    tg->A = ablation_find(tg->mat);
    tg->x = x;
    tg->rT = maxd(0.5, js_hypot(PV(q, D) - x, PV(q, miss)));
    tg->pan = clampd((x - PV(q, D)) / tg->rT, -1, 1) * 0.9;
    tg->E = E;
    /* a kinetic hit only flash-boils the surface at hypervelocity; a plain strike (no bolt) does not at all */
    tg->abl = bo->type == BV_SLUG ? clampd(bo->vHit / 3000, 0.05, 1) : bo->type == BV_NONE ? 0 : 1;
    pm = plate_find(tg->mat);
    if (pm) {
        plan_plate(ar, pm, PV(q, tgThick) / 1000, PV(q, tgSize), &tg->plate);
        tg->has_plate = 1;
    }
    *has = 1;
}

/* one collapse of a 5 mm steam bubble (Rayleigh-Plesset, as the implosion model), reused at every size */
static float *bubble_pulse(arena *ar, int *len_out)
{
    double rho = 1000, c = 1480, gam = 1.4, pInf = 1 * P0, pg0 = 0.05 * pInf, R0 = 0.005;
    double tc = 0.915 * R0 * sqrt(rho / (pInf - pg0)), Tsim = mind(1.6, tc * 14 + 0.02);
    int N = (int)ceil(Tsim * FS) + 2, n = 0, steps = 0, i, ip = 0, a0, e0, on;
    double *Q = dnew(ar, N), R = R0, U = 0, t = 0, dtMax = 1 / (2 * FS), uMax = 2.5 * c, flo = 1e-5 * R0, damp = 0.6 / tc, pk = 0;
    float *b, *out;
#define BE_ACC(Rx, Ux, res) do { double pg_ = pg0 * pow(R0 / (Rx), 3 * gam); \
        res = ((pg_ - pInf) / rho - 1.5 * (Ux) * (Ux) - 3 * gam * pg_ * (Ux) / (rho * c)) / (Rx); } while (0)
    while (t < Tsim && steps < 1200000) {
        double dt = mind(dtMax, 0.004 * R / (fabs(U) + 1e-6)), a1, Rm, Um, Rn, Un, tn, a2;
        if (dt < 1e-10) dt = 1e-10;
        BE_ACC(R, U, a1);
        Rm = maxd(flo, R + U * dt / 2);
        Um = U + a1 * dt / 2;
        Rn = maxd(flo, R + Um * dt);
        BE_ACC(Rm, Um, a2);
        Un = clampd((U + a2 * dt) * (1 - damp * dt), -uMax, uMax);
        tn = t + dt;
        while (n < N && n / FS <= tn) {
            double f = (n / FS - t) / dt, Ri = R + (Rn - R) * f, Ui = U + (Un - U) * f;
            Q[n++] = Ri * Ri * Ui;
        }
        R = Rn; U = Un; t = tn; steps++;
    }
#undef BE_ACC
    for (; n < N; n++)
        Q[n] = R * R * U;
    b = fnew(ar, N - 1);
    for (i = 0; i < N - 1; i++) {
        b[i] = (float)(rho * (Q[i + 1] - Q[i]) * FS);
        if (fabs(b[i]) > pk) { pk = fabs(b[i]); ip = i; }
    }
    a0 = (int)maxd(0, ip - js_round(0.0015 * FS));
    e0 = (int)mind(N - 1, ip + js_round(0.006 * FS));
    on = e0 - a0;
    out = fcopy(ar, b + a0, on);
    for (i = 0; i < on; i++)
        out[i] = (float)(out[i] * mind(mind(1, i / 16.0), (on - i) / 64.0));
    to_peak(out, on, 1);
    *len_out = on;
    return out;
}

static void target_layers(scene_t *sc, target_t *tg, const be_params *p, double g3, double g4, double t0, rng_t *r)
{
    arena *ar = sc->ar;
    double rT = tg->rT, Et = tg->E, tH, lvl;
    const char *mat = tg->mat;
    const thit_t *first = &tg->hits[0], *last = &tg->hits[tg->nh - 1];
    const char *spatter = tg->A ? tg->A->spatter : 0;
    int i, k, len;
    float *buf;
    tH = t0 + first->te + rT / C_AIR;
    lvl = 10 * sqrt(maxd(Et, 0.01));   /* Pa at 1 m for the struck body: set against the bolt, not measured */
    push_event(sc, BE_EVENT_IMPACT, tH);
    if (g3 > 0) {
        /* ablation: the energy that boils the surface off acts as a small blast, then the vapour jet hisses */
        const be_ablation *A = tg->A;
        float *abl[3];
        int abn[3], na = 0;
        for (k = 0; k < (tg->abl > 0 ? (tg->nh < 3 ? tg->nh : 3) : 0); k++) {
            double Eh = tg->hits[k].E;
            xblast_t b = blast_params(clampd(Eh * A->frac * tg->abl / 4.184e6, 1e-9, 100), rT);
            int an = (int)ceil((b.td * 14 + 0.3) * FS) + 2400;
            float *ab = fnew(ar, an);
            shape_t s = { 0 };
            s.A = b.peak; s.T = b.td; s.k = 1.2;
            add_shape(ab, an, 0.0005, b.td * 9, &s);
            noise_burst(ar, ab, an, 0.0008, 0.02 + 0.04 * log10(1 + Eh / 50), 1500, 12000, A->hiss * b.peak * 0.5 * tg->abl, r, 0.0005);
            post_layer(ar, ab, an, rT, PV(p, ground) * 0.5);
            abl[na] = ab;
            abn[na] = an;
            na++;
        }
        if (na)
            for (k = 0; k < tg->nh; k++)
                push_mono(sc, abl[k % na], abn[k % na], t0 + tg->hits[k].te + rT / C_AIR, tg->pan, g3);
        if (tg->has_plate) {
            /* the plate rings with bending waves; a pulsed beam strikes it again before it settles */
            int n, P = tg->nh;
            float *ir = bend_ir(ar, &tg->plate, !strcmp(mat, "wood") ? 3000 : 7000, r, &n);
            to_peak(ir, n, lvl);
            if (P > 1) {
                int span = (int)js_round((last->te - first->te) * FS), cn = n + span + 8;
                float *cv = fnew(ar, cn);
                for (k = 0; k < P; k++) {
                    double q = rnd(r);
                    add_at(cv, cn, ir, n, tg->hits[k].te - first->te, (0.7 + 0.3 * q) / sqrt(P));
                }
                ir = cv;
                n = cn;
            }
            put_at(sc, ir, n, tH, tg->pan, g3, rT);
        } else if (!strcmp(mat, "concrete")) {
            buf = modal_hit(ar, be_material_find("concrete"), Et * 0.2, clampd(pow(500 / maxd(Et, 1), 0.15), 0.3, 2), r, 0, 0, 0, &len);
            to_peak(buf, len, 0.8 * lvl);
            put_at(sc, buf, len, tH, tg->pan, g3, rT);
        } else if (!strcmp(mat, "water")) {
            int bpn, nb;
            float *bp;
            len = (int)ceil(2.5 * FS);
            buf = fnew(ar, len);
            noise_burst(ar, buf, len, 0, 0.18 + 0.1 * log10(1 + Et / 1000), 150, 3000, 0.6 * lvl, r, 0.002);   /* splash */
            bp = bubble_pulse(ar, &bpn);
            nb = (int)js_round(10 + 30 * log10(1 + Et / 100));
            for (k = 0; k < nb; k++) {
                /* steam bubbles collapse in the cold water around the hit: a crackle of sharp pulses */
                double q1 = rnd(r), R = 0.0005 + 0.0035 * pow(q1, 1.5), q2, tt, q3;
                int sn;
                float *st = stretch(ar, bp, bpn, maxd(0.15, R / 0.005), &sn);
                q2 = rnd(r);
                tt = 0.01 + 0.25 * -log(1 - q2 * 0.97);
                q3 = rnd(r);
                add_at(buf, len, st, sn, tt, 0.12 * lvl * (R / 0.004) * (0.5 + q3));
            }
            put_at(sc, buf, len, tH, tg->pan, g3, rT);
        }
        if (spatter) {
            /* molten droplets, shards or chips land around the hit */
            int ns = (int)js_round(3 + 5 * log10(1 + Et / 100) * (!strcmp(mat, "glass") || !strcmp(mat, "concrete") ? 2 : 1));
            int sn = (int)ceil(1.2 * FS);
            float *sp = fnew(ar, sn);
            const be_material *sm = be_material_find(spatter);
            for (k = 0; k < ns; k++) {
                double q1 = rnd(r), q2 = rnd(r), q3, q4;
                int hn;
                float *hb = modal_hit(ar, sm, 0.02 * (0.2 + q1), 2.5 + 3 * q2, r, 3, 0.35, 1, &hn);
                q3 = rnd(r);
                q4 = rnd(r);
                add_at(sp, sn, hb, hn, 0.04 + 0.5 * pow(q3, 1.5), 0.5 + q4);
            }
            to_peak(sp, sn, 0.12 * lvl);
            put_at(sc, sp, sn, tH, tg->pan, g3, rT);
        }
    }
    if (g4 > 0 && PV(p, tgHeat) > 0.01) {
        double heat = PV(p, tgHeat), t1 = t0 + last->te + rT / C_AIR, ts, lp = 0, ck = 0, nf;
        int steam = !strcmp(mat, "ice") || !strcmp(mat, "water"), n;
        float *sz;
        /* sizzle: the hot spot boils and spits, then calms as it cools */
        ts = (steam ? 0.8 : 0.3) + (steam ? 2.2 : 1.6) * heat;
        n = (int)ceil((ts * 3 + 0.1) * FS);
        sz = fnew(ar, n);
        nf = n / 3.0;
        for (i = 0; i < n; i++) {
            double t = i / FS, w = rnd(r) * 2 - 1;
            lp += (w - lp) * (steam ? 0.25 : 0.12);
            ck *= 0.92;
            if (rnd(r) < 0.002 * exp(-t / ts) * (steam ? 0.5 : 1.5))
                ck += (rnd(r) - 0.3) * 6;
            sz[i] = (float)(((w - lp) * (steam ? 1 : 0.5) + ck) * exp(-t / ts) * mind(mind(1, t / 0.02), (n - i) / nf));
        }
        to_peak(sz, n, lvl * 1.2 * heat * (steam ? 1.6 : 1));
        put_at(sc, sz, n, t1 + 0.01, tg->pan, g4, rT);
        if (strcmp(mat, "water")) {
            /* ticks: the hot spot contracts and the body clicks. Ice cracks instead, and each crack sings */
            int nt = (int)js_round(heat * (!strcmp(mat, "ice") ? 10 : 22)), tn;
            double tau = 0.5 + 1.6 * heat;
            float *tb;
            const be_material *tm = be_material_find(spatter ? spatter : "steel");
            tn = (int)ceil((tau * 3 + 1.4) * FS);
            tb = fnew(ar, tn);
            for (k = 0; k < nt; k++) {
                double q = rnd(r), tt2 = 0.2 + tau * -log(1 - q * 0.95);
                if (!strcmp(mat, "ice") && tg->has_plate) {
                    int cn = (int)ceil(1.2 * FS);
                    float *cr = fnew(ar, cn);
                    double q1 = rnd(r), q2 = rnd(r), q3;
                    bend_chirp(cr, cn, 0, 5 + 60 * q1, tg->plate.kap, 1, 9000, 40, 0.004, 6000, q2);
                    q3 = rnd(r);
                    to_peak(cr, cn, 0.3 + 0.7 * q3);
                    add_at(tb, tn, cr, cn, tt2, 1);
                } else {
                    double q1 = rnd(r), q2 = rnd(r), q3;
                    int kn;
                    float *tk = modal_hit(ar, tm, 0.01 * (0.2 + q1), 2.5 + 3 * q2, r, 3, 0.35, 1, &kn);
                    q3 = rnd(r);
                    to_peak(tk, kn, 0.3 + 0.7 * q3);
                    add_at(tb, tn, tk, kn, tt2, 1);
                }
            }
            to_peak(tb, tn, lvl * (!strcmp(mat, "ice") ? 1.5 : 0.6) * heat);
            put_at(sc, tb, tn, t1, tg->pan, g4, rT);
        }
        push_event(sc, BE_EVENT_PASS, t1 + 0.2);
    }
}

static void plan_weapon(arena *ar, const be_params *q, rng_t *g, weapon_t *wp)
{
    const char *bv = PS(q, bvType);
    double vExit = !strcmp(bv, "slug") || !strcmp(bv, "bolt") ? PV(q, bvSpeed) : 2000;
    plan_launcher(ar, q, vExit, &wp->ln);
    wp->exitT = wp->ln.tC + wp->ln.transit;
    plan_bolt(ar, q, g, &wp->bo);
    plan_target(ar, q, &wp->bo, wp->exitT, &wp->tg, &wp->has_tg);
    wp->r0 = maxd(0.5, js_hypot(PV(q, D), PV(q, miss)));
    wp->pan0 = -PV(q, D) / wp->r0 * 0.9;
}

static void weapon_layers(scene_t *sc, weapon_t *wp, const be_params *p, const gains_t *o, rng_t *r)
{
    launcher_layers(sc, &wp->ln, p, o->g1, o->t0, wp->r0, clampd(wp->pan0 + o->pan, -1, 1), r);
    bolt_layers(sc, &wp->bo, p, o->g2, o->t0 + wp->exitT, r);
    if (wp->has_tg)
        target_layers(sc, &wp->tg, p, o->g3, o->g4, o->t0, r);
}

/* ------------------------------------------------------------------ discharge: sparks, arcs, lightning */
typedef struct { double r, t, pan, pk; } dseg_t;
typedef struct {
    double El, Rc, Tn;
    dseg_t *segs, *bsegs;
    int ns, nb;
    double rMin, rMax, rMean, tFirst, tLast, dev, tC;
} dplan_t;

/* a crooked channel: each step bends a little from the last, pulled back toward the main direction */
static int walk_channel(arena *ar, const double *start, const double *main_, double len, double ds, double tort, rng_t *r,
                        double **pts_out)
{
    int n = (int)maxd(2, js_round(len / ds)), i, k;
    double *pts = dnew(ar, 3 * (n + 1)), u[3], q[3], m;
    for (k = 0; k < 3; k++) {
        pts[k] = start[k];
        u[k] = main_[k];
        q[k] = start[k];
    }
    for (i = 0; i < n; i++) {
        for (k = 0; k < 3; k++) {
            double w = rnd(r);
            u[k] = u[k] * 0.8 + main_[k] * 0.2 + tort * 0.9 * (w - 0.5);
        }
        m = js_hypot3(u[0], u[1], u[2]);
        if (!m) m = 1;
        q[0] = q[0] + u[0] / m * ds;
        q[1] = q[1] + u[1] / m * ds;
        q[2] = maxd(0, q[2] + u[2] / m * ds);
        for (k = 0; k < 3; k++)
            pts[3 * (i + 1) + k] = q[k];
    }
    *pts_out = pts;
    return n + 1;
}

static void dseg_add(dseg_t *s, const double *a, const double *c, double gain, double Rc, double r0, double ds)
{
    static const double ear[3] = { 0, 0, 1.7 };
    double mid[3], r;
    int k;
    for (k = 0; k < 3; k++)
        mid[k] = (a[k] + c[k]) / 2;
    r = maxd(maxd(js_hypot3(mid[0] - ear[0], mid[1] - ear[1], mid[2] - ear[2]), Rc), 0.05);
    s->r = r;
    s->t = r / C_AIR;
    s->pan = clampd(mid[1] / r, -1, 1) * 0.9;
    s->pk = gain * 0.2 * P0 * pow(Rc / r0, 0.75) * r0 / maxd(r, r0) * mind(1, ds / (2 * r0));
}

/* Thunder after Few: the channel heats a cylinder of relaxation radius Rc = sqrt(E per metre / (pi P0)); every
   stretch of it sends an N-wave about 2.6 Rc / c long, and the ear hears them spread out by distance. */
static void plan_discharge(arena *ar, const be_params *p, rng_t *g, dplan_t *pl)
{
    double E = PV(p, dcEnergy), Lc = PV(p, dcLength), El = E / Lc, Rc = sqrt(El / (PI * P0)), d = PV(p, dcDist), Tn, ds, r0;
    double start[3], main_[3], *pts, rSum = 0;
    int vert = !strcmp(PS(p, dcOrient), "vertical"), nSeg, npts, i, b, nbr = (int)js_round(PV(p, dcBranches)), cap;
    /* a channel shorter than its own shock radius acts as a point source */
    if (Rc > Lc / 2)
        Rc = cbrt(3 * E / (4 * PI * P0));
    Tn = 2.6 * Rc / C_AIR;
    nSeg = (int)clampd(js_round(Lc / maxd(4 * Rc, Lc / 300)), 3, 300);
    ds = Lc / nSeg;
    if (vert) { start[0] = d; start[1] = 0; start[2] = Lc * 0.85; main_[0] = 0; main_[1] = 0; main_[2] = -1; }
    else { start[0] = d; start[1] = -Lc * 0.42; start[2] = 1.5; main_[0] = 0; main_[1] = 1; main_[2] = 0; }
    npts = walk_channel(ar, start, main_, Lc, ds, PV(p, dcTort), g, &pts);
    r0 = maxd(ds / 2, Rc);
    pl->segs = (dseg_t *)ar_alloc(ar, (npts - 1) * sizeof(dseg_t));
    pl->ns = npts - 1;
    for (i = 0; i < npts - 1; i++)
        dseg_add(&pl->segs[i], pts + 3 * i, pts + 3 * (i + 1), 1, Rc, r0, ds);
    cap = 0;
    pl->bsegs = 0;
    pl->nb = 0;
    for (b = 0; b < nbr; b++) {
        double q = rnd(g), dir[3], *bp, lb;
        const double *from = pts + 3 * (1 + (int)floor(q * (npts - 2)));
        int k, nbp;
        for (k = 0; k < 3; k++) {
            double w = rnd(g);
            dir[k] = main_[k] + (w - 0.5) * 1.6;
        }
        q = rnd(g);
        lb = Lc * (0.1 + 0.25 * q);
        nbp = walk_channel(ar, from, dir, lb, ds, PV(p, dcTort), g, &bp);
        if (pl->nb + nbp - 1 > cap) {
            int nc = (pl->nb + nbp - 1) * 2;
            dseg_t *ns = (dseg_t *)ar_alloc(ar, nc * sizeof(dseg_t));
            if (pl->nb)
                memcpy(ns, pl->bsegs, pl->nb * sizeof(dseg_t));
            pl->bsegs = ns;
            cap = nc;
        }
        for (i = 0; i < nbp - 1; i++)
            dseg_add(&pl->bsegs[pl->nb++], bp + 3 * i, bp + 3 * (i + 1), 0.35, Rc, r0, ds);
    }
    pl->rMin = INFINITY;
    pl->rMax = 0;
    for (i = 0; i < pl->ns + pl->nb; i++) {
        const dseg_t *s = i < pl->ns ? &pl->segs[i] : &pl->bsegs[i - pl->ns];
        if (s->r < pl->rMin) pl->rMin = s->r;
        if (s->r > pl->rMax) pl->rMax = s->r;
    }
    for (i = 0; i < pl->ns; i++)
        rSum += pl->segs[i].r;
    pl->El = El;
    pl->Rc = Rc;
    pl->Tn = Tn;
    pl->rMean = rSum / pl->ns;
    pl->tFirst = pl->rMin / C_AIR;
    pl->tLast = pl->rMax / C_AIR;
    pl->dev = maxd(d, 0.3);
    pl->tC = PV(p, dcCharge) > 0.05 ? PV(p, dcCharge) : 0;
}

typedef struct {
    arena *ar;
    const dplan_t *pl;
    rng_t *r;
    int len;
    double lr;
    float *bL[6], *bR[6];
    double br[6];
    int bn[6];
} dstrike_t;

/* one stroke along a list of stretches, into the distance bins or into the buffers given */
static void dstrike(dstrike_t *st, const dseg_t *segs, const int *idx, int n, double tS, double gain, double tnS, float *bl,
                    float *br, int blen)
{
    const dplan_t *pl = st->pl;
    int i;
    for (i = 0; i < n; i++) {
        const dseg_t *s = &segs[idx ? idx[i] : i];
        float *L = bl, *R = br;
        int len = blen;
        double q1, pk, q2, T, ts;
        if (!L) {
            int bi = (int)mind(5, floor(log(s->r / pl->rMin) / st->lr * 6));
            if (!st->bL[bi]) {
                st->bL[bi] = fnew(st->ar, st->len);
                st->bR[bi] = fnew(st->ar, st->len);
            }
            L = st->bL[bi];
            R = st->bR[bi];
            st->br[bi] += s->r;
            st->bn[bi]++;
            len = st->len;
        }
        q1 = rnd(st->r);
        pk = s->pk * gain * (0.6 + 0.8 * q1);
        q2 = rnd(st->r);
        T = pl->Tn * tnS * (0.8 + 0.45 * q2);
        ts = s->t - pl->tFirst + tS + 0.001;
        add_n(L, len, ts, T, pk * cos((s->pan + 1) * PI / 4));
        add_n(R, len, ts, T, pk * sin((s->pan + 1) * PI / 4));
    }
}

static void discharge_layers(scene_t *sc, const dplan_t *pl, const be_params *p, const gains_t *o, rng_t *r)
{
    static const double sg[5] = { 1, 0.7, 0.55, 0.45, 0.4 };
    arena *ar = sc->ar;
    double tC = pl->tC, t0 = o->t0, span = pl->tLast - pl->tFirst, tS = 0, pkMain = 0;
    int i, b, strokes = (int)clampd(js_round(PV(p, dcStrokes)), 1, 5);
    dstrike_t st;
    memset(&st, 0, sizeof st);
    st.ar = ar;
    st.pl = pl;
    st.r = r;
    st.lr = log(pl->rMax / pl->rMin);
    if (!st.lr) st.lr = 1;
    st.len = (int)ceil((span + pl->Tn * 4 + 0.4) * FS);
    for (i = 0; i < strokes; i++) {
        dstrike(&st, pl->segs, 0, pl->ns, tS, sg[i], 1, 0, 0, 0);
        if (!i)
            dstrike(&st, pl->bsegs, 0, pl->nb, 0, 1, 0.7, 0, 0, 0);
        tS += 0.03 + 0.05 * rnd(r);
    }
    for (b = 0; b < 6; b++) {
        double pk;
        if (!st.bL[b])
            continue;
        pk = peak_of(st.bL[b], st.bR[b], st.len);
        if (pk > pkMain) pkMain = pk;
        air_filter(st.bL[b], st.len, st.br[b] / st.bn[b]);
        air_filter(st.bR[b], st.len, st.br[b] / st.bn[b]);
        if (o->g1 > 0)
            push_stereo(sc, st.bL[b], st.bR[b], st.len, t0 + tC + pl->tFirst - 0.001, o->g1);
    }
    push_event(sc, BE_EVENT_CRACK, t0 + tC + pl->tFirst);
    if (PV(p, dcSustain) > 0.05 && o->g2 > 0) {
        /* a sustained arc re-strikes along the same channel; plasma hiss follows the re-strike rate */
        double sus = PV(p, dcSustain), rate = PV(p, dcRate), t = 0.02 + tS, pb;
        int bn = (int)ceil((sus + span + 0.15) * FS), step = (int)ceil(pl->ns / 40.0), nsub = 0, hn, k;
        float *bl = fnew(ar, bn), *br = fnew(ar, bn), *hs;
        int *sub = (int *)ar_alloc(ar, pl->ns * sizeof(int));
        for (k = 0; k < pl->ns; k++)
            if (k % step == 0)
                sub[nsub++] = k;
        while (t < sus) {
            double q = rnd(r);
            dstrike(&st, pl->segs, sub, nsub, t, 0.5 * (1 - 0.5 * t / sus) * (0.4 + q), 0.6, bl, br, bn);
            q = rnd(r);
            t += (1 + 0.3 * (q - 0.5)) / rate;
        }
        hn = (int)ceil(sus * FS);
        hs = fnew(ar, hn);
        for (i = 0; i < hn; i++) {
            double w = rnd(r) * 2 - 1;
            hs[i] = (float)(w * (0.5 + 0.5 * fabs(fsin(0.5 * rate * i / FS))) * (1 - 0.6 * i / hn));
        }
        one_pole_hp(hs, hn, 2500);
        one_pole_lp(hs, hn, 9000);
        pb = peak_of(bl, br, bn);
        to_peak(hs, hn, 0.2 * (pb ? pb : pkMain));
        for (i = 0; i < hn; i++) {
            bl[i] = (float)(bl[i] + hs[i]);
            br[i] = (float)(br[i] + hs[i]);
        }
        air_filter(bl, bn, pl->rMean);
        air_filter(br, bn, pl->rMean);
        push_stereo(sc, bl, br, bn, t0 + tC + pl->tFirst, o->g2);
    }
    if (tC > 0 && o->g3 > 0) {
        int cn;
        float *cb = charge_whine(ar, tC, PV(p, dcChargeHz), &cn);
        to_peak(cb, cn, 0.12 * pkMain);
        air_filter(cb, cn, pl->dev);
        push_mono(sc, cb, cn, t0 + pl->dev / C_AIR, 0, o->g3);
        push_event(sc, BE_EVENT_PASS, t0 + pl->dev / C_AIR);
    }
    if (PV(p, dcKick) > 0.01 && o->g4 > 0) {
        /* magnetic forces jolt the conductors, and the circuit rings down at its LC frequency */
        int kn, rn = (int)ceil(0.7 * FS), rl;
        float *kb = modal_hit(ar, be_material_find("steel"), 1, 0.6, r, 0, 0, 0, &kn), *rb;
        double ring = PV(p, dcRingHz), tau = 14 / (PI * ring);
        to_peak(kb, kn, 0.3 * pkMain * PV(p, dcKick));
        rl = rn + 2400;
        rb = fnew(ar, rl);
        for (i = 0; i < rn; i++) {
            double tt = i / FS;
            rb[i] = (float)(exp(-tt / tau) * (fsin(ring * tt) + 0.3 * fsin(2 * ring * tt)));
        }
        to_peak(rb, rl, 0.2 * pkMain * PV(p, dcKick));
        for (i = 0; i < kn && i < rl; i++)
            rb[i] = (float)(rb[i] + kb[i]);
        air_filter(rb, rl, pl->dev);
        push_mono(sc, rb, rl, t0 + tC + pl->dev / C_AIR, 0, o->g4);
    }
}

/* ------------------------------------------------------------------ destruction and implosion */
static const be_break *break_find(const char *id)
{
    int i;
    for (i = 0; i < BREAK_COUNT; i++)
        if (!strcmp(BREAKS[i].id, id))
            return &BREAKS[i];
    return 0;
}

typedef struct { double t, E, v; } dhit_t;
typedef struct { double m, h, t0, x, r, pan, sc; dhit_t hits[4]; int nh; } frag2_t;
typedef struct { double t, E, sc, x; } crk_t;
typedef struct {
    frag2_t *frs;
    int n;
    crk_t *cracks;
    int ncr;
    double Tfrac, E, tEnd, tFirst, tMed, Emax, big, baseSc, scMed;
} dsplan_t;

static void sort_doubles(double *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        double x = a[i];
        for (j = i - 1; j >= 0 && a[j] > x; j--)
            a[j + 1] = a[j];
        a[j + 1] = x;
    }
}

/* Break-up plan: power-law piece masses, release spread over the fracture time, free fall, bounces by restitution. */
static void plan_destruction(arena *ar, const char *mat, double mass, double height, double pieces, double dist, double spread,
                             rng_t *r, dsplan_t *pl)
{
    const be_break *d = break_find(mat);
    int n = (int)maxd(1, js_round(pieces)), i, b, nCr;
    double *ws = dnew(ar, n), sum = 0, g = 9.81, Tfrac, tEnd = 0, tFirst = INFINITY, Emax = 0, big = 0, bs;
    double *lands = dnew(ar, n), *scs = dnew(ar, n);
    for (i = 0; i < n; i++) {
        double w = pow(rnd(r) + 0.03, -0.9);
        ws[i] = w;
        sum += w;
    }
    Tfrac = (0.04 + 0.25 * log10(1 + mass)) * (1.3 - d->brittle * 0.6);
    pl->frs = (frag2_t *)ar_alloc(ar, n * sizeof(frag2_t));
    for (i = 0; i < n; i++) {
        frag2_t *f = &pl->frs[i];
        double m = mass * 0.85 * ws[i] / sum, q1 = rnd(r), h = height * (0.15 + 0.85 * q1), q2 = rnd(r), t0 = Tfrac * pow(q2, 1.5);
        double v = sqrt(2 * g * h), q3 = rnd(r), x = spread * (q3 * 2 - 1), rr = maxd(1, js_hypot(dist, x)), tt = t0 + v / g, vb = v;
        f->nh = 1;
        f->hits[0].t = tt;
        f->hits[0].E = m * g * h;
        f->hits[0].v = v;
        for (b = 0; b < 3; b++) {
            double dt;
            vb *= d->e * (0.7 + 0.6 * rnd(r));
            dt = 2 * vb / g;
            if (dt < 0.03)
                break;
            tt += dt;
            f->hits[f->nh].t = tt;
            f->hits[f->nh].E = 0.5 * m * vb * vb;
            f->hits[f->nh].v = vb;
            f->nh++;
        }
        if (tt > tEnd) tEnd = tt;
        if (f->hits[0].t < tFirst) tFirst = f->hits[0].t;
        if (f->hits[0].E > Emax) Emax = f->hits[0].E;
        if (m > big) big = m;
        lands[i] = f->hits[0].t;
        f->m = m; f->h = h; f->t0 = t0; f->x = x; f->r = rr; f->pan = x / rr * 0.9;
        f->sc = clampd(pow(0.2 / m, 1.0 / 3), 0.25, 5);
        scs[i] = f->sc;
    }
    sort_doubles(lands, n);
    sort_doubles(scs, n);
    nCr = (int)js_round(clampd(n * 1.5 * d->brittle + 6, 6, 160));
    bs = clampd(pow(0.2 / mass, 1.0 / 3), 0.25, 5);
    pl->cracks = (crk_t *)ar_alloc(ar, nCr * sizeof(crk_t));
    for (i = 0; i < nCr; i++) {
        crk_t *c = &pl->cracks[i];
        double q1 = rnd(r), q2, q3, q4;
        c->t = Tfrac * pow(q1, 2);
        q2 = rnd(r);
        c->E = mass * g * height * 0.002 * (0.2 + q2);
        q3 = rnd(r);
        c->sc = mind(6, bs * (1.5 + 3 * q3));
        q4 = rnd(r);
        c->x = spread * (q4 - 0.5);
    }
    pl->n = n;
    pl->ncr = nCr;
    pl->Tfrac = Tfrac;
    pl->E = mass * g * height;
    pl->tEnd = tEnd;
    pl->tFirst = tFirst;
    pl->tMed = lands[n >> 1];
    pl->Emax = Emax;
    pl->big = big;
    pl->baseSc = bs;
    pl->scMed = scs[n >> 1] ? scs[n >> 1] : 1;
}

/* Stick-slip before failure: a jittered slip train that speeds up toward the break and rings the body's modes. */
static float *groan_buf(arena *ar, const char *type, const be_material *M, double sc, double dur, rng_t *r, int *len_out)
{
    int N = (int)ceil(dur * FS), tot = N + (int)ceil(0.4 * FS), k, n, K = 0, creak = !strcmp(type, "creak");
    float *buf = fnew(ar, tot + 64);
    double base = !strcmp(type, "groan") ? 0.5 : creak ? 0.06 : !strcmp(type, "rumble") ? 0.08 : 0.04;
    double rate0 = !strcmp(type, "groan") ? 28 : creak ? 140 : !strcmp(type, "rumble") ? 16 : 3;
    double c1[5], c2[5], y1[5] = { 0 }, y2[5] = { 0 }, gg[5], ph;
    for (k = 0; k < (M->n_ratios < 5 ? M->n_ratios : 5); k++) {
        double q = rnd(r), f = M->f0 * sc * M->ratios[k] * (1 + (q - 0.5) * 0.1), R;
        if (f > 12000)
            break;
        R = pow(10, -3 / (base / (1 + 0.3 * k) * FS));
        c1[K] = 2 * R * cos(TAU * f / FS);
        c2[K] = R * R;
        gg[K] = 1.0 / (1 + k);
        K++;
    }
    ph = rnd(r);
    for (n = 0; n < tot; n++) {
        double x = 0, u = mind(1, (double)n / N), ts = n / FS, y = 0, v;
        if (n < N) {
            double gate = creak ? (sin(TAU * 1.3 * ts + 2 * sin(TAU * 0.37 * ts)) > -0.2 ? 1 : 0) : 1;
            ph += rate0 * (1 + 2.5 * u * u * u) / FS;
            if (ph >= 1) {
                double q1 = rnd(r), q2;
                ph -= 1 + (q1 - 0.5) * 0.3;
                q2 = rnd(r);
                x = gate * (0.5 + q2) * (0.25 + 0.75 * u);
            }
        }
        for (k = 0; k < K; k++) {
            v = c1[k] * y1[k] - c2[k] * y2[k] + x * gg[k];
            y2[k] = y1[k];
            y1[k] = v;
            y += v;
        }
        buf[n] = (float)y;
    }
    if (!strcmp(type, "rumble")) {
        float *nz = shaped_noise(ar, tot, r, 160, 20);
        double pk = peak_of(buf, 0, tot + 64);
        to_peak(nz, tot, (pk ? pk : 1) * 0.8);
        for (n = 0; n < tot; n++) {
            double uu = mind(1, (double)n / N);
            buf[n] = (float)(buf[n] + nz[n] * uu * uu * (n < N ? 1 : exp(-(n - N) / FS / 0.08)));
        }
    }
    *len_out = tot + 64;
    return buf;
}

static void fragment_layers(scene_t *sc, const dsplan_t *pl, const be_material *mat, int steel, double t0, double gain,
                            double pan, rng_t *r, const grains_t *grains)
{
    int i, b, len;
    for (i = 0; i < pl->n; i++) {
        const frag2_t *f = &pl->frs[i];
        for (b = 0; b < f->nh; b++) {
            const dhit_t *h = &f->hits[b];
            double ts = clampd(pow(f->sc, -0.6), 0.25, steel ? 0.7 : 1.5) * (b ? 0.8 : 1);
            float *hb = grains ? grain_hit(sc->ar, grains, h->E, f->sc, pl->scMed, r, &len)
                               : modal_hit(sc->ar, mat, h->E, f->sc, r, f->m > 1 ? 6 : 4, ts, 1, &len);
            scale(hb, len, 1 / f->r);
            air_filter(hb, len, f->r);
            push_mono(sc, hb, len, t0 + h->t + f->r / C_AIR, clampd(f->pan + pan, -1, 1), gain);
        }
    }
}

static void destruction_layers(scene_t *sc, const dsplan_t *pl, const be_params *p, const gains_t *o, rng_t *r,
                               const grains_t *grains)
{
    arena *ar = sc->ar;
    const char *matn = PS(p, dsMat);
    const be_material *mat = be_material_find(matn);
    const be_break *d = break_find(matn);
    double rr = maxd(1, PV(p, dsDist)), tA = o->t0 + rr / C_AIR, trigPk = 6 * mat->eff * sqrt(pl->E * 0.25) / rr;
    int i, k, len;
    float *buf;
    if (o->g1 > 0) {
        buf = modal_hit(ar, mat, pl->E * 0.25, pl->baseSc, r, 0, 0, 0, &len);
        scale(buf, len, 1 / rr);
        post_layer(ar, buf, len, rr, o->ground);
        push_mono(sc, buf, len, tA, o->pan, o->g1);
        for (i = 0; i < pl->ncr; i++) {
            const crk_t *c = &pl->cracks[i];
            int cl;
            float *cb = modal_hit(ar, mat, c->E, c->sc, r, 3, 0.4, 1, &cl);
            double rc = js_hypot(rr, c->x);
            scale(cb, cl, 1 / rc);
            air_filter(cb, cl, rc);
            push_mono(sc, cb, cl, o->t0 + c->t + rc / C_AIR, clampd(o->pan + c->x / rc * 0.9, -1, 1), o->g1);
        }
    }
    push_event(sc, BE_EVENT_CRACK, tA);
    if (o->g2 > 0 && PV(p, dsGroan) > 0.05) {
        buf = groan_buf(ar, d->groan, mat, pl->baseSc, PV(p, dsGroan), r, &len);
        to_peak(buf, len, trigPk * 0.2);
        air_filter(buf, len, rr);
        push_mono(sc, buf, len, tA - PV(p, dsGroan), o->pan, o->g2);
        push_event(sc, BE_EVENT_PASS, tA - PV(p, dsGroan));
    }
    if (o->g4 > 0)
        fragment_layers(sc, pl, mat, !strcmp(matn, "steel"), o->t0, o->g4, o->pan, r, grains);
    push_event(sc, BE_EVENT_IMPACT, tA + pl->tFirst);
    if (o->g3 > 0) {
        /* fine debris scattered by each first landing */
        for (i = 0; i < pl->n; i++) {
            const frag2_t *f = &pl->frs[i];
            for (k = 0; k < d->fine; k++) {
                double q1 = rnd(r), fe = f->hits[0].E * 0.01 * (0.2 + q1), q2 = rnd(r), fsc = mind(6, f->sc * (2 + 3 * q2)), q3, q4;
                float *fb = grains ? grain_hit(ar, grains, fe, fsc, pl->scMed, r, &len) : modal_hit(ar, mat, fe, fsc, r, 3, 0.35, 1, &len);
                scale(fb, len, 1 / f->r);
                air_filter(fb, len, f->r);
                q3 = rnd(r);
                q4 = rnd(r);
                push_mono(sc, fb, len, o->t0 + f->hits[0].t + 0.02 + 0.33 * q3 + f->r / C_AIR,
                          clampd(f->pan + o->pan + (q4 - 0.5) * 0.3, -1, 1), o->g3);
            }
        }
        if (d->rubble) {
            int n = (int)ceil((pl->tEnd + 1.2) * FS);
            float *env = fnew(ar, n), *nz = shaped_noise(ar, n, r, 1400, 90);
            double s = 0, a = exp(-1 / (0.25 * FS));
            for (i = 0; i < pl->n; i++) {
                int ix = (int)floor(pl->frs[i].hits[0].t * FS);
                if (ix < n)
                    env[ix] = (float)(env[ix] + sqrt(pl->frs[i].hits[0].E));
            }
            for (i = 0; i < n; i++) {
                s = s * a + env[i];
                nz[i] = (float)(nz[i] * s);
            }
            one_pole_lp(nz, n, 3000);
            to_peak(nz, n, trigPk * 0.25);
            air_filter(nz, n, rr);
            push_mono(sc, nz, n, tA, o->pan, o->g3);
        }
    }
}

/* Rayleigh-Plesset collapse of a spherical cavity with residual gas and acoustic radiation damping. Far-field
   pressure is rho * d/dt(R^2 dR/dt) / r. Exact for liquids; for air it is an approximation. */
typedef struct { double R0, tc, tMin, Rmin, c, rho, r; int water, N; double *Q; } implo_t;

static void plan_implosion(arena *ar, int water, double imDp, double imGas, double imR0, double imDist, implo_t *pl)
{
    double rho = water ? 1000 : RHO, c = water ? 1480 : C_AIR, gam = 1.4, pInf = imDp * P0, pg0 = imGas * pInf, R0 = imR0;
    double tc = 0.915 * R0 * sqrt(rho / (pInf - pg0)), Tsim = mind(1.6, tc * 14 + 0.02);
    int N = (int)ceil(Tsim * FS) + 2, n = 0, steps = 0;
    double *Q = dnew(ar, N), R = R0, U = 0, t = 0, Rmin = R0, tMin = 0, dtMax = 1 / (2 * FS), uMax = (water ? 2.5 : 1.2) * c;
    double flo = 1e-5 * R0, damp = (water ? 0.6 : 0.8) / tc;
#define BE_ACC(Rx, Ux, res) do { double pg_ = pg0 * pow(R0 / (Rx), 3 * gam); \
        res = ((pg_ - pInf) / rho - 1.5 * (Ux) * (Ux) - 3 * gam * pg_ * (Ux) / (rho * c)) / (Rx); } while (0)
    while (t < Tsim && steps < 1200000) {
        double dt = mind(dtMax, 0.004 * R / (fabs(U) + 1e-6)), a1, Rm, Um, Rn, Un, tn, a2;
        if (dt < 1e-10) dt = 1e-10;
        BE_ACC(R, U, a1);
        Rm = maxd(flo, R + U * dt / 2);
        Um = U + a1 * dt / 2;
        Rn = maxd(flo, R + Um * dt);
        BE_ACC(Rm, Um, a2);
        Un = clampd((U + a2 * dt) * (1 - damp * dt), -uMax, uMax);
        tn = t + dt;
        while (n < N && n / FS <= tn) {
            double f = (n / FS - t) / dt, Ri = R + (Rn - R) * f, Ui = U + (Un - U) * f;
            Q[n++] = Ri * Ri * Ui;
        }
        if (Rn < Rmin) { Rmin = Rn; tMin = tn; }
        R = Rn; U = Un; t = tn; steps++;
    }
#undef BE_ACC
    for (; n < N; n++)
        Q[n] = R * R * U;
    pl->R0 = R0; pl->tc = tc; pl->tMin = tMin; pl->Rmin = Rmin; pl->c = c; pl->rho = rho; pl->water = water;
    pl->N = N; pl->Q = Q; pl->r = maxd(imDist, R0 * 2);
}

static void implosion_layers(scene_t *sc, const implo_t *pl, const be_params *p, const gains_t *o, rng_t *r)
{
    arena *ar = sc->ar;
    double rr = pl->r, tA = o->t0 + rr / pl->c, pk, scl, eff;
    int n = pl->N - 1, i, len = n + 2400, cnt;
    float *buf = fnew(ar, len);
    const char *shell = PS(p, imShell);
    const be_material *M;
    for (i = 0; i < n; i++)
        buf[i] = (float)(pl->rho * (pl->Q[i + 1] - pl->Q[i]) * FS / rr);
    pk = peak_of(buf, 0, len);
    if (!pk) pk = 1;
    if (o->g1 > 0) {
        if (pl->water) one_pole_lp(buf, len, 9000);
        else post_layer(ar, buf, len, rr, o->ground);
        push_mono(sc, buf, len, tA, o->pan, o->g1);
    }
    push_event(sc, BE_EVENT_CRACK, tA + pl->tMin);
    if (!strcmp(shell, "none"))
        return;
    /* shell layers are levelled against the collapse pulse rather than measured */
    M = be_material_find(shell);
    scl = clampd(pow(0.15 / pl->R0, 0.8), 0.2, 4);
    eff = 6 * M->eff;
#define BE_PATH(b, bn) do { if (pl->water) one_pole_lp(b, bn, 9000); else air_filter(b, bn, rr); } while (0)
    if (o->g2 > 0 && PV(p, imGroan) > 0.05) {
        int gn;
        float *gb = groan_buf(ar, break_find(shell)->groan, M, scl, PV(p, imGroan), r, &gn);
        to_peak(gb, gn, pk * 0.05);
        BE_PATH(gb, gn);
        push_mono(sc, gb, gn, tA - PV(p, imGroan), o->pan, o->g2);
        push_event(sc, BE_EVENT_PASS, tA - PV(p, imGroan));
    }
    if (o->g3 > 0) {
        int fn;
        double win;
        float *fb = modal_hit(ar, M, 1, scl, r, 0, 0, 0, &fn);
        to_peak(fb, fn, pk * 0.3);
        BE_PATH(fb, fn);
        push_mono(sc, fb, fn, tA, o->pan, o->g3);
        cnt = !strcmp(shell, "steel") ? 14 : 24;
        win = maxd(pl->tMin * 1.8, 0.02);
        for (i = 0; i < cnt; i++) {
            double q1 = rnd(r), q2, q3, q4;
            int cl;
            float *cb = modal_hit(ar, M, 1, scl * (1.2 + 2.5 * q1), r, 4, 0.5, 1, &cl);
            q2 = rnd(r);
            to_peak(cb, cl, pk * 0.12 * (0.3 + q2));
            BE_PATH(cb, cl);
            q3 = rnd(r);
            q4 = rnd(r);
            push_mono(sc, cb, cl, tA + win * q3, clampd(o->pan + (q4 - 0.5) * 0.6, -1, 1), o->g3);
        }
    }
#undef BE_PATH
    if (o->g4 > 0 && !pl->water) {
        double mass = 4 * PI * pl->R0 * pl->R0 * 0.003 * break_find(shell)->shell_density, big;
        dsplan_t dp;
        plan_destruction(ar, shell, mass, 1, !strcmp(shell, "glass") ? 30 : 8, rr, pl->R0 * 4, r, &dp);
        big = eff * sqrt(dp.Emax) / rr;
        fragment_layers(sc, &dp, M, !strcmp(shell, "steel"), o->t0 + pl->tMin, o->g4 * pk * 0.15 / big, o->pan, r, 0);
        push_event(sc, BE_EVENT_IMPACT, tA + pl->tMin + dp.tFirst);
    }
}

/* ------------------------------------------------------------------ forcefield: a driven sheet of modes, or a plasma */
#define FF_HUM 2.0
#define LOOP_S 4
#define MU0 (4e-7 * PI)
#define MPROT 1.6726e-27

enum { FF_HUMMODE, FF_IMPACT, FF_POWERUP, FF_POWERDOWN };
typedef struct { double f, r, a, ph, lf, lp; int fam; } fmode_t;
typedef struct {
    double f0, top, dur, tHit, r, beat, sawT;
    fmode_t *modes;
    int nm, mode, plasma;
} fplan_t;

static int field_mode(const char *s)
{
    if (!strcmp(s, "hum")) return FF_HUMMODE;
    if (!strcmp(s, "impact")) return FF_IMPACT;
    if (!strcmp(s, "powerup")) return FF_POWERUP;
    if (!strcmp(s, "powerdown")) return FF_POWERDOWN;
    return -1;
}

static void fmk(fplan_t *pl, double f, double r, double a, int fam, rng_t *g)
{
    fmode_t *m = &pl->modes[pl->nm++];
    m->f = f; m->r = r; m->a = a; m->fam = fam;
    m->ph = rnd(g);
    m->lf = 1 + floor(rnd(g) * 9);
    m->lp = rnd(g);
    if (f > pl->top)
        pl->top = f;
}

static void plan_field(arena *ar, const be_params *p, rng_t *g, fplan_t *pl)
{
    int k, fam;
    memset(pl, 0, sizeof *pl);
    pl->mode = field_mode(PS(p, ffMode));
    pl->plasma = !strcmp(PS(p, ffType), "plasma");
    pl->f0 = PV(p, ffBase);
    pl->modes = (fmode_t *)ar_alloc(ar, 40 * sizeof(fmode_t));
    if (pl->plasma) {
        /* Alfven speed B / sqrt(mu0 rho): pitch rises with field strength and falls with density */
        double th, cs2, c2, sum, disc, rs[3];
        static const double wt[3] = { 0.55, 1, 0.8 };
        pl->f0 = clampd(PV(p, ffBase) * PV(p, ffB) * sqrt(1e19 / PV(p, ffDens)), 15, 800);
        th = PV(p, ffAngle) * PI / 180;
        cs2 = 5.0 / 6 * PV(p, ffBeta);
        c2 = cos(th) * cos(th);
        sum = 1 + cs2;
        disc = sqrt(maxd(0, sum * sum - 4 * cs2 * c2));
        rs[0] = sqrt(maxd(1e-4, (sum - disc) / 2));
        rs[1] = maxd(0.02, cos(th));
        rs[2] = sqrt((sum + disc) / 2);
        for (fam = 0; fam < 3; fam++)
            for (k = 1; k <= 6; k++) {
                double q = rnd(g), f = pl->f0 * rs[fam] * k * (1 + (q - 0.5) * 0.004);
                if (f >= 18 && f <= 9000) {
                    double q2 = rnd(g);
                    fmk(pl, f, rs[fam] * k, wt[fam] * (0.7 + 0.6 * q2) / pow(k, 1.2), fam, g);
                }
            }
        /* surface ripple answers at half the drive frequency (Faraday waves) */
        if (PV(p, ffSurface) > 0.01)
            for (k = 1; k <= 10; k++) {
                double fs = pl->f0 / 2 * k - 0.35 * pow(k, 1.5);
                if (fs >= 18) {
                    double q2 = rnd(g);
                    fmk(pl, fs, k / 2.0, PV(p, ffSurface) * 0.7 * (0.7 + 0.6 * q2) / k, 3, g);
                }
            }
        if (!pl->nm)
            fmk(pl, pl->f0, 1, 1, 1, g);
    } else {
        const be_field *rat = 0;
        int i;
        for (i = 0; i < FIELD_COUNT; i++)
            if (!strcmp(FIELDS[i].id, PS(p, ffType)))
                rat = &FIELDS[i];
        if (!rat)
            rat = &FIELDS[0];
        for (k = 0; k < rat->n; k++) {
            double q = rnd(g), fm = pl->f0 * rat->r[k] * (1 + (q - 0.5) * 0.012), q2;
            if (fm > 9000)
                break;
            q2 = rnd(g);
            fmk(pl, fm, rat->r[k], (0.6 + 0.8 * q2) / pow(1 + k, 1.1), k ? 2 : 1, g);
        }
    }
    pl->dur = pl->mode == FF_HUMMODE ? LOOP_S : pl->mode == FF_IMPACT ? 4.2 : 3.6;
    pl->tHit = pl->mode == FF_IMPACT || pl->mode == FF_POWERDOWN ? 0.9 : 0;
    pl->r = maxd(0.5, PV(p, ffDist));
    pl->beat = PV(p, ffBeat);
    pl->sawT = pl->plasma && PV(p, ffSaw) > 0.01 ? clampd(PV(p, ffSawT), 0.2, 6) : 0;
}

/* gliding tone with a sin^2 envelope; curve < 0 gives the 1/t^2 fall of a whistler */
static void add_tone(float *mid, float *side, int ns, double t0, double len, double fA, double fB, double amp, double pan,
                     int periodic, double curve)
{
    int n0 = (int)js_round(t0 * FS), n = (int)js_round(len * FS), i;
    double ph = 0, q = sqrt(fA / fB) - 1;
    for (i = 0; i < n; i++) {
        double u = (double)i / n, f = curve < 0 ? fA / pow(1 + u * q, 2) : fA + (fB - fA) * pow(u, curve), e, s;
        int idx = n0 + i;
        ph += f / FS;
        if (idx >= ns) {
            if (!periodic)
                break;
            idx -= ns;
        }
        e = sin(PI * u);
        s = amp * e * e * fsin(ph);
        mid[idx] = (float)(mid[idx] + s);
        side[idx] = (float)(side[idx] + s * pan);
    }
}

/* a soliton passing the listener: sech^2 pulse, taller ones are narrower and brighter */
static void add_soliton(float *mid, float *side, int ns, double tc, double A, double f0, double amp, int periodic, rng_t *r)
{
    double w = 0.09 / sqrt(A), f = f0 * (2 + 2 * A), lp = 0;
    int n = (int)js_round(w * 8 * FS), n0 = (int)js_round((tc - 4 * w) * FS), i;
    for (i = 0; i < n; i++) {
        double x = (i / FS - 4 * w) / w, sech = 1 / cosh(x), q = rnd(r), s;
        int idx = n0 + i;
        lp += ((q * 2 - 1) - lp) * 0.12;
        if (idx < 0) {
            if (!periodic)
                continue;
            idx += ns;
        } else if (idx >= ns) {
            if (!periodic)
                break;
            idx -= ns;
        }
        s = amp * A * sech * sech * (0.7 * fsin(f * i / FS) + 1.6 * lp);
        mid[idx] = (float)(mid[idx] + s);
        side[idx] = (float)(side[idx] + s * tanh(x) * 0.8);
    }
}

static void spark_cluster(float *mid, float *side, int ns, double t0, int count, double spread, double amp, rng_t *r)
{
    int k;
    for (k = 0; k < count; k++) {
        double q1 = rnd(r), ts = t0 + 0.001 - log(q1 + 1e-6) * spread, q2 = rnd(r), pk = amp * (0.15 + 0.85 * q2);
        double q3 = rnd(r), T = 0.00008 + 0.00022 * q3, q4 = rnd(r), pn = (q4 * 2 - 1) * 0.7;
        if (ts + T < ns / FS - 0.001) {
            add_n(mid, ns, ts, T, pk);
            add_n(side, ns, ts, T, pk * pn);
        }
    }
}

static double field_signal(arena *ar, const fplan_t *pl, const be_params *p, const gains_t *o, rng_t *r, int ns, int periodic,
                           float **mid_out, float **side_out)
{
    double Lp = ns / FS, Ls = periodic ? Lp : 4, e = 0, pm = 0, saw = pl->sawT ? PV(p, ffSaw) : 0, Ts = pl->sawT ? pl->sawT : 1;
    double kph = 0, td = -1, norm, fc0, *fa, *fb, *pa, *pb, *crashes;
    int M = pl->nm, k, i, nc = 0;
    float *mid = fnew(ar, ns), *side = fnew(ar, ns), *gA = fnew(ar, ns), *pA = fnew(ar, ns), *hz = 0;
    if (periodic && saw)
        Ts = Lp / maxd(1, js_round(Lp / Ts));
    /* level and pitch envelopes first, so loop frequencies can be fitted to the average pitch */
    for (i = 0; i < ns; i++) {
        double t = i / FS, g = 1, pf = 1, dt;
        int falling = pl->mode == FF_POWERDOWN && t > pl->tHit;
        if (pl->mode == FF_IMPACT && t > pl->tHit) {
            dt = t - pl->tHit;
            g = 1 + 1.6 * exp(-dt / 0.12) - 0.45 * exp(-dt / 0.9) * (1 - exp(-dt / 0.15));
            pf = 1 + 0.04 * exp(-dt / 0.25);
        } else if (pl->mode == FF_POWERUP) {
            g = mind(1, pow(t / 1.6, 1.5)) * (1 + 0.25 * exp(-pow((t - 1.6) / 0.25, 2)));
            pf = 0.5 + 0.5 * (1 - exp(-t / 0.45));
        } else if (falling) {
            dt = t - pl->tHit;
            if (pl->plasma) {
                /* kink instability: a helical wobble that grows exponentially until the column disrupts */
                double d = 0.05 * exp(dt / 0.35);
                if (d < 1) {
                    kph += (5 + 9 * d) / FS;
                    g = 1 + 0.7 * d * fsin(kph);
                    pf = 1 + 0.09 * d * fsin(kph + 0.25);
                } else {
                    if (td < 0) td = t;
                    g = 1.7 * exp(-(t - td) / 0.12);
                    pf = exp(-(t - td) / 0.3);
                }
            } else {
                g = exp(-dt / 0.55);
                pf = 0.25 + 0.75 * exp(-dt / 0.8);
            }
        }
        if (saw && !falling) {
            /* reconnection cycle: stress builds along a ramp, then the field snaps back in a few milliseconds */
            double s = fmod(t / Ts, 1), eps = 0.004 / Ts, st = s < 1 - eps ? s : (1 - eps) * (1 - (s - (1 - eps)) / eps);
            pf *= 1 + 0.05 * saw * (st - 0.5);
            g *= 1 + saw * (0.3 * st - 0.2);
        }
        gA[i] = (float)g;
        pA[i] = (float)pf;
        pm += pf;
    }
    pm /= ns;
    crashes = dnew(ar, (int)(Lp / Ts) + 2);
    if (saw)
        for (k = periodic ? 0 : 1; k * Ts < Lp - 0.06; k++)
            if (!(pl->mode == FF_POWERDOWN && k * Ts > pl->tHit))
                crashes[nc++] = k * Ts;
    fa = dnew(ar, M); fb = dnew(ar, M); pa = dnew(ar, M); pb = dnew(ar, M);
    for (k = 0; k < M; k++) {
        double f1 = pl->modes[k].f, f2 = f1 + pl->beat * pl->modes[k].r;
        if (periodic) {
            f1 = js_round(f1 * pm * Lp) / (pm * Lp);
            f2 = js_round(f2 * pm * Lp) / (pm * Lp);
        }
        fa[k] = f1;
        fb[k] = f2;
        pa[k] = pl->modes[k].ph;
        pb[k] = pl->modes[k].ph + 0.37;
        e += pl->modes[k].a * pl->modes[k].a;
    }
    norm = 1 / sqrt(e);
    if (o->g2 > 0 && PV(p, ffCorona) > 0) {
        float *w = fnew(ar, ns);
        double a1 = exp(-TAU * 2500 / FS), a2 = exp(-TAU * 9000 / FS), s1 = 0, s2 = 0, eh = 0, rmsv;
        int pass;
        hz = fnew(ar, ns);
        for (i = 0; i < ns; i++)
            w[i] = (float)(rnd(r) * 2 - 1);
        for (pass = periodic ? 0 : 1; pass < 2; pass++)
            for (i = 0; i < ns; i++) {
                double h;
                s1 = w[i] + (s1 - w[i]) * a1;
                h = w[i] - s1;
                s2 = h + (s2 - h) * a2;
                hz[i] = (float)s2;
            }
        for (i = 0; i < ns; i++)
            eh += (double)hz[i] * hz[i];
        rmsv = sqrt(eh / ns);
        scale(hz, ns, 1 / (rmsv ? rmsv : 1));
    }
    fc0 = periodic ? js_round(fa[0] * Lp) / Lp : fa[0];
    for (i = 0; i < ns; i++) {
        double tt = i / FS, A = 0, Bv = 0, gg = gA[i], pp = pA[i], hum, cor;
        for (k = 0; k < M; k++) {
            const fmode_t *md = &pl->modes[k];
            double sh = 1 + PV(p, ffShimmer) * 0.8 * ((double)k / M) * fsin(md->lf * tt / Ls + md->lp);
            pa[k] += fa[k] * pp / FS;
            pb[k] += fb[k] * pp / FS;
            A += md->a * sh * fsin(pa[k]);
            Bv += md->a * sh * fsin(pb[k]);
        }
        hum = o->g1 * FF_HUM * norm * gg;
        /* corona hisses twice per drive cycle */
        cor = hz ? o->g2 * PV(p, ffCorona) * 0.5 * FF_HUM * hz[i] * gg * (0.55 + 0.45 * fabs(fsin(fc0 * tt))) : 0;
        mid[i] = (float)(hum * (A + Bv) * 0.5 + cor);
        side[i] = (float)(hum * (A - Bv) * 0.35);
    }
    if (pl->plasma) {
        double live = pl->mode == FF_POWERDOWN ? (td > 0 ? td : pl->tHit + 1) : Lp;
        if (o->g4 > 0)
            for (k = 0; k < nc; k++) {
                double q = rnd(r);
                spark_cluster(mid, side, ns, crashes[k], 6 + (int)floor(8 * q), 0.008, o->g4 * FF_HUM * 1.5 * saw, r);
            }
        if (o->g2 > 0 && PV(p, ffChorus) > 0.01) {
            /* chorus: short rising tones, as whistler-mode emissions from a stressed magnetosphere */
            int ncr = (int)js_round(PV(p, ffChorus) * live * 4);
            for (k = 0; k < ncr; k++) {
                double q1 = rnd(r), fA = clampd(900 * sqrt(PV(p, ffB)) * (1 + 0.6 * q1), 300, 3500), q2 = rnd(r), len = 0.12 + 0.33 * q2;
                double q3 = rnd(r), t0 = q3 * (periodic ? Lp : maxd(0.1, live - len)), q4 = rnd(r), fB = mind(7000, fA * (1.6 + 0.8 * q4));
                double q5 = rnd(r), amp = o->g2 * FF_HUM * 0.18 * PV(p, ffChorus) * (0.3 + 0.7 * q5), q6 = rnd(r), pan = (q6 * 2 - 1) * 0.7;
                add_tone(mid, side, ns, t0, len, fA, fB, amp, pan, periodic, 1.3);
            }
        }
        if (o->g1 > 0 && PV(p, ffSoliton) > 0.01) {
            double sa = o->g1 * FF_HUM * 0.5 * PV(p, ffSoliton);
            int j;
            if (pl->mode == FF_IMPACT) {
                /* an impact launches solitons round the field; taller ones travel faster, so the returns drift apart */
                for (k = 0; k < 2 + js_round(PV(p, ffSoliton) * 3); k++) {
                    double Ak = 0.4 + 0.6 * rnd(r), Tk = 0.9 / (0.6 + Ak);
                    for (j = 1; pl->tHit + j * Tk < Lp - 0.5; j++)
                        add_soliton(mid, side, ns, pl->tHit + j * Tk, Ak * exp(-0.35 * (j - 1)), pl->f0, sa * 1.6, 0, r);
                }
            } else if (pl->mode == FF_HUMMODE) {
                for (k = 0; k < 1 + js_round(PV(p, ffSoliton) * 2); k++) {
                    double Ah = 0.5 + 0.5 * rnd(r), off;
                    int np = 2 + (int)js_round(Ah * 3) + k;
                    off = rnd(r);
                    for (j = 0; j < np; j++)
                        add_soliton(mid, side, ns, (j + off) * Lp / np, Ah, pl->f0, sa, periodic, r);
                }
            }
        }
    }
    *mid_out = mid;
    *side_out = side;
    return td;
}

static int dbl_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void field_layers(scene_t *sc, const fplan_t *pl, const be_params *p, const gains_t *o, rng_t *r)
{
    arena *ar = sc->ar;
    int ns = (int)ceil(pl->dur * FS), i, k, fi = (int)js_round(0.15 * FS), fo = (int)js_round(0.3 * FS);
    double rr = pl->r, tA = o->t0 + rr / C_AIR, td, E, hit, tEv;
    float *mid, *side, *L = fnew(ar, ns), *R = fnew(ar, ns);
    td = field_signal(ar, pl, p, o, r, ns, 0, &mid, &side);
    for (i = 0; i < ns; i++) {
        double fd = mind(mind(1, (double)i / fi), (double)(ns - i) / fo);
        L[i] = (float)((mid[i] + side[i]) / rr * fd);
        R[i] = (float)((mid[i] - side[i]) / rr * fd);
    }
    air_filter(L, ns, rr);
    air_filter(R, ns, rr);
    push_stereo(sc, L, R, ns, tA, 1);
    E = pl->mode == FF_IMPACT ? PV(p, ffHitE) : pl->mode == FF_POWERUP ? 100 : 30;
    hit = FF_HUM * pow(E, 0.25);
    tEv = pl->plasma && pl->mode == FF_POWERDOWN && td > 0 ? td : pl->tHit;
    if (pl->mode == FF_IMPACT && o->g3 > 0) {
        /* struck sheet: stretched modes start sharp and settle, the higher ones bloom late like a gong */
        int rn = (int)ceil(3.2 * FS), nf = 0, nk;
        float *rb = fnew(ar, rn + 64);
        double beta = PV(p, ffGlide) * 0.7, last = pl->top, fl[80], fk[80], ph[80] = { 0 }, en[80], at[80], dd[80], da[80], gl = 1, gd;
        for (k = 0; k < pl->nm; k++)
            fl[nf++] = pl->modes[k].f;
        for (k = 1; nf < 18 && last + k * 0.55 * pl->f0 < 9000; k++)
            fl[nf++] = last + k * 0.55 * pl->f0;
        qsort(fl, nf, sizeof(double), dbl_cmp);
        nk = 0;
        if (nf > 22) {
            for (k = 0; k < nf; k++)
                if (k % 2 == 0 || k < 6)
                    fk[nk++] = fl[k];
        } else {
            for (k = 0; k < nf; k++)
                fk[nk++] = fl[k];
        }
        for (k = 0; k < nk; k++) {
            en[k] = 1;
            at[k] = 1;
            dd[k] = exp(-6.9 / (2.5 / (1 + 0.25 * k) * FS));
            da[k] = exp(-1 / ((0.004 + 0.03 * k * PV(p, ffGlide)) * FS));
        }
        gd = exp(-1 / (0.22 * FS));
        for (i = 0; i < rn; i++) {
            double pf = sqrt(1 + beta * gl), y = 0;
            gl *= gd;
            for (k = 0; k < nk; k++) {
                ph[k] += fk[k] * pf / FS;
                en[k] *= dd[k];
                at[k] *= da[k];
                y += fsin(ph[k]) * en[k] * (1 - at[k]) / pow(1 + k, 0.6);
            }
            rb[i] = (float)y;
        }
        to_peak(rb, rn + 64, 0.6 * hit / rr);
        air_filter(rb, rn + 64, rr);
        push_mono(sc, rb, rn + 64, tA + pl->tHit, o->pan, o->g3);
    }
    if (o->g4 > 0) {
        int cn = (int)ceil(2.2 * FS), disrupt = pl->plasma && pl->mode == FF_POWERDOWN;
        int cnt = pl->mode == FF_IMPACT ? 34 : pl->mode == FF_POWERUP ? 18 : pl->mode == FF_POWERDOWN ? 26 : 0;
        if (cnt) {
            float *cm = fnew(ar, cn), *cs = fnew(ar, cn), *cl, *cr;
            if (pl->mode != FF_POWERDOWN || disrupt)
                add_n(cm, cn, 0.001, 0.00025, 1.2 * hit);
            spark_cluster(cm, cs, cn, 0.001, cnt, pl->mode == FF_POWERDOWN && !disrupt ? 0.4 : 0.05, 0.5 * hit, r);
            if (disrupt) {
                /* the disruption dumps the stored field energy: a low thud under the sparks */
                double tp = 0;
                int nn = (int)js_round(0.5 * FS);
                for (i = 0; i < nn; i++) {
                    double tq = i / FS;
                    tp += (30 + 70 * exp(-tq / 0.06)) / FS;
                    cm[i] = (float)(cm[i] + 2.2 * FF_HUM * fsin(tp) * exp(-tq / 0.16) * (1 - exp(-tq / 0.003)));
                }
            }
            if (pl->plasma && pl->mode == FF_IMPACT) {
                /* each spark sends a whistler: dispersion makes the highs arrive first, so the tone falls */
                for (k = 0; k < 3 + floor(3 * rnd(r)); k++) {
                    double q1 = rnd(r), q2 = rnd(r), q3 = rnd(r), q4 = rnd(r), q5 = rnd(r), q6 = rnd(r);
                    add_tone(cm, cs, cn, 0.15 + 0.5 * q1, 0.8 + 0.6 * q2, 5000 + 2000 * q3, 300 + 200 * q4,
                             0.12 * hit * (0.5 + 0.5 * q5), (q6 * 2 - 1) * 0.7, 0, -1);
                }
            }
            cl = fnew(ar, cn);
            cr = fnew(ar, cn);
            for (i = 0; i < cn; i++) {
                cl[i] = (float)((cm[i] + cs[i]) / rr);
                cr[i] = (float)((cm[i] - cs[i]) / rr);
            }
            air_filter(cl, cn, rr);
            air_filter(cr, cn, rr);
            push_stereo(sc, cl, cr, cn, tA + tEv, o->g4);
        }
    }
    if (pl->mode != FF_HUMMODE)
        push_event(sc, pl->mode == FF_IMPACT ? BE_EVENT_IMPACT : BE_EVENT_LAUNCH, tA + tEv);
}

/* ------------------------------------------------------------------ jets and rockets */
/* Jet mixing noise: broad hump around the Strouhal peak, about 6 dB per octave either side. Unit rms. */
static float *jet_noise(arena *ar, int n, double fpk, rng_t *r, int periodic)
{
    float *w = fnew(ar, n), *out = fnew(ar, n);
    double a1 = exp(-TAU * mind(fpk * 1.5, 16000) / FS), a2 = exp(-TAU * fpk * 0.5 / FS), lp = 0, hp = 0, e = 0, rm;
    int i, pass;
    for (i = 0; i < n; i++)
        w[i] = (float)(rnd(r) * 2 - 1);
    for (pass = periodic ? 0 : 1; pass < 2; pass++)
        for (i = 0; i < n; i++) {
            lp = w[i] + (lp - w[i]) * a1;
            hp = lp + (hp - lp) * a2;
            out[i] = (float)(lp - hp);
        }
    for (i = 0; i < n; i++)
        e += (double)out[i] * out[i];
    rm = sqrt(e / n);
    scale(out, n, 1 / (rm ? rm : 1));
    return out;
}

/* Crackle of a supersonic exhaust: sparse, steep, positive-skewed pressure spikes. Unit rms. */
static float *crackle_noise(arena *ar, int n, double fpk, rng_t *r, int periodic)
{
    double fx = clampd(6 * fpk, 900, 5000), dec = exp(-TAU * fx / FS), rate = 0.25 * fx / FS, s = 0, h = 0;
    double a = exp(-TAU * fx / 4 / FS), e = 0, rm;
    float *imp = fnew(ar, n), *out = fnew(ar, n);
    int i, pass;
    for (i = 0; i < n; i++)
        if (rnd(r) < rate)
            imp[i] = (float)(-log(rnd(r) + 1e-9));
    for (pass = periodic ? 0 : 1; pass < 2; pass++)
        for (i = 0; i < n; i++) {
            s = s * dec + imp[i];
            h = s + (h - s) * a;
            out[i] = (float)(s - h);
        }
    for (i = 0; i < n; i++)
        e += (double)out[i] * out[i];
    rm = sqrt(e / n);
    scale(out, n, 1 / (rm ? rm : 1));
    return out;
}

enum { RK_LAUNCH, RK_FLYBY, RK_LANDING, RK_STATIC };
typedef struct {
    traj_t tr;
    float *th;
    int hit, iBurn, iCut;
    double vTd, vmax, mEnd;
} rtraj_t;

typedef struct {
    int mode, vertical, hasTarget;
    double F, Ue, m0, burn, v0, dia, D, miss, targetDist, ramp, maxT;
} thrust_t;

/* Path from thrust, mass and drag: launch from rest, fly-by at constant speed, landing with a braking burn. */
static void traj_thrust(arena *ar, const thrust_t *q, rtraj_t *out)
{
    double dt = 1 / TRAJ_HZ, A = PI * q->dia * q->dia / 4, mdot = q->F / q->Ue, m = q->m0, mMin = 0.2 * q->m0;
    double g = q->vertical ? 9.81 : 0, x = 0, v = 0, thr = 0, vTd = 0, xStop = INFINITY, vmax = 0, vT = 1.5, t;
    int maxN = (int)js_round(TRAJ_HZ * q->maxT), n = 0, hit = 0, i, iBurn = -1, iCut = -1, post = -1, burning = 0;
    double *xs = dnew(ar, maxN + 1), *vs = dnew(ar, maxN + 1), *T = dnew(ar, maxN + 1);
    float *th = fnew(ar, maxN + 2);
    /* a fly-by is trimmed to about four seconds of approach and five of departure */
    if (q->mode == RK_FLYBY) {
        v = q->v0;
        thr = 1;
        xStop = q->D + mind(clampd(25 * q->miss, 30, 2000), q->v0 * 5);
        x = maxd(0, q->D - q->v0 * 4);
        iBurn = 0;
    }
    if (q->mode == RK_LANDING)
        v = q->v0;
    for (i = 0; i <= maxN; i++) {
        t = i * dt;
        if (q->mode == RK_LAUNCH) {
            if (iBurn < 0) iBurn = 0;
            if (t < q->burn && m > mMin)
                thr = mind(1, t / q->ramp);
            else {
                if (iCut < 0) iCut = i;
                thr = maxd(0, thr - dt / 0.06);
            }
        } else if (q->mode == RK_LANDING) {
            if (post >= 0)
                thr = maxd(0, thr - dt / 0.25);
            else {
                /* braking burn: throttle to the deceleration that reaches touchdown speed exactly at the pad */
                double aReq = maxd(0, v * v - vT * vT) / (2 * maxd(q->D - x, 0.05)), aMax = maxd(0.5, q->F / m - g);
                if (!burning && aReq >= 0.85 * aMax) { burning = 1; iBurn = i; }
                if (burning) {
                    double want = clampd(m * (aReq + g) / q->F, 0.12, 1);
                    thr += clampd(want - thr, -dt / q->ramp, dt / q->ramp);
                }
            }
        }
        xs[i] = x; vs[i] = v; th[i] = (float)thr; T[i] = t + js_hypot(q->D - x, q->miss) / C_AIR; n = i + 1;
        if (v > vmax) vmax = v;
        if (post >= 0) {
            if (++post > 0.4 * TRAJ_HZ) break;
            continue;
        }
        if (q->mode == RK_LANDING) {
            if (x >= q->D) { hit = 1; vTd = v; post = 0; x = q->D; v = 0; if (iCut < 0) iCut = i; continue; }
            if (burning) {
                v += (g - q->F * thr / m) * dt;
                if (v < vT) v = vT;
                m = maxd(mMin, m - mdot * thr * dt);
            }
            x += v * dt;
            if (x > q->D) x = q->D;
        } else if (q->mode == RK_FLYBY) {
            if (q->hasTarget && x >= q->targetDist) { hit = 1; vTd = v; break; }
            if (x >= xStop) break;
            x += v * dt;
        } else {
            double a;
            if (q->hasTarget && x >= q->targetDist) { hit = 1; vTd = v; break; }
            if (iCut >= 0 && thr <= 0 && !q->hasTarget) break;
            a = (q->F * thr - 0.5 * RHO * 0.35 * A * v * v) / m - g;
            if (x <= 0 && a < 0) { a = 0; v = 0; }
            v += a * dt;
            x += v * dt;
            if (x < 0) { x = 0; v = 0; }
            m = maxd(mMin, m - mdot * thr * dt);
            if (iCut >= 0 && q->hasTarget && v < 5) break;
        }
    }
    th[n] = th[n - 1];
    out->tr.n = n; out->tr.x = xs; out->tr.v = vs; out->tr.T = T; out->tr.hit = hit; out->tr.xEnd = 0;
    out->th = th; out->hit = hit; out->iBurn = iBurn > 0 ? iBurn : 0; out->iCut = iCut; out->vTd = vTd; out->vmax = vmax; out->mEnd = m;
}

static int shock_from(const traj_t *tr, double miss, double d, double lenRatio, crack_t *c)
{
    int n = tr->n, iMin = 0, i;
    double M, len, b, m2;
    for (i = 1; i < n; i++)
        if (tr->T[i] < tr->T[iMin])
            iMin = i;
    if (!(iMin > 0 && iMin < n - 1 && tr->v[iMin] > C_AIR * 1.01))
        return 0;
    M = tr->v[iMin] / C_AIR; len = d * lenRatio; b = maxd(miss, 0.2); m2 = M * M - 1;
    c->t = tr->T[iMin]; c->x = tr->x[iMin]; c->M = M; c->tau = iMin / TRAJ_HZ;
    c->peak = 0.53 * P0 * pow(m2, 0.125) * d / (pow(b, 0.75) * pow(len, 0.25));
    c->dur = 1.82 * M * pow(b, 0.25) * d / (C_AIR * pow(m2, 0.375) * pow(len, 0.25));
    return 1;
}

typedef struct {
    int mode, vert, has_crack, has_hit, has_pay, passed;
    double F, Ue, De, Mj, p1, fpk, crk, r, r0, passT, passV;
    rtraj_t rt;
    crack_t crack;
    hit_t hit;
    xplan_t pay;
} rkplan_t;

static int rocket_mode(const char *s)
{
    if (!strcmp(s, "launch")) return RK_LAUNCH;
    if (!strcmp(s, "flyby")) return RK_FLYBY;
    if (!strcmp(s, "landing")) return RK_LANDING;
    if (!strcmp(s, "static")) return RK_STATIC;
    return -1;
}

/* Acoustic output from mechanical jet power: efficiency rises steeply with exhaust Mach number, levels near 0.5 % */
static void plan_rocket(arena *ar, const be_params *p, rng_t *g, rkplan_t *out)
{
    double F = PV(p, rkThrust), Ue = PV(p, rkUe), De = PV(p, rkNozzle), Mj = Ue / C_AIR, eta, Pm, Wa;
    thrust_t q;
    int i;
    memset(out, 0, sizeof *out);
    out->mode = rocket_mode(PS(p, rkMode));
    out->vert = !strcmp(PS(p, rkPath), "vertical");
    eta = mind(0.005, 1e-4 * pow(Mj, 5));
    Pm = 0.5 * F * Ue;
    Wa = eta * Pm;
    out->F = F; out->Ue = Ue; out->De = De; out->Mj = Mj;
    out->p1 = sqrt(Wa * RHO * C_AIR / (4 * PI));
    out->fpk = clampd(clampd(0.2 / Mj, 0.03, 0.2) * Ue / De, 25, 9000);
    out->crk = PV(p, rkCrackle) * clampd((Mj - 1.2) / 1.5, 0, 1);
    if (out->mode == RK_STATIC) {
        out->r = maxd(PV(p, miss), 1);
        return;
    }
    memset(&q, 0, sizeof q);
    q.mode = out->mode; q.F = F; q.Ue = Ue; q.m0 = PV(p, rkMass); q.burn = PV(p, rkBurn); q.v0 = PV(p, rkV0); q.dia = De * 1.4;
    q.D = PV(p, D); q.miss = PV(p, miss); q.vertical = out->vert;
    q.hasTarget = out->mode != RK_LANDING && strcmp(PS(p, target), "none");
    q.targetDist = PV(p, targetDist);
    q.ramp = clampd(0.02 + 0.08 * sqrt(De), 0.02, 0.6);
    q.maxT = out->mode == RK_LAUNCH ? mind(14, PV(p, rkBurn) + (q.hasTarget ? 8 : 0.5)) : 14;
    traj_thrust(ar, &q, &out->rt);
    {
        const traj_t *tr = &out->rt.tr;
        int iPass = tr->n - 1;
        for (i = 0; i < tr->n; i++)
            if (tr->x[i] >= q.D) { iPass = i; break; }
        out->passed = tr->x[tr->n - 1] >= q.D;
        out->passT = tr->T[iPass];
        out->passV = tr->v[iPass];
    }
    out->r0 = js_hypot(q.D, q.miss);
    out->has_crack = shock_from(&out->rt.tr, q.miss, De * 1.4, 8, &out->crack);
    if (out->rt.hit) {
        const traj_t *tr = &out->rt.tr;
        double xh = tr->x[tr->n - 1], tau = (tr->n - 1) / TRAJ_HZ, rh = maxd(0.5, js_hypot(q.D - xh, q.miss));
        if (out->mode == RK_LANDING)
            for (i = 0; i < tr->n; i++)
                if (tr->x[i] >= q.D) { tau = i / TRAJ_HZ; break; }
        out->has_hit = 1;
        out->hit.tau = tau; out->hit.t = tau + rh / C_AIR; out->hit.x = xh; out->hit.v = out->rt.vTd; out->hit.r = rh;
        out->hit.E = 0.5 * out->rt.mEnd * out->rt.vTd * out->rt.vTd;
        if (out->mode != RK_LANDING && PV(p, payload) >= 0.01) {
            out->has_pay = 1;
            plan_explosion(ar, PV(p, payload), rh, (int)js_round(PV(p, exFrags)), (int)js_round(PV(p, exDebris)), g, &out->pay);
        }
    }
}

static float *ignition_buf(arena *ar, const rkplan_t *rk, rng_t *r, double sign, int *len_out)
{
    double td = clampd(rk->De / C_AIR * 4, 0.0006, 0.04), fw = clampd(0.5 * rk->fpk, 20, 220);
    int n = (int)ceil((td * 10 + 4 / fw + 0.05) * FS), len = n + 2400, i, nn;
    float *buf = fnew(ar, len), *nb;
    shape_t s = { 0 };
    s.A = sign * 2.5 * rk->p1; s.T = td; s.k = 1;
    add_shape(buf, len, 0.0005, td * 9, &s);
    for (i = 0; i < n; i++) {
        double t = i / FS;
        buf[i] = (float)(buf[i] + sign * 1.5 * rk->p1 * sin(TAU * fw * t) * exp(-t * fw / 1.5) * (1 - exp(-t / 0.002)));
    }
    nn = (int)js_round(0.04 * FS);
    nb = shaped_noise(ar, nn, r, 2500, 60);
    for (i = 0; i < nn; i++)
        nb[i] = (float)(nb[i] * exp(-i / FS / 0.012));
    to_peak(nb, nn, 0.8 * rk->p1);
    for (i = 0; i < nn; i++)
        buf[i] = (float)(buf[i] + nb[i]);
    *len_out = len;
    return buf;
}

typedef struct {
    const rkplan_t *rk;
    const be_params *p;
    const gains_t *o;
    rng_t *r;
    const float *nz, *ck;
    double exS, fch, cph, wph, rough, fw;
} exhaust_st;

static double exhaust_gen(void *v, int k, int i, double f, double x, double rr)
{
    exhaust_st *e = (exhaust_st *)v;
    const rkplan_t *rk = e->rk;
    const be_params *p = e->p;
    double thr = e->rk->rt.th[i] + (e->rk->rt.th[i + 1] - e->rk->rt.th[i]) * f, dir, h, ge, s, wh, q;
    if (thr <= 0.0005)
        return 0;
    /* loudest about 45 degrees off the exhaust axis */
    dir = db_to_lin(6 * (e->exS * (PV(p, D) - x) / rr * 0.697 + PV(p, miss) / rr * 0.717) - 3);
    h = rk->vert ? (rk->mode == RK_LANDING ? PV(p, D) - x : x) : 1e9;
    ge = 1 + exp(-maxd(0, h) / (6 * rk->De));
    q = rnd(e->r);
    e->rough += ((q * 2 - 1) - e->rough) * 0.0008;
    e->cph += TAU * e->fch / FS;
    e->wph += TAU * e->fw * (0.6 + 0.4 * thr) / FS;
    s = (e->nz[k] * (1 - 0.5 * rk->crk) + (e->ck ? e->ck[k] * rk->crk : 0)) * (1 + 9 * e->rough) *
        (1 + 0.6 * PV(p, rkChug) * sin(e->cph + 30 * e->rough));
    wh = PV(p, rkWhine) > 0 ? PV(p, rkWhine) * 0.35 * thr * (sin(e->wph) + 0.5 * sin(2 * e->wph + 1) + 0.4 * sin(1.37 * e->wph)) : 0;
    return rk->p1 * dir * (e->o->g1 * s * sqrt(thr) * ge + e->o->g3 * wh);
}

static double rk_pan(const rkplan_t *rk, const be_params *p, double x, double r)
{
    return rk->vert || r < 0.01 ? 0 : (x - PV(p, D)) / r * 0.9;
}

static void rocket_layers(scene_t *sc, rkplan_t *rk, const be_params *p, const gains_t *o, rng_t *r)
{
    arena *ar = sc->ar;
    const traj_t *tr = &rk->rt.tr;
    double t0 = o->t0, D = PV(p, D), miss = PV(p, miss), ground = PV(p, ground);
    int ns = (int)floor((tr->n - 1) / TRAJ_HZ * FS), len;
    float *buf;
    if (ns >= 64 && (o->g1 > 0 || o->g3 > 0)) {
        exhaust_st st;
        stereo_t wk;
        memset(&st, 0, sizeof st);
        st.rk = rk; st.p = p; st.o = o; st.r = r;
        st.nz = jet_noise(ar, ns, rk->fpk, r, 0);
        st.ck = rk->crk > 0 ? crackle_noise(ar, ns, rk->fpk, r, 0) : 0;
        st.exS = rk->mode == RK_LANDING ? 1 : -1;
        st.fch = clampd(60 / sqrt(rk->De), 25, 400);
        st.fw = PV(p, rkWhineHz);
        if (moving_source(ar, tr, D, miss, exhaust_gen, &st, rk->vert, 0, &wk))
            push_stereo(sc, wk.L, wk.R, wk.n, t0 + wk.t, 1);
    }
    if (rk->mode != RK_FLYBY) {
        double xi = tr->x[rk->rt.iBurn], ri = maxd(1, js_hypot(D - xi, miss)), ti = t0 + tr->T[rk->rt.iBurn];
        if (o->g2 > 0) {
            buf = ignition_buf(ar, rk, r, 1, &len);
            scale(buf, len, 1 / ri);
            post_layer(ar, buf, len, ri, ground);
            push_mono(sc, buf, len, ti, rk_pan(rk, p, xi, ri), o->g2);
        }
        push_event(sc, BE_EVENT_LAUNCH, ti);
        if (rk->mode == RK_LAUNCH && rk->rt.iCut >= 0 && rk->rt.iCut < tr->n) {
            double xc = tr->x[rk->rt.iCut], rc = maxd(1, js_hypot(D - xc, miss)), tc = t0 + tr->T[rk->rt.iCut];
            if (o->g2 > 0) {
                buf = ignition_buf(ar, rk, r, -0.4, &len);
                scale(buf, len, 1 / rc);
                air_filter(buf, len, rc);
                push_mono(sc, buf, len, tc, rk_pan(rk, p, xc, rc), o->g2);
            }
            push_event(sc, BE_EVENT_PASS, tc);
        }
    } else if (rk->passed)
        push_event(sc, BE_EVENT_PASS, t0 + rk->passT);
    if (rk->has_crack && o->g3 > 0) {
        double rb = js_hypot(D - rk->crack.x, miss);
        buf = crack_buf(ar, &rk->crack, &len);
        post_layer(ar, buf, len, rb, ground);
        push_mono(sc, buf, len, t0 + rk->crack.t, rk_pan(rk, p, rk->crack.x, rb), o->g3);
        push_event(sc, BE_EVENT_CRACK, t0 + rk->crack.t);
    }
    if (rk->has_hit && o->g4 > 0) {
        const hit_t *h = &rk->hit;
        double hp = rk_pan(rk, p, h->x, h->r), scl = clampd(pow(0.2 / PV(p, rkMass), 1.0 / 3), 0.25, 3);
        const be_material *surf = be_material_find(strcmp(PS(p, target), "none") ? PS(p, target) : "concrete");
        if (rk->mode == RK_LANDING) {
            buf = modal_hit(ar, be_material_find("steel"), h->E, scl, r, 0, 0, 0, &len);
            scale(buf, len, 1 / h->r);
            post_layer(ar, buf, len, h->r, ground * 0.5);
            push_mono(sc, buf, len, t0 + h->t, hp, o->g4);
            buf = modal_hit(ar, surf, h->E, scl, r, 0, 0, 0, &len);
            scale(buf, len, 1 / h->r);
            air_filter(buf, len, h->r);
            push_mono(sc, buf, len, t0 + h->t, hp, o->g4);
            push_event(sc, BE_EVENT_IMPACT, t0 + h->t);
        } else {
            buf = modal_hit(ar, surf, h->E, scl, r, 0, 0, 0, &len);
            scale(buf, len, 1 / h->r);
            post_layer(ar, buf, len, h->r, ground * 0.5);
            push_mono(sc, buf, len, t0 + h->t, hp, o->g4);
            push_event(sc, BE_EVENT_IMPACT, t0 + h->t);
            if (rk->has_pay) {
                xopt_t xo;
                memset(&xo, 0, sizeof xo);
                xo.W = PV(p, payload); xo.r = h->r; xo.surface = surf; xo.t0 = t0 + h->tau; xo.pan = hp; xo.ground = ground;
                xo.gBlast = xo.gRumble = xo.gFrag = xo.gDebris = o->g4;
                explosion_layers(sc, &rk->pay, &xo, r);
            }
        }
    }
}

/* Static burn, one seamless period: every modulation runs on a whole number of cycles per loop. */
static void rocket_static(arena *ar, const rkplan_t *rk, const be_params *p, const gains_t *o, rng_t *r, int Np, float **mid_out,
                          float **side_out)
{
    float *a = jet_noise(ar, Np, rk->fpk, r, 1), *b = jet_noise(ar, Np, rk->fpk, r, 1);
    float *ck = rk->crk > 0 ? crackle_noise(ar, Np, rk->fpk, r, 1) : 0, *mid = fnew(ar, Np), *side = fnew(ar, Np);
    double Lp = Np / FS, fch = js_round(clampd(60 / sqrt(rk->De), 25, 400) * Lp) / Lp, fw = js_round(PV(p, rkWhineHz) * Lp) / Lp;
    double fw2 = js_round(1.37 * PV(p, rkWhineHz) * Lp) / Lp, ca[6], cp[6], dir = db_to_lin(6 * 0.717 - 3);
    int k, i;
    for (k = 1; k <= 6; k++) {
        double q1 = rnd(r), q2;
        ca[k - 1] = 0.06 / sqrt(k) * (0.5 + q1);
        q2 = rnd(r);
        cp[k - 1] = q2 * TAU;
    }
    for (i = 0; i < Np; i++) {
        double t = i / FS, rough = 0, mod, s, wh;
        for (k = 0; k < 6; k++)
            rough += ca[k] * sin(TAU * (k + 1) * t / Lp + cp[k]);
        mod = (1 + rough) * (1 + 0.6 * PV(p, rkChug) * sin(TAU * fch * t + 3 * rough));
        s = a[i] * (1 - 0.5 * rk->crk) + (ck ? ck[i] * rk->crk : 0);
        wh = PV(p, rkWhine) > 0 ? PV(p, rkWhine) * 0.35 * (sin(TAU * fw * t) + 0.5 * sin(TAU * 2 * fw * t + 1) + 0.4 * sin(TAU * fw2 * t)) : 0;
        mid[i] = (float)(rk->p1 * dir * (o->g1 * s * mod + o->g3 * wh));
        side[i] = (float)(rk->p1 * dir * o->g1 * b[i] * mod * 0.35);
    }
    *mid_out = mid;
    *side_out = side;
}

/* ------------------------------------------------------------------ rotors */
enum { RT_HOVER, RT_FLYBY, RT_TAKEOFF, RT_LANDING };
typedef struct {
    int N, B, mode, nr, RS, has_tr, passed, nev;
    double Dm, n0, vtip, Mt, p1, dur, bpf, D, r, passT;
    float **nj;
    traj_t tr;
    double ev_t[2];
} rtplan_t;

typedef struct { int mode; double x0, v, ta, tl, v0, dd; } rtmotion_t;

static double rt_tau(const rtmotion_t *m, double t)
{
    if (m->mode == RT_HOVER) return 1;
    if (m->mode == RT_FLYBY) return 1.08;
    if (m->mode == RT_TAKEOFF)
        return t < 0.5 ? 0.7 * t : t < 1 ? 0.35 : t < 1.6 ? 0.35 + 0.95 * (t - 1) / 0.6 : t < 3 ? 1.3 : 1.3 - 0.2 * mind(1, (t - 3) / 0.5);
    return t < m->tl - 0.6 ? 0.93 : t < m->tl ? 0.93 + 0.3 * (1 - (m->tl - t) / 0.6) : maxd(0, 1.23 * (1 - (t - m->tl) / 1.2));
}

static double rt_x(const void *st, double t)
{
    const rtmotion_t *m = (const rtmotion_t *)st;
    if (m->mode == RT_HOVER) return 0;
    if (m->mode == RT_FLYBY) return m->x0 + m->v * t;
    if (m->mode == RT_TAKEOFF) {
        double s = maxd(0, t - 1.5), ta = m->v / 3;
        return s < ta ? 1.5 * s * s : 1.5 * ta * ta + m->v * (s - ta);
    }
    return t < m->tl ? m->v0 * t - 0.5 * m->v0 / m->tl * t * t : m->dd;
}

static int rotor_mode(const char *s)
{
    if (!strcmp(s, "hover")) return RT_HOVER;
    if (!strcmp(s, "flyby")) return RT_FLYBY;
    if (!strcmp(s, "takeoff")) return RT_TAKEOFF;
    if (!strcmp(s, "landing")) return RT_LANDING;
    return -1;
}

/* Hover speed follows from weight: thrust per rotor = kT rho n^2 D^4. Sound power is a small, tip-Mach-dependent
   share of shaft power. */
static void plan_rotor(arena *ar, const be_params *p, rng_t *g, rtplan_t *out)
{
    int N = (int)clampd(js_round(PV(p, rtCount)), 1, 8), B = (int)clampd(js_round(PV(p, rtBlades)), 2, 7), j, k, i, hover;
    double Dm = PV(p, rtDia), m = PV(p, rtMass), kT = clampd(0.11 * pow(0.3 / Dm, 0.3), 0.04, 0.11), n0, vtip, Mt, A1, T1, Pm, Wa;
    double v = PV(p, rtSpeed), D = PV(p, D), dur, miss = PV(p, miss);
    rtmotion_t mo;
    memset(out, 0, sizeof *out);
    memset(&mo, 0, sizeof mo);
    n0 = sqrt(m * 9.81 / (N * kT * RHO * pow(Dm, 4)));
    if (PI * Dm * n0 > 0.92 * C_AIR)
        n0 = 0.92 * C_AIR / (PI * Dm);
    vtip = PI * Dm * n0; Mt = vtip / C_AIR; A1 = PI * Dm * Dm / 4; T1 = m * 9.81 / N;
    Pm = N * pow(T1, 1.5) / sqrt(2 * RHO * A1);
    Wa = Pm * 1e-3 * pow(Mt, 3);
    mo.mode = rotor_mode(PS(p, rtMode));
    mo.v = v;
    out->mode = mo.mode;
    if (mo.mode == RT_HOVER) { dur = LOOP_S; D = 0; }
    else if (mo.mode == RT_FLYBY) {
        double xs = D + mind(clampd(15 * miss, 20, 300), v * 4.5);
        mo.x0 = maxd(0, D - v * 4.5);
        dur = maxd(0.5, (xs - mo.x0) / v);
    } else if (mo.mode == RT_TAKEOFF) {
        dur = 5.5;
        out->ev_t[out->nev++] = 1.5;
    } else {
        mo.dd = maxd(D, 2);
        mo.tl = mind(2 * mo.dd / v, 8);
        mo.v0 = 2 * mo.dd / mo.tl;
        D = mo.dd;
        dur = mo.tl + 1.6;
        out->ev_t[out->nev++] = mo.tl;
    }
    out->RS = 200;
    out->nr = (int)ceil(dur * out->RS) + 2;
    hover = mo.mode == RT_HOVER;
    out->nj = (float **)ar_alloc(ar, N * sizeof(float *));
    for (j = 0; j < N; j++) {
        double det = (rnd(g) * 2 - 1) * 0.012, ca[5], cp[5], base;
        float *arr = fnew(ar, out->nr);
        for (k = 1; k <= 5; k++) {
            double q1 = rnd(g), q2;
            ca[k - 1] = 0.009 / k * (0.5 + q1);
            q2 = rnd(g);
            cp[k - 1] = q2 * TAU;
        }
        /* in a loop every rotor must complete a whole number of blade passes */
        base = hover ? js_round(n0 * (1 + PV(p, rtWander) * det) * B * dur) / (B * dur) : n0;
        for (i = 0; i < out->nr; i++) {
            double t = (double)i / out->RS, w = hover ? 0 : det;
            for (k = 0; k < 5; k++)
                w += ca[k] * sin(TAU * (k + 1) * t / dur + cp[k]);
            arr[i] = (float)(base * (1 + PV(p, rtWander) * w) * sqrt(rt_tau(&mo, mind(t, dur))));
        }
        out->nj[j] = arr;
    }
    out->N = N; out->B = B; out->Dm = Dm; out->n0 = n0; out->vtip = vtip; out->Mt = Mt;
    out->p1 = sqrt(Wa * RHO * C_AIR / (4 * PI));
    out->dur = dur; out->bpf = B * n0; out->D = D; out->r = maxd(1, miss);
    if (!hover) {
        int iPass;
        traj_kin_fn(ar, dur, D, miss, rt_x, &mo, &out->tr);
        out->has_tr = 1;
        iPass = out->tr.n - 1;
        for (i = 0; i < out->tr.n; i++)
            if (out->tr.x[i] >= D) { iPass = i; break; }
        out->passed = out->tr.x[out->tr.n - 1] >= D;
        out->passT = out->tr.T[iPass];
    }
}

/* Rotor source at 1 m: blade-pass tones from a wavetable, vortex noise modulated at blade rate, slap, motor whine. */
static void rotor_signal(arena *ar, const rtplan_t *rt, const be_params *p, const gains_t *o, rng_t *r, int ns, int periodic,
                         float **mid_out, float **side_out)
{
    int N = rt->N, B = rt->B, size = 2048, M = (int)clampd(floor(15000 / (rt->bpf * 1.4)), 2, 40), m, i, j, k, pass, mB;
    float *tone = fnew(ar, size + 1), *slapT = fnew(ar, size + 1), *wn, *bb, *mid, *side;
    double kx = clampd(2.2 - 1.8 * rt->Mt, 0.5, 2.2), e = 0, fb0, bbShare, lp = 0, bp = 0, eb = 0, rm, *c, gN, gS, gW;
    for (m = 1; m <= M; m++) {
        double am = pow(m, -kx), ph = rnd(r) * TAU;
        for (i = 0; i < size; i++) {
            double th = TAU * i / size;
            tone[i] = (float)(tone[i] + am * sin(m * th + ph));
            slapT[i] = (float)(slapT[i] + cos(m * th));
        }
    }
    for (i = 0; i < size; i++)
        e += (double)tone[i] * tone[i];
    e = sqrt(e / size);
    if (!e) e = 1;
    for (i = 0; i < size; i++) {
        tone[i] = (float)(tone[i] / e);
        slapT[i] = (float)(slapT[i] * (1.0 / sqrt(M / 2.0)));
    }
    tone[size] = tone[0];
    slapT[size] = slapT[0];
    fb0 = 0.25 * rt->vtip / (0.02 * rt->Dm + 0.004);
    bbShare = clampd(0.9 - 0.25 * log10(rt->Dm / 0.1), 0.3, 0.9);
    wn = fnew(ar, ns);
    bb = fnew(ar, ns);
    for (i = 0; i < ns; i++)
        wn[i] = (float)(rnd(r) * 2 - 1);
    for (pass = periodic ? 0 : 1; pass < 2; pass++)
        for (i = 0; i < ns; i++) {
            int iu0 = (int)mind(rt->nr - 1, floor(i / FS * rt->RS));
            double fc = clampd(fb0 * rt->nj[0][iu0] / rt->n0, 80, 7500), fk = 2 * sin(PI * fc / FS), hp;
            lp += fk * bp;
            hp = wn[i] - lp - 1.4 * bp;
            bp += fk * hp;
            bb[i] = (float)bp;
        }
    for (i = 0; i < ns; i++)
        eb += (double)bb[i] * bb[i];
    rm = sqrt(eb / ns);
    scale(bb, ns, 1 / (rm ? rm : 1));
    c = dnew(ar, N);
    mB = (int)maxd(1, js_round(clampd(1200 / rt->bpf, 3, 400)));
    mid = fnew(ar, ns);
    side = fnew(ar, ns);
    gN = 1 / sqrt(N);
    gS = o->g4 * PV(p, rtSlap);
    gW = o->g3 * PV(p, rtWhine) * 0.5;
    for (k = 0; k < ns; k++) {
        double u = k / FS * rt->RS, fu, sm = 0, ss = 0, amod = 0, fr = 0, amp;
        int iu = (int)mind(rt->nr - 2, floor(u));
        fu = u - iu;
        for (j = 0; j < N; j++) {
            double nn = rt->nj[j][iu] + (rt->nj[j][iu + 1] - rt->nj[j][iu]) * fu, pos, fp, s;
            int ip;
            c[j] += B * nn / FS;
            if (c[j] >= 1) c[j] -= floor(c[j]);
            pos = c[j] * size;
            ip = (int)floor(pos);
            fp = pos - ip;
            s = o->g1 * (tone[ip] + (tone[ip + 1] - tone[ip]) * fp) + gS * (slapT[ip] + (slapT[ip + 1] - slapT[ip]) * fp);
            if (gW > 0)
                s += gW * (fsin(mB * c[j]) + 0.4 * fsin(2 * mB * c[j] + 0.16));
            sm += s;
            ss += (j & 1) ? -s : s;
            amod += 1 + 0.7 * fsin(c[j] + 0.25 + j * 0.3);
            fr += nn;
        }
        fr /= N * rt->n0;
        amp = rt->p1 * pow(fr, 2.5);
        mid[k] = (float)(amp * (sm * gN + o->g2 * bbShare * bb[k] * amod / N));
        side[k] = (float)(amp * ss * gN * 0.45);
    }
    *mid_out = mid;
    *side_out = side;
}

static double rotor_gen(void *v, int k, int i_, double f_, double x_, double r_) { return ((const float *)v)[k]; }

static void rotor_layers(scene_t *sc, rtplan_t *rt, const be_params *p, const gains_t *o, rng_t *r)
{
    const traj_t *tr = &rt->tr;
    int ns = (int)floor((tr->n - 1) / TRAJ_HZ * FS), e;
    float *mid, *side;
    stereo_t wk;
    if (ns < 64)
        return;
    rotor_signal(sc->ar, rt, p, o, r, ns, 0, &mid, &side);
    if (moving_source(sc->ar, tr, rt->D, PV(p, miss), rotor_gen, mid, 0, 0, &wk))
        push_stereo(sc, wk.L, wk.R, wk.n, o->t0 + wk.t, 1);
    if (rt->mode == RT_FLYBY && rt->passed)
        push_event(sc, BE_EVENT_PASS, o->t0 + rt->passT);
    for (e = 0; e < rt->nev; e++) {
        int i = (int)mind(tr->n - 1, js_round(rt->ev_t[e] * TRAJ_HZ));
        push_event(sc, BE_EVENT_LAUNCH, o->t0 + tr->T[i]);
    }
}

/* ------------------------------------------------------------------ projectile */
static int launch_of(const char *s)
{
    if (!strcmp(s, "blast")) return LAUNCH_BLAST;
    if (!strcmp(s, "string")) return LAUNCH_STRING;
    if (!strcmp(s, "ricSteel")) return LAUNCH_RIC_STEEL;
    if (!strcmp(s, "ricStone")) return LAUNCH_RIC_STONE;
    return LAUNCH_NONE;
}

static void proj_from(const be_params *q, proj_t *p)
{
    memset(p, 0, sizeof *p);
    p->v0 = PV(q, v0); p->cal = PV(q, cal); p->mass = PV(q, mass); p->lenRatio = PV(q, lenRatio);
    p->cdScale = PV(q, cdScale); p->tumble = PV(q, tumble); p->tumbleHz = PV(q, tumbleHz); p->swing = PV(q, swing) != 0;
    p->pathLen = PV(q, pathLen); p->D = PV(q, D); p->miss = PV(q, miss); p->targetDist = PV(q, targetDist);
    p->launch = launch_of(PS(q, launch));
    p->target = strcmp(PS(q, target), "none") ? be_material_find(PS(q, target)) : 0;
    p->maxT = 0;     /* 8 s */
    p->vStop = 0;    /* 6 m/s */
}

typedef struct {
    int kind;                /* 0 projectile, 1 explosion */
    be_params q;
    double pan, gf;
    proj_t pp;
    proj_an an;
    int has_pay;
    xplan_t pay;
    xplan_t ex;
    weapon_t wp;
    dplan_t dc;
    implo_t im;
    dsplan_t ds;
    fplan_t ff;
    rkplan_t rk;
    rtplan_t rt;
} plan_t;

static void proj_layers(scene_t *sc, plan_t *pl, const be_params *p, const gains_t *o, rng_t *r)
{
    arena *ar = sc->ar;
    const proj_an *an = &pl->an;
    const proj_t *pp = &pl->pp;
    double g, t0 = o->t0, pan0 = an->r0 > 0.01 ? -PV(p, D) / an->r0 * 0.9 : 0, ground = PV(p, ground);
    float *buf;
    int len;
    g = o->g2;
    if (an->has_blast && g > 0) {
        buf = blast_buf(ar, &an->blast, r, &len);
        post_layer(ar, buf, len, an->blast.r, ground);
        push_mono(sc, buf, len, t0 + an->blast.t, pan0, g);
        push_event(sc, BE_EVENT_LAUNCH, t0 + an->blast.t);
    } else if (pp->launch >= LAUNCH_STRING && g > 0) {
        const char *mat = pp->launch == LAUNCH_STRING ? "string" : pp->launch == LAUNCH_RIC_STEEL ? "steel" : "concrete";
        double mul = pp->launch == LAUNCH_STRING ? 1 : 2.6, r1 = maxd(an->r0, 1);
        buf = modal_hit(ar, be_material_find(mat), an->muzzleE * mul, pow(0.008 / (PV(p, cal) / 1000), 0.3), r, 0, 0, 0, &len);
        scale(buf, len, 1 / r1);
        post_layer(ar, buf, len, r1, ground);
        push_mono(sc, buf, len, t0 + an->launchT, pan0, g);
        push_event(sc, BE_EVENT_LAUNCH, t0 + an->launchT);
    }
    g = o->g1;
    if (an->has_crack && g > 0) {
        double rc = js_hypot(PV(p, D) - an->crack.x, PV(p, miss));
        buf = crack_buf(ar, &an->crack, &len);
        post_layer(ar, buf, len, rc, ground);
        push_mono(sc, buf, len, t0 + an->crack.t, (an->crack.x - PV(p, D)) / rc * 0.9, g);
        push_event(sc, BE_EVENT_CRACK, t0 + an->crack.t);
    }
    g = o->g3;
    if (g > 0) {
        stereo_t wk;
        if (wake_layer(ar, pp, &an->tr, an->has_hit, r, &wk)) {
            push_stereo(sc, wk.L, wk.R, wk.n, t0 + wk.t, g);
            if (an->passed)
                push_event(sc, BE_EVENT_PASS, t0 + an->passT);
        }
    }
    g = o->g4;
    if (an->has_hit && g > 0) {
        double hp = (an->hit.x - PV(p, D)) / an->hit.r * 0.9;
        buf = modal_hit(ar, pp->target, an->hit.E, pow(0.008 / (PV(p, cal) / 1000), 0.3), r, 0, 0, 0, &len);
        scale(buf, len, 1 / an->hit.r);
        post_layer(ar, buf, len, an->hit.r, ground * 0.5);
        push_mono(sc, buf, len, t0 + an->hit.t, hp, g);
        push_event(sc, BE_EVENT_IMPACT, t0 + an->hit.t);
        if (pl->has_pay) {
            xopt_t xo;
            xo.W = PV(p, payload); xo.r = an->hit.r; xo.surface = pp->target; xo.t0 = t0 + an->hit.tau; xo.pan = hp;
            xo.ground = ground; xo.gBlast = xo.gRumble = xo.gFrag = xo.gDebris = g; xo.grains = 0;
            explosion_layers(sc, &pl->pay, &xo, r);
        }
    }
}

/* ------------------------------------------------------------------ assembling a scene */
static int kind_of(const char *k)
{
    if (!strcmp(k, "projectile")) return 0;
    if (!strcmp(k, "explosion")) return 1;
    if (!strcmp(k, "blaster")) return 2;
    if (!strcmp(k, "discharge")) return 3;
    if (!strcmp(k, "implosion")) return 4;
    if (!strcmp(k, "destruction")) return 5;
    if (!strcmp(k, "field")) return 6;
    if (!strcmp(k, "rocket")) return 7;
    if (!strcmp(k, "rotor")) return 8;
    return -1;
}

int be_kind_supported(const char *kind) { return kind && kind_of(kind) >= 0; }

static gains_t scene_gains(const be_params *p)
{
    gains_t o;
    memset(&o, 0, sizeof o);
    o.g1 = trim_gain(PV(p, trimCrack));
    o.g2 = trim_gain(PV(p, trimLaunch));
    o.g3 = trim_gain(PV(p, trimWake));
    o.g4 = trim_gain(PV(p, trimImpact));
    return o;
}

static void plan_rep(arena *ar, const be_params *p, int seed, int k, plan_t *out)
{
    rng_t g = rng_new((double)seed * 31 + k * 7919 + 17);
    double j = PV(p, seqJitter), pan = 0;
    int kind = kind_of(PS(p, kind));
    out->q = *p;
    if (k > 0 && j > 0) {
        double a = 1 + j * 0.7 * (rnd(&g) - 0.5);
        if (kind == 1)
            PV(&out->q, exDist) = PV(p, exDist) * a;
        else if (kind == 2) {
            double b2, b3;
            PV(&out->q, miss) = PV(p, miss) * a;
            b2 = rnd(&g);
            PV(&out->q, bpStiff) = PV(p, bpStiff) * (1 + j * 0.3 * (b2 - 0.5));
            b3 = rnd(&g);
            PV(&out->q, bvSpeed) = PV(p, bvSpeed) * (1 + j * 0.1 * (b3 - 0.5));
        } else if (kind == 3)
            PV(&out->q, dcDist) = PV(p, dcDist) * a;
        else if (kind == 4)
            PV(&out->q, imDist) = PV(p, imDist) * a;
        else if (kind == 5)
            PV(&out->q, dsDist) = PV(p, dsDist) * a;
        else if (kind == 6)
            PV(&out->q, ffDist) = PV(p, ffDist) * a;
        else
            PV(&out->q, miss) = PV(p, miss) * a;
        if (kind != 0)
            pan = j * 1.4 * (rnd(&g) - 0.5);
    }
    out->kind = kind;
    out->has_pay = 0;
    if (kind == 1)
        plan_explosion(ar, PV(&out->q, exW), PV(&out->q, exDist), (int)js_round(PV(&out->q, exFrags)),
                       (int)js_round(PV(&out->q, exDebris)), &g, &out->ex);
    else if (kind == 2)
        plan_weapon(ar, &out->q, &g, &out->wp);
    else if (kind == 3)
        plan_discharge(ar, &out->q, &g, &out->dc);
    else if (kind == 4)
        plan_implosion(ar, !strcmp(PS(&out->q, imMedium), "water"), PV(&out->q, imDp), PV(&out->q, imGas), PV(&out->q, imR0),
                       PV(&out->q, imDist), &out->im);
    else if (kind == 6)
        plan_field(ar, &out->q, &g, &out->ff);
    else if (kind == 7)
        plan_rocket(ar, &out->q, &g, &out->rk);
    else if (kind == 8)
        plan_rotor(ar, &out->q, &g, &out->rt);
    else if (kind == 5)
        plan_destruction(ar, PS(&out->q, dsMat), PV(&out->q, dsMass), PV(&out->q, dsHeight), PV(&out->q, dsPieces),
                         PV(&out->q, dsDist), PV(&out->q, dsSpread), &g, &out->ds);
    else {
        proj_from(&out->q, &out->pp);
        analyze_proj(ar, &out->pp, &out->an);
        if (out->an.has_hit && PV(&out->q, payload) >= 0.01) {
            out->has_pay = 1;
            plan_explosion(ar, PV(&out->q, payload), out->an.hit.r, (int)js_round(PV(&out->q, exFrags)),
                           (int)js_round(PV(&out->q, exDebris)), &g, &out->pay);
        }
    }
    out->pan = pan;
    out->gf = 1;
}

typedef struct { float *L, *R; int N; double tStart, peakPa; } mix_t;

static void mix_scene(scene_t *sc, const be_params *p, int seed, mix_t *out)
{
    arena *ar = sc->ar;
    int count = (int)clampd(js_round(PV(p, seqCount) ? PV(p, seqCount) : 1), 1, 16), k, i, N;
    double tOff = 0, tStart = INFINITY, tEnd = 0, durS, interval = PV(p, seqInterval) ? PV(p, seqInterval) : 0.1;
    rng_t trnd = rng_new((double)(js_uint32(seed) ^ 0x51ed));
    const be_env *env = be_env_find(PS(p, env));
    float *L, *R;
    grains_t bank, *grains = 0;
    if (!env)
        env = be_env_find("none");
    if ((kind_of(PS(p, kind)) == 1 && !strcmp(PS(p, exSurface), "rec")) ||
        (kind_of(PS(p, kind)) == 5 && !strcmp(PS(p, dsDebris), "rec"))) {
        float *gL, *gR;
        int gn;
        make_gravel(ar, &gL, &gR, &gn);          /* the only recording this engine has: check() made sure */
        if (cut_grains(ar, gL, gR, gn, &bank))
            grains = &bank;
    }
    for (k = 0; k < count; k++) {
        plan_t *pl = (plan_t *)ar_alloc(ar, sizeof *pl);
        rng_t r;
        gains_t o;
        plan_rep(ar, p, seed, k, pl);
        r = rng_new((double)seed + k * 104729.0);
        o = scene_gains(&pl->q);
        o.t0 = tOff;
        o.pan = pl->pan;
        o.ground = PV(&pl->q, ground);
        o.g1 *= pl->gf; o.g2 *= pl->gf; o.g3 *= pl->gf; o.g4 *= pl->gf;
        if (pl->kind == 1) {
            xopt_t xo;
            const char *surf = PS(&pl->q, exSurface);
            xo.W = PV(&pl->q, exW); xo.r = PV(&pl->q, exDist); xo.surface = be_material_find(surf); xo.t0 = tOff;
            xo.pan = pl->pan; xo.ground = PV(&pl->q, ground);
            xo.gBlast = o.g1; xo.gRumble = o.g2; xo.gFrag = o.g3; xo.gDebris = o.g4; xo.grains = grains;
            explosion_layers(sc, &pl->ex, &xo, &r);
        } else if (pl->kind == 2)
            weapon_layers(sc, &pl->wp, &pl->q, &o, &r);
        else if (pl->kind == 3)
            discharge_layers(sc, &pl->dc, &pl->q, &o, &r);
        else if (pl->kind == 4)
            implosion_layers(sc, &pl->im, &pl->q, &o, &r);
        else if (pl->kind == 5)
            destruction_layers(sc, &pl->ds, &pl->q, &o, &r, grains);
        else if (pl->kind == 6)
            field_layers(sc, &pl->ff, &pl->q, &o, &r);
        else if (pl->kind == 7)
            rocket_layers(sc, &pl->rk, &pl->q, &o, &r);
        else if (pl->kind == 8)
            rotor_layers(sc, &pl->rt, &pl->q, &o, &r);
        else
            proj_layers(sc, pl, &pl->q, &o, &r);
        tOff += interval * (1 + PV(p, seqJitter) * (rnd(&trnd) - 0.5));
    }
    for (i = 0; i < sc->n_layers; i++) {
        const layer_t *l = &sc->layers[i];
        if (l->t < tStart) tStart = l->t;
        if (l->t + l->n / FS > tEnd) tEnd = l->t + l->n / FS;
    }
    if (!sc->n_layers) { tStart = 0; tEnd = 0.2; }
    tStart -= 0.03;
    durS = mind(24, tEnd - tStart + (env->rt60 ? env->rt60 * 0.9 + 0.2 : 0.05));
    N = (int)ceil(durS * FS);
    L = fnew(ar, N);
    R = fnew(ar, N);
    for (i = 0; i < sc->n_layers; i++) {
        const layer_t *l = &sc->layers[i];
        int s = (int)js_round((l->t - tStart) * FS), j;
        if (l->m) {
            double gl = l->g * cos((l->pan + 1) * PI / 4), gr = l->g * sin((l->pan + 1) * PI / 4);
            for (j = 0; j < l->n && s + j < N; j++) {
                L[s + j] = (float)(L[s + j] + l->m[j] * gl);
                R[s + j] = (float)(R[s + j] + l->m[j] * gr);
            }
        } else {
            for (j = 0; j < l->n && s + j < N; j++) {
                L[s + j] = (float)(L[s + j] + l->L[j] * l->g);
                R[s + j] = (float)(R[s + j] + l->R[j] * l->g);
            }
        }
    }
    out->peakPa = peak_of(L, R, N);
    if (env->rt60) {
        fdn_opt fo;
        float *wL, *wR;
        double wg = db_to_lin(env->wet);
        env_fdn_opt(env, &fo);
        fdn(ar, L, R, N, &fo, &wL, &wR);
        for (i = 0; i < N; i++) {
            L[i] = (float)(L[i] + wL[i] * wg);
            R[i] = (float)(R[i] + wR[i] * wg);
        }
    }
    out->L = L;
    out->R = R;
    out->N = N;
    out->tStart = tStart;
}

/* keep the first n samples, fading the last 50 ms out */
static int cut_fade(float *L, float *R, int n)
{
    int fo = (int)mind(n, js_round(0.05 * FS)), j;
    for (j = 0; j < fo; j++) {
        L[n - 1 - j] = (float)(L[n - 1 - j] * ((double)j / fo));
        R[n - 1 - j] = (float)(R[n - 1 - j] * ((double)j / fo));
    }
    return n;
}

static int trim_tail(float *L, float *R, int len)
{
    double pk = peak_of(L, R, len), thr = pk * 3e-4;
    int n = len - 1;
    while (n > 0 && fabs(L[n]) < thr && fabs(R[n]) < thr)
        n--;
    n = (int)mind(len, n + js_round(0.08 * FS));
    return cut_fade(L, R, n);
}

static int check(const be_params *p)
{
    const be_env *env = be_env_find(PS(p, env));
    if (kind_of(PS(p, kind)) < 0)
        return BE_ERR_KIND;
    if (env && env->ir)
        return BE_ERR_UNSUPPORTED;
    if (!strcmp(PS(p, seqFrom), "rec"))
        return BE_ERR_UNSUPPORTED;
    if (kind_of(PS(p, kind)) == 0) {
        if (strcmp(PS(p, target), "none") && !be_material_find(PS(p, target)))
            return BE_ERR_VALUE;
        if (!(PV(p, mass) > 0) || !(PV(p, cal) > 0) || !(PV(p, v0) > 0))
            return BE_ERR_VALUE;
    } else if (kind_of(PS(p, kind)) == 2) {
        if (launcher_of(PS(p, lnType)) < 0 || bolt_of(PS(p, bvType)) < 0)
            return BE_ERR_VALUE;
        if (strcmp(PS(p, tgMat), "none") && !ablation_find(PS(p, tgMat)))
            return BE_ERR_VALUE;
        if (launcher_of(PS(p, lnType)) == LN_LINE && strcmp(PS(p, bpBody), "none") && !body_find(PS(p, bpBody)))
            return BE_ERR_VALUE;
    } else if (kind_of(PS(p, kind)) == 7) {
        if (rocket_mode(PS(p, rkMode)) < 0 || !(PV(p, rkThrust) > 0) || !(PV(p, rkUe) > 0) || !(PV(p, rkNozzle) > 0) ||
            !(PV(p, rkMass) > 0))
            return BE_ERR_VALUE;
        if (strcmp(PS(p, target), "none") && !be_material_find(PS(p, target)))
            return BE_ERR_VALUE;
    } else if (kind_of(PS(p, kind)) == 8) {
        if (rotor_mode(PS(p, rtMode)) < 0 || !(PV(p, rtDia) > 0) || !(PV(p, rtMass) > 0))
            return BE_ERR_VALUE;
        if (rotor_mode(PS(p, rtMode)) != RT_HOVER && !(PV(p, rtSpeed) > 0))
            return BE_ERR_VALUE;
    } else if (kind_of(PS(p, kind)) == 6) {
        if (field_mode(PS(p, ffMode)) < 0 || !(PV(p, ffDens) > 0))
            return BE_ERR_VALUE;
    } else if (kind_of(PS(p, kind)) == 4) {
        const char *sh = PS(p, imShell);
        if (strcmp(sh, "none") && (!break_find(sh) || !(break_find(sh)->shell_density > 0)))
            return BE_ERR_VALUE;
        if (!(PV(p, imR0) > 0) || !(PV(p, imDp) > 0) || !(PV(p, imGas) >= 0) || !(PV(p, imGas) < 1))
            return BE_ERR_VALUE;
    } else if (kind_of(PS(p, kind)) == 5) {
        if (!strcmp(PS(p, dsMat), "measured"))
            return BE_ERR_UNSUPPORTED;                  /* its modes are measured from a recording */
        if (!break_find(PS(p, dsMat)) || !(PV(p, dsMass) > 0))
            return BE_ERR_VALUE;
        if (!strcmp(PS(p, dsDebris), "rec") && strcmp(PS(p, grAsset), "ex:gravel"))
            return BE_ERR_UNSUPPORTED;
    } else if (kind_of(PS(p, kind)) == 3) {
        if (!(PV(p, dcEnergy) > 0) || !(PV(p, dcLength) > 0) || !(PV(p, dcDist) >= 0))
            return BE_ERR_VALUE;
    } else {
        if (!(PV(p, exW) > 0) || !(PV(p, exDist) > 0))
            return BE_ERR_VALUE;
        if (!strcmp(PS(p, exSurface), "rec") && strcmp(PS(p, grAsset), "ex:gravel"))
            return BE_ERR_UNSUPPORTED;
    }
    return BE_OK;
}

int be_check(const be_params *p) { return p ? check(p) : BE_ERR_VALUE; }

int be_preset_supported(int i)
{
    be_params p;
    if (i < 0 || i >= be_preset_count())
        return BE_ERR_PRESET;
    be_params_reset(&p);
    if (be_params_preset(&p, be_preset_id(i)))
        return BE_ERR_PRESET;
    return check(&p);
}

static void sort_events(be_event *e, int n)       /* stable, as Array.prototype.sort */
{
    int i, j;
    for (i = 1; i < n; i++) {
        be_event x = e[i];
        for (j = i - 1; j >= 0 && e[j].t > x.t; j--)
            e[j + 1] = e[j];
        e[j + 1] = x;
    }
}

int be_is_loop(const be_params *p)
{
    int k = p ? kind_of(PS(p, kind)) : -1;
    return (k == 6 && !strcmp(PS(p, ffMode), "hum")) || (k == 7 && !strcmp(PS(p, rkMode), "static")) ||
           (k == 8 && !strcmp(PS(p, rtMode), "hover"));
}

/* A filter applied to one period of a loop must itself come out periodic: run it over two, keep the second. */
static float *periodic_post(arena *ar, const float *x, int n, double r, double ground)
{
    float *t = fnew(ar, 2 * n);
    memcpy(t, x, (size_t)n * sizeof(float));
    memcpy(t + n, x, (size_t)n * sizeof(float));
    post_layer(ar, t, 2 * n, r, ground);
    return t + n;
}

/* a seamless loop: one period, its filters and its room wrapped round the seam */
static void render_loop(arena *ar, const be_params *p, int seed, mix_t *out)
{
    plan_t *pl = (plan_t *)ar_alloc(ar, sizeof *pl);
    rng_t r = rng_new((double)seed + 5);
    int Np = LOOP_S * FSI, i;
    gains_t o;
    float *mid, *side, *L = fnew(ar, Np), *R = fnew(ar, Np);
    double rr;
    const be_env *env = be_env_find(PS(p, env));
    plan_rep(ar, p, seed, 0, pl);
    o = scene_gains(p);
    if (pl->kind == 8) {
        rotor_signal(ar, &pl->rt, p, &o, &r, Np, 1, &mid, &side);
        rr = pl->rt.r;
    } else if (pl->kind == 6) {
        field_signal(ar, &pl->ff, p, &o, &r, Np, 1, &mid, &side);
        rr = pl->ff.r;
    } else {
        rocket_static(ar, &pl->rk, p, &o, &r, Np, &mid, &side);
        rr = pl->rk.r;
    }
    for (i = 0; i < Np; i++) {
        L[i] = (float)((mid[i] + side[i]) / rr);
        R[i] = (float)((mid[i] - side[i]) / rr);
    }
    L = periodic_post(ar, L, Np, rr, PV(p, ground));
    R = periodic_post(ar, R, Np, rr, PV(p, ground));
    out->peakPa = peak_of(L, R, Np);
    if (!env)
        env = be_env_find("none");
    if (env->rt60) {
        fdn_opt fo;
        float *tL = tile3(ar, L, Np), *tR = tile3(ar, R, Np), *wL, *wR;
        double wg = db_to_lin(env->wet);
        env_fdn_opt(env, &fo);
        fdn(ar, tL, tR, 3 * Np, &fo, &wL, &wR);
        for (i = 0; i < Np; i++) {
            L[i] = (float)(L[i] + wL[2 * Np + i] * wg);
            R[i] = (float)(R[i] + wR[2 * Np + i] * wg);
        }
    }
    out->L = L;
    out->R = R;
    out->N = Np;
    out->tStart = 0;
}

int be_render(const be_params *p, int seed, be_sound *out)
{
    arena ar;
    scene_t sc;
    mix_t mx;
    double pk;
    int st, n, i, ne = 0, loop = be_is_loop(p);
    memset(out, 0, sizeof *out);
    if (!p)
        return BE_ERR_VALUE;
    st = check(p);
    if (st)
        return st;
    memset(&ar, 0, sizeof ar);
    if (setjmp(ar.fail)) {
        ar_free(&ar);
        be_sound_free(out);
        return BE_ERR_MEMORY;
    }
    memset(&sc, 0, sizeof sc);
    sc.ar = &ar;
    if (loop)
        render_loop(&ar, p, seed, &mx);
    else
        mix_scene(&sc, p, seed, &mx);
    pk = peak_of(mx.L, mx.R, mx.N);
    if (!pk)
        pk = 1;
    scale(mx.L, mx.N, 1 / pk);
    scale(mx.R, mx.N, 1 / pk);
    n = loop ? mx.N : trim_tail(mx.L, mx.R, mx.N);
    out->L = (float *)malloc((size_t)(n ? n : 1) * sizeof(float));
    out->R = (float *)malloc((size_t)(n ? n : 1) * sizeof(float));
    out->events = (be_event *)malloc((size_t)(sc.n_ev ? sc.n_ev : 1) * sizeof(be_event));
    if (!out->L || !out->R || !out->events)
        longjmp(ar.fail, 1);
    memcpy(out->L, mx.L, (size_t)n * sizeof(float));
    memcpy(out->R, mx.R, (size_t)n * sizeof(float));
    out->n = n;
    out->rate = FSI;
    out->peak_pa = mx.peakPa;
    out->spl = mx.peakPa > 0 ? 20 * log10(mx.peakPa / 2e-5) : 0;
    for (i = 0; i < sc.n_ev; i++)
        if (sc.ev[i].t - mx.tStart < n / FS) {
            out->events[ne].kind = sc.ev[i].kind;
            out->events[ne].t = sc.ev[i].t - mx.tStart;
            ne++;
        }
    sort_events(out->events, ne);
    out->n_events = ne;
    ar_free(&ar);
    return BE_OK;
}

/* ------------------------------------------------------------------ Ballistic's finishing for action */
static float *stretch(arena *ar, const float *x, int M, double s, int *n_out)
{
    int N = (int)floor(M * s), j;
    float *y = fnew(ar, N);
    for (j = 0; j < N; j++) {
        double u = j / s, f, a, b, c, d, c1, c2, c3;
        int i = (int)floor(u);
        f = u - i;
        a = x[i > 0 ? i - 1 : 0]; b = x[i]; c = x[i + 1 < M ? i + 1 : M - 1]; d = x[i + 2 < M ? i + 2 : M - 1];
        c1 = 0.5 * (c - a); c2 = a - 2.5 * b + 2 * c - 0.5 * d; c3 = 0.5 * (d - a) + 1.5 * (b - c);
        y[j] = (float)(((c3 * f + c2) * f + c1) * f + b);
    }
    *n_out = N;
    return y;
}

typedef struct { int n; double level; } onset_t;

/* onsets from sample 'from' on; the samples before it only warm the detector */
static int find_onsets(const float *L, const float *R, int N, int from, onset_t *out5)
{
    double pk = peak_of(L, R, N), fast = 0, slow = 0, aF = exp(-1 / (0.012 * FS)), aSa = exp(-1 / (0.03 * FS));
    double aSr = exp(-1 / (0.25 * FS));
    int win = (int)js_round(0.02 * FS), n, j, cnt = 0, cap = 64, i, keep;
    double last = -1e9;
    onset_t *all = (onset_t *)malloc(cap * sizeof *all);
    if (!all)
        return 0;
    for (n = 0; n < N; n++) {
        double x = maxd(fabs(L[n]), fabs(R[n]));
        fast = x > fast ? x : x + (fast - x) * aF;
        slow = x > slow ? x + (slow - x) * aSa : x + (slow - x) * aSr;
        if (n >= from && fast > 0.2 * pk && fast > 2.5 * slow && n - last > 0.14 * FS) {
            double lv = 0;
            for (j = n; j < n + win && j < N; j++)
                lv = maxd(lv, maxd(fabs(L[j]), fabs(R[j])));
            if (cnt == cap) {
                onset_t *na = (onset_t *)realloc(all, (cap *= 2) * sizeof *na);
                if (!na) { free(all); return 0; }
                all = na;
            }
            all[cnt].n = n;
            all[cnt].level = lv;
            cnt++;
            last = n;
        }
    }
    for (i = 1; i < cnt; i++) {               /* by level, loudest first, stable */
        onset_t x = all[i];
        for (j = i - 1; j >= 0 && all[j].level < x.level; j--)
            all[j + 1] = all[j];
        all[j + 1] = x;
    }
    keep = cnt < 5 ? cnt : 5;
    for (i = 1; i < keep; i++) {              /* then in time */
        onset_t x = all[i];
        for (j = i - 1; j >= 0 && all[j].n > x.n; j--)
            all[j + 1] = all[j];
        all[j + 1] = x;
    }
    memcpy(out5, all, keep * sizeof *all);
    free(all);
    return keep;
}

static void transient(float *L, float *R, int N, double amt)
{
    double f = 0, s = 0, pk = peak_of(L, R, N), eps = 1e-3 * pk, pw = 0.9 * amt;
    double aFa = exp(-1 / (0.0005 * FS)), aFr = exp(-1 / (0.03 * FS)), aSa = exp(-1 / (0.02 * FS)), aSr = exp(-1 / (0.2 * FS));
    int n;
    for (n = 0; n < N; n++) {
        double x = maxd(fabs(L[n]), fabs(R[n])), g;
        f = x > f ? x + (f - x) * aFa : x + (f - x) * aFr;
        s = x > s ? x + (s - x) * aSa : x + (s - x) * aSr;
        g = clampd(pow((f + eps) / (s + eps), pw), 0.6, 4);
        L[n] = (float)(L[n] * g);
        R[n] = (float)(R[n] * g);
    }
}

static void compress(float *L, float *R, int N, double amt)
{
    double thr = db_to_lin(-10 - 14 * amt), ratio = 1 + 5 * amt, env = 0, att = exp(-1 / (0.003 * FS));
    double rel = exp(-1 / (0.14 * FS)), ex = 1 / ratio - 1;
    int n;
    for (n = 0; n < N; n++) {
        double x = maxd(fabs(L[n]), fabs(R[n]));
        env = x > env ? x + (env - x) * att : x + (env - x) * rel;
        if (env > thr) {
            double g = pow(env / thr, ex);
            L[n] = (float)(L[n] * g);
            R[n] = (float)(R[n] * g);
        }
    }
}

static void limiter(arena *ar, float *L, float *R, int N, double ceil_)
{
    float *h = fnew(ar, N);
    double att = 1 - exp(-1 / (0.0015 * FS)), rel = 1 - exp(-1 / (0.08 * FS)), nx = 1, y = 1;
    int n;
    for (n = N - 1; n >= 0; n--) {
        double x = maxd(fabs(L[n]), fabs(R[n])), gr = x > ceil_ ? ceil_ / x : 1;
        nx = mind(gr, nx + (1 - nx) * att);
        h[n] = (float)nx;
    }
    for (n = 0; n < N; n++) {
        y = mind(h[n], y + (1 - y) * rel);
        L[n] = (float)(L[n] * y);
        R[n] = (float)(R[n] * y);
    }
}

static const double TAIL_DELAYS[8] = { 61, 89, 127, 173, 229, 283, 349, 419 };

static void norm_to(float *L, float *R, int N, int a0, int b0, double target)
{
    double pk = 0, v;
    int q;
    for (q = a0; q < b0 && q < N; q++) {
        v = fabs(L[q]); if (v > pk) pk = v;
        v = fabs(R[q]); if (v > pk) pk = v;
    }
    if (pk > 0) {
        scale(L, N, target / pk);
        scale(R, N, target / pk);
    }
}

/* a loop is processed as three periods and the middle one kept, so every stage sees a steady state */
static int finish(be_sound *s, const char *preset, const char *const *keys, const double *values, int nk, int loop)
{
    double a[A_COUNT], slow, sigPk;
    arena ar;
    float *L, *R;
    int N, n, i, j, non, a0;
    volatile int pad = 0;
    onset_t on[15];
    if (!s || !s->L)
        return BE_ERR_VALUE;
    memcpy(a, ACTION_DEFAULTS, sizeof a);
    if (preset && *preset) {
        for (i = 0; i < ACTION_COUNT && strcmp(ACTIONS[i].id, preset); i++)
            ;
        if (i == ACTION_COUNT)
            return BE_ERR_PRESET;
        memcpy(a, ACTIONS[i].a, sizeof a);
    }
    for (j = 0; j < nk; j++) {
        for (i = 0; i < A_COUNT && strcmp(ACTION_KEYS[i], keys[j]); i++)
            ;
        if (i == A_COUNT)
            return BE_ERR_KEY;
        if (values[j] != values[j])
            return BE_ERR_VALUE;
        a[i] = values[j];
    }
    if (!(peak_of(s->L, s->R, s->n) > 0))
        return BE_OK;
    memset(&ar, 0, sizeof ar);
    if (setjmp(ar.fail)) {
        ar_free(&ar);
        return BE_ERR_MEMORY;
    }
    slow = clampd(a[A_slowmo] ? a[A_slowmo] : 1, 1, 4);
    {
        const float *sL = loop ? tile3(&ar, s->L, s->n) : s->L, *sR = loop ? tile3(&ar, s->R, s->n) : s->R;
        int sn = loop ? 3 * s->n : s->n;
        if (slow > 1.01) {
            L = stretch(&ar, sL, sn, slow, &N);
            R = stretch(&ar, sR, sn, slow, &N);
        } else {
            slow = 1;
            N = sn;
            L = fcopy(&ar, sL, N);
            R = fcopy(&ar, sR, N);
        }
    }
    a0 = loop ? (int)js_round(s->n * slow) : 0;
    if (!loop && N > 24 * FSI)
        N = 24 * FSI;
    if (a[A_tail] > 0 && !loop) {
        int extra = (int)mind(24 * FSI + 4 * FSI - N, js_round((1 + a[A_tail] * 3) * FS));
        float *nl = fnew(&ar, N + extra), *nr = fnew(&ar, N + extra);
        memcpy(nl, L, (size_t)N * sizeof(float));
        memcpy(nr, R, (size_t)N * sizeof(float));
        L = nl;
        R = nr;
        N += extra;
    }
    if (loop) {
        /* a loop's onsets come from the kept period, heard after one period of warm-up, and repeat in every
           period, so a sub drop near the end wraps round the seam */
        onset_t o5[5];
        int Pl = (int)js_round(s->n * slow), k, m = find_onsets(L, R, 2 * Pl, Pl, o5);
        non = 0;
        for (i = 0; i < m; i++)
            for (k = 0; k < 3; k++) {
                on[non].n = o5[i].n - Pl + k * Pl;
                on[non].level = o5[i].level;
                non++;
            }
    } else
        non = find_onsets(L, R, N, 0, on);
    if (a[A_punch] > 0)
        transient(L, R, N, a[A_punch]);
    if (a[A_weight] > 0) {
        biquad(L, N, BQ_LS, 110, 0.7, 8 * a[A_weight]);
        biquad(R, N, BQ_LS, 110, 0.7, 8 * a[A_weight]);
        for (i = 0; i < non; i++) {
            double ph = 0;
            int lim = (int)mind(N - on[i].n, js_round(0.9 * FS));
            for (j = 0; j < lim; j++) {
                double t = j / FS, sv;
                ph += TAU * (36 + 80 * exp(-t / 0.05)) / FS;
                sv = sin(ph) * exp(-t / 0.2) * (1 - exp(-t / 0.003)) * on[i].level * 0.9 * a[A_weight];
                L[on[i].n + j] = (float)(L[on[i].n + j] + sv);
                R[on[i].n + j] = (float)(R[on[i].n + j] + sv);
            }
        }
    }
    norm_to(L, R, N, a0, loop ? 2 * a0 : N, 1);
    if (a[A_drive] > 0) {
        double dg = 1 + 7 * a[A_drive], bias = 0.12 * a[A_drive], tb = tanh(bias), tn = tanh(dg);
        for (n = 0; n < N; n++) {
            L[n] = (float)((tanh(dg * L[n] + bias) - tb) / tn);
            R[n] = (float)((tanh(dg * R[n] + bias) - tb) / tn);
        }
    }
    if (a[A_bite] > 0) {
        biquad(L, N, BQ_PK, 3200, 0.9, 9 * a[A_bite]);
        biquad(L, N, BQ_HS, 8000, 0.7, 4 * a[A_bite]);
        biquad(R, N, BQ_PK, 3200, 0.9, 9 * a[A_bite]);
        biquad(R, N, BQ_HS, 8000, 0.7, 4 * a[A_bite]);
    }
    sigPk = peak_of(L, R, N);
    if (!sigPk)
        sigPk = 1;
    if (a[A_tail] > 0) {
        float *tl = fcopy(&ar, L, N), *tr = fcopy(&ar, R, N), *wL, *wR;
        fdn_opt fo;
        double wg = 0.55 * a[A_tail];
        one_pole_hp(tl, N, 140); one_pole_hp(tr, N, 140); one_pole_lp(tl, N, 6000); one_pole_lp(tr, N, 6000);
        memset(&fo, 0, sizeof fo);
        fo.n_delays = 8;
        memcpy(fo.delays, TAIL_DELAYS, sizeof fo.delays);
        fo.damp = 2200; fo.pre = 35; fo.rt60 = 1.2 + 4 * a[A_tail];
        fdn(&ar, tl, tr, N, &fo, &wL, &wR);
        for (n = 0; n < N; n++) {
            L[n] = (float)(L[n] + wL[n] * wg);
            R[n] = (float)(R[n] + wR[n] * wg);
        }
    }
    if (a[A_swell] > 0 && !loop) {
        int anchor = non ? on[0].n : 0, Ls, exN, start;
        float *xl, *xr, *wL, *wR, *sL, *sR;
        fdn_opt fo;
        double sp, sg;
        if (!non) {
            double pv = 0;
            for (n = 0; n < N; n++) {
                double av = fabs(L[n]) + fabs(R[n]);
                if (av > pv) { pv = av; anchor = n; }
            }
        }
        Ls = (int)js_round((0.35 + 0.9 * a[A_swell]) * FS);
        exN = (int)js_round(0.3 * FS);
        xl = fnew(&ar, Ls);
        xr = fnew(&ar, Ls);
        for (i = 0; i < exN && i < Ls && anchor + i < N; i++) {
            double fw = 1 - (double)i / exN;
            xl[i] = (float)(L[anchor + i] * fw);
            xr[i] = (float)(R[anchor + i] * fw);
        }
        memset(&fo, 0, sizeof fo);
        fo.n_delays = 8;
        for (i = 0; i < 8; i++)
            fo.delays[i] = TAIL_DELAYS[i] * 0.6;
        fo.damp = 3500; fo.pre = 0; fo.rt60 = 2.2;
        fdn(&ar, xl, xr, Ls, &fo, &wL, &wR);
        one_pole_hp(wL, Ls, 160);
        one_pole_hp(wR, Ls, 160);
        sL = fnew(&ar, Ls);
        sR = fnew(&ar, Ls);
        for (i = 0; i < Ls; i++) {
            double e = pow((double)i / Ls, 2.2);
            sL[i] = (float)(wL[Ls - 1 - i] * e);
            sR[i] = (float)(wR[Ls - 1 - i] * e);
        }
        sp = peak_of(sL, sR, Ls);
        if (!sp)
            sp = 1;
        sg = 0.55 * a[A_swell] * sigPk / sp;
        start = anchor - (int)js_round(0.003 * FS) - Ls;
        if (start < 0) {
            float *nl = fnew(&ar, N - start), *nr = fnew(&ar, N - start);
            pad = -start;
            memcpy(nl + pad, L, (size_t)N * sizeof(float));
            memcpy(nr + pad, R, (size_t)N * sizeof(float));
            L = nl;
            R = nr;
            start = 0;
            N += pad;
        }
        for (i = 0; i < Ls; i++) {
            L[start + i] = (float)(L[start + i] + sL[i] * sg);
            R[start + i] = (float)(R[start + i] + sR[i] * sg);
        }
    }
    if (a[A_width] > 0) {
        double wd = 1 + 1.2 * a[A_width];
        for (n = 0; n < N; n++) {
            double m = (L[n] + R[n]) * 0.5, sd = (L[n] - R[n]) * 0.5 * wd;
            L[n] = (float)(m + sd);
            R[n] = (float)(m - sd);
        }
    }
    norm_to(L, R, N, a0, loop ? 2 * a0 : N, 1);
    if (a[A_loud] > 0) {
        compress(L, R, N, a[A_loud]);
        norm_to(L, R, N, a0, loop ? 2 * a0 : N, db_to_lin(7 * a[A_loud]));
    }
    limiter(&ar, L, R, N, 0.89);
    if (loop) {
        n = a0;
        L += a0;
        R += a0;
        s->n_events = 0;
    } else
        n = trim_tail(L, R, N);
    {
        float *nl = (float *)malloc((size_t)(n ? n : 1) * sizeof(float)), *nr = (float *)malloc((size_t)(n ? n : 1) * sizeof(float));
        if (!nl || !nr) {
            free(nl);
            free(nr);
            longjmp(ar.fail, 1);
        }
        memcpy(nl, L, (size_t)n * sizeof(float));
        memcpy(nr, R, (size_t)n * sizeof(float));
        free(s->L);
        free(s->R);
        s->L = nl;
        s->R = nr;
        s->n = n;
    }
    for (i = 0; i < s->n_events; i++)
        s->events[i].t = s->events[i].t * slow + pad / FS;
    ar_free(&ar);
    return BE_OK;
}

int be_finish(be_sound *s, const char *preset, const char *const *keys, const double *values, int nk)
{
    return finish(s, preset, keys, values, nk, 0);
}

int be_finish_loop(be_sound *s, const char *preset, const char *const *keys, const double *values, int nk)
{
    return finish(s, preset, keys, values, nk, 1);
}

/* ------------------------------------------------------------------ small things */
void be_cut(be_sound *s, double seconds, double fade)
{
    int n, f, i;
    if (!s || !s->L)
        return;
    n = (int)js_round(seconds * FS);
    if (n < 1) n = 1;
    if (n >= s->n)
        return;
    f = (int)js_round(fade * FS);
    if (f > n) f = n;
    for (i = 0; i < f; i++) {
        double g = (double)i / f;
        s->L[n - 1 - i] = (float)(s->L[n - 1 - i] * g);
        s->R[n - 1 - i] = (float)(s->R[n - 1 - i] * g);
    }
    s->n = n;
}

void be_set_peak(be_sound *s, double peak)
{
    double pk;
    if (!s || !s->L)
        return;
    pk = peak_of(s->L, s->R, s->n);
    if (pk > 0) {
        scale(s->L, s->n, peak / pk);
        scale(s->R, s->n, peak / pk);
    }
}

void be_sound_free(be_sound *s)
{
    if (!s)
        return;
    free(s->L);
    free(s->R);
    free(s->events);
    s->L = s->R = 0;
    s->events = 0;
    s->n = s->n_events = 0;
}

static void put16(unsigned char *b, unsigned v) { b[0] = (unsigned char)(v & 255); b[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *b, unsigned long v)
{
    b[0] = (unsigned char)(v & 255); b[1] = (unsigned char)(v >> 8 & 255);
    b[2] = (unsigned char)(v >> 16 & 255); b[3] = (unsigned char)(v >> 24 & 255);
}

int be_write_wav(const be_sound *s, const char *path, int bits)
{
    FILE *f;
    unsigned char h[44];
    unsigned long data;
    int i, bps = bits == 32 ? 4 : 2;
    if (!s || !s->L || !path)
        return BE_ERR_VALUE;
    f = fopen(path, "wb");
    if (!f)
        return BE_ERR_IO;
    data = (unsigned long)s->n * 2 * bps;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, bps == 4 ? 3 : 1); put16(h + 22, 2); put32(h + 24, FSI); put32(h + 28, (unsigned long)FSI * 2 * bps);
    put16(h + 32, (unsigned)(2 * bps)); put16(h + 34, (unsigned)(8 * bps)); memcpy(h + 36, "data", 4); put32(h + 40, data);
    fwrite(h, 1, 44, f);
    for (i = 0; i < s->n; i++) {
        float ch[2];
        int c;
        ch[0] = s->L[i];
        ch[1] = s->R[i];
        for (c = 0; c < 2; c++) {
            unsigned char b[4];
            if (bps == 4) {
                uint32_t u;
                memcpy(&u, &ch[c], 4);
                put32(b, u);
            } else {
                double v = ch[c] > 1 ? 1 : ch[c] < -1 ? -1 : ch[c];
                long q = (long)js_round(v * 32767);
                put16(b, (unsigned)(q & 0xffff));
            }
            fwrite(b, 1, (size_t)bps, f);
        }
    }
    if (fclose(f))
        return BE_ERR_IO;
    return BE_OK;
}
