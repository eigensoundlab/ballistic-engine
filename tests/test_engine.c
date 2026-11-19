/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Daniel Sandner */
/* The engine on its own: every preset it renders comes out whole, the same seed gives the same take, parameters by
   name, errors in words, the finishing, the small helpers. The comparison with the JavaScript engine sample for
   sample is tests/conformance.mjs. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ballistic.h"

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static double peak(const be_sound *s)
{
    double p = 0;
    int i;
    for (i = 0; i < s->n; i++) {
        if (fabs(s->L[i]) > p) p = fabs(s->L[i]);
        if (fabs(s->R[i]) > p) p = fabs(s->R[i]);
    }
    return p;
}

static int finite_all(const be_sound *s)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (!(s->L[i] == s->L[i]) || !(s->R[i] == s->R[i]) || isinf(s->L[i]) || isinf(s->R[i]))
            return 0;
    return 1;
}

static void every_preset(void)
{
    int i, rendered = 0, later = 0;
    be_params *p = be_params_new();
    for (i = 0; i < be_preset_count(); i++) {
        be_sound s;
        int st;
        CHECK(be_params_preset(p, be_preset_id(i)) == BE_OK, "preset %s", be_preset_id(i));
        st = be_render(p, 3, &s);
        if (!be_kind_supported(be_preset_kind(i))) {
            CHECK(st == BE_ERR_KIND || st == BE_ERR_UNSUPPORTED, "%s: a kind not ported yet says so (%d)", be_preset_id(i), st);
            later++;
            continue;
        }
        CHECK(st == BE_OK, "%s renders: %s", be_preset_id(i), be_status_text(st));
        if (st)
            continue;
        rendered++;
        CHECK(s.rate == 48000 && s.n > 4800 && s.n < 24 * 48000, "%s: %d samples", be_preset_id(i), s.n);
        CHECK(finite_all(&s), "%s: no NaN or infinity", be_preset_id(i));
        CHECK(fabs(peak(&s) - 1) < 1e-6, "%s: peak 1, not %g", be_preset_id(i), peak(&s));
        CHECK(s.n_events >= 1 && s.peak_pa > 0 && s.spl > 30, "%s: events and level", be_preset_id(i));
        {
            int k;
            for (k = 1; k < s.n_events; k++)
                CHECK(s.events[k].t >= s.events[k - 1].t, "%s: events in time", be_preset_id(i));
        }
        CHECK(be_finish(&s, "game", 0, 0, 0) == BE_OK && finite_all(&s) && peak(&s) <= 0.8901,
              "%s: game finishing, peak %g", be_preset_id(i), peak(&s));
        be_sound_free(&s);
    }
    CHECK(rendered >= 16, "%d presets rendered", rendered);
    printf("%d presets render, %d wait for later versions\n", rendered, later);
    be_params_free(p);
}

static void seeds(void)
{
    be_params *p = be_params_new();
    be_sound a, b, c;
    be_params_preset(p, "pistol");
    be_render(p, 5, &a);
    be_render(p, 5, &b);
    be_render(p, 6, &c);
    CHECK(a.n == b.n && !memcmp(a.L, b.L, a.n * sizeof(float)), "the same seed, the same take");
    CHECK(a.n != c.n || memcmp(a.L, c.L, a.n * sizeof(float)), "another seed, another take");
    be_sound_free(&a);
    be_sound_free(&b);
    be_sound_free(&c);
    be_params_free(p);
}

static void params(void)
{
    be_params *p = be_params_new(), *q;
    double v = 0;
    be_sound s;
    CHECK(be_params_get(p, "D", &v) == BE_OK && v == 300, "default D is 300, not %g", v);
    CHECK(!strcmp(be_params_get_text(p, "env"), "field"), "default env");
    CHECK(be_params_set(p, "D", 12) == BE_OK && be_params_get(p, "D", &v) == BE_OK && v == 12, "set D");
    CHECK(be_params_set_text(p, "miss", "2.5") == BE_OK && be_params_get(p, "miss", &v) == BE_OK && v == 2.5, "a number as text");
    CHECK(be_params_set_text(p, "swing", "true") == BE_OK && be_params_get(p, "swing", &v) == BE_OK && v == 1, "a flag");
    CHECK(be_params_set(p, "Dee", 1) == BE_ERR_KEY, "an unknown key");
    CHECK(be_params_set(p, "env", 1) == BE_ERR_VALUE, "a number for a word");
    CHECK(be_params_set_text(p, "D", "near") == BE_ERR_VALUE, "a word for a number");
    CHECK(be_params_preset(p, "nope") == BE_ERR_PRESET, "an unknown preset");
    q = be_params_copy(p);
    CHECK(q && be_params_get(q, "D", &v) == BE_OK && v == 12, "a copy");
    be_params_preset(q, "pistol");
    be_params_set_text(q, "target", "cheese");
    CHECK(be_render(q, 1, &s) == BE_ERR_VALUE, "an unknown target material");
    be_params_preset(q, "tesla");
    CHECK(be_render(q, 1, &s) == BE_ERR_KIND, "a kind not ported yet");
    be_params_preset(q, "pistol");
    be_params_set_text(q, "env", "ir");
    CHECK(be_render(q, 1, &s) == BE_ERR_UNSUPPORTED, "a recorded room");
    be_params_set_text(q, "env", "nowhere");
    CHECK(be_render(q, 1, &s) == BE_OK, "an unknown room is no room, as in Ballistic");
    be_sound_free(&s);
    CHECK(be_kind_supported("projectile") && be_kind_supported("explosion") && !be_kind_supported("rotor"), "kinds");
    CHECK(strlen(be_status_text(BE_ERR_KIND)) > 10 && be_version()[0] == '0', "words for statuses, a version");
    be_params_free(p);
    be_params_free(q);
}

static void finishing_and_helpers(void)
{
    be_params *p = be_params_new();
    be_sound s;
    const char *keys[] = { "slowmo", "tail" };
    double vals[] = { 2, 0.5 };
    const char *bad[] = { "oomph" };
    double one = 1;
    int n0;
    FILE *f;
    unsigned char h[44];
    be_params_preset(p, "pistol");
    be_render(p, 2, &s);
    n0 = s.n;
    CHECK(be_finish(&s, "slow", keys, vals, 2) == BE_OK && s.n > 1.8 * n0, "slow motion is longer: %d from %d", s.n, n0);
    CHECK(be_finish(&s, "loudest", 0, 0, 0) == BE_ERR_PRESET, "an unknown action preset");
    CHECK(be_finish(&s, 0, bad, &one, 1) == BE_ERR_KEY, "an unknown action key");
    be_cut(&s, 0.5, 0.04);
    CHECK(s.n == 24000 && s.L[s.n - 1] == 0, "cut to 0.5 s with a fade");
    be_set_peak(&s, 0.25);
    CHECK(fabs(peak(&s) - 0.25) < 1e-6, "peak set");
    CHECK(be_write_wav(&s, "test-pistol.wav", 16) == BE_OK, "a WAV");
    f = fopen("test-pistol.wav", "rb");
    CHECK(f && fread(h, 1, 44, f) == 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) &&
          h[22] == 2 && (h[24] | h[25] << 8 | h[26] << 16) == 48000 && h[34] == 16, "its header");
    if (f)
        fclose(f);
    remove("test-pistol.wav");
    be_sound_free(&s);
    be_params_free(p);
}

int main(void)
{
    every_preset();
    seeds();
    params();
    finishing_and_helpers();
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
