/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Daniel Sandner */
/* The sound engine of Ballistic Foley Lab, in C.

   A port of reference/engine.js, kept line for line where it can be: the same order of operations, the same
   random draws in the same order, doubles where the JavaScript computes and floats where it stores (Float32Array),
   JavaScript's own Math.round and Math.hypot. tests/conformance.mjs renders every preset both ways and compares.
   Keep it that way: a change here is a change in Ballistic first.

   Kinds so far: projectile (shots, arrows, ricochets, swings, with an explosive payload) and explosion. */
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
typedef struct { int nwave; double A, T, k; } shape_t;

static double shape_at(const shape_t *s, double t)
{
    double u;
    if (s->nwave)
        return s->A * (1 - 2 * t / s->T);
    u = t / s->T;
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
    shape_t s1, s2;
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
    shape_t s;
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
        shape_t s;
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

/* ------------------------------------------------------------------ projectile */
typedef struct { double g1, g2, g3, g4, t0, pan, ground; } gains_t;

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
    if (kind_of(PS(p, kind)) == 1 && !strcmp(PS(p, exSurface), "rec")) {
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
        } else
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
    } else {
        if (!(PV(p, exW) > 0) || !(PV(p, exDist) > 0))
            return BE_ERR_VALUE;
        if (!strcmp(PS(p, exSurface), "rec") && strcmp(PS(p, grAsset), "ex:gravel"))
            return BE_ERR_UNSUPPORTED;
    }
    return BE_OK;
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

int be_render(const be_params *p, int seed, be_sound *out)
{
    arena ar;
    scene_t sc;
    mix_t mx;
    double pk;
    int st, n, i, ne = 0;
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
    mix_scene(&sc, p, seed, &mx);
    pk = peak_of(mx.L, mx.R, mx.N);
    if (!pk)
        pk = 1;
    scale(mx.L, mx.N, 1 / pk);
    scale(mx.R, mx.N, 1 / pk);
    n = trim_tail(mx.L, mx.R, mx.N);
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

static int find_onsets(const float *L, const float *R, int N, onset_t *out5)
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
        if (fast > 0.2 * pk && fast > 2.5 * slow && n - last > 0.14 * FS) {
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

static void normalize(float *L, float *R, int N, double target)
{
    double pk = peak_of(L, R, N);
    if (pk > 0) {
        scale(L, N, target / pk);
        scale(R, N, target / pk);
    }
}

static const double TAIL_DELAYS[8] = { 61, 89, 127, 173, 229, 283, 349, 419 };

int be_finish(be_sound *s, const char *preset, const char *const *keys, const double *values, int nk)
{
    double a[A_COUNT], slow, sigPk;
    arena ar;
    float *L, *R;
    int N, n, i, j, non;
    volatile int pad = 0;
    onset_t on[5];
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
    if (slow > 1.01) {
        L = stretch(&ar, s->L, s->n, slow, &N);
        R = stretch(&ar, s->R, s->n, slow, &N);
    } else {
        slow = 1;
        N = s->n;
        L = fcopy(&ar, s->L, N);
        R = fcopy(&ar, s->R, N);
    }
    if (N > 24 * FSI)
        N = 24 * FSI;
    if (a[A_tail] > 0) {
        int extra = (int)mind(24 * FSI + 4 * FSI - N, js_round((1 + a[A_tail] * 3) * FS));
        float *nl = fnew(&ar, N + extra), *nr = fnew(&ar, N + extra);
        memcpy(nl, L, (size_t)N * sizeof(float));
        memcpy(nr, R, (size_t)N * sizeof(float));
        L = nl;
        R = nr;
        N += extra;
    }
    non = find_onsets(L, R, N, on);
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
    normalize(L, R, N, 1);
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
    if (a[A_swell] > 0) {
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
    normalize(L, R, N, 1);
    if (a[A_loud] > 0) {
        compress(L, R, N, a[A_loud]);
        normalize(L, R, N, db_to_lin(7 * a[A_loud]));
    }
    limiter(&ar, L, R, N, 0.89);
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
