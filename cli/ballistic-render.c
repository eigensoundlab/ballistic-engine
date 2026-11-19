/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Daniel Sandner */
/* ballistic-render: render one of Ballistic's sounds to a WAV file.

     ballistic-render --preset pistol --set D=0 --set env=indoor --action game --seed 3 -o pistol.wav
     ballistic-render --list                    the presets, and which this version renders
     ballistic-render --keys                    the parameters

   Options: --preset ID, --set KEY=VALUE (repeat), --seed N (1), --action PRESET, --action-set KEY=VALUE,
   --length SECONDS (cut with a 40 ms fade), --peak 0..1, --bits 16|32, --raw (32-bit float, L R interleaved,
   no header: for tests), --time (print how long the render took), -o FILE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ballistic.h"

static int fail(const char *what, int st)
{
    fprintf(stderr, "ballistic-render: %s: %s\n", what, be_status_text(st));
    return 2;
}

static double now(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}

int main(int argc, char **argv)
{
    be_params *p = be_params_new();
    const char *out = 0, *action = 0, *akeys[16];
    double avals[16], length = 0, peak = 0, t0, t1;
    int i, seed = 1, bits = 16, raw = 0, timing = 0, na = 0, st, events = 0;
    be_sound s;
    if (!p)
        return fail("start", BE_ERR_MEMORY);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "--list")) {
            int k;
            for (k = 0; k < be_preset_count(); k++)
                printf("%-14s %-12s %s%s\n", be_preset_id(k), be_preset_kind(k), be_preset_name(k),
                       be_kind_supported(be_preset_kind(k)) ? "" : "   (not yet)");
            return 0;
        } else if (!strcmp(a, "--keys")) {
            int k;
            for (k = 0; k < be_key_count(); k++)
                printf("%s%s\n", be_key_name(k), be_key_is_text(k) ? " (word)" : "");
            return 0;
        } else if (!strcmp(a, "--version")) {
            printf("ballistic-engine %s, following %s\n", be_version(), be_reference());
            return 0;
        } else if (!strcmp(a, "--raw")) {
            raw = 1;
        } else if (!strcmp(a, "--time")) {
            timing = 1;
        } else if (!strcmp(a, "--events")) {
            events = 1;
        } else if (!v) {
            fprintf(stderr, "ballistic-render: %s needs a value\n", a);
            return 2;
        } else if (!strcmp(a, "--preset")) {
            if ((st = be_params_preset(p, v)))
                return fail(v, st);
            i++;
        } else if (!strcmp(a, "--set") || !strcmp(a, "--action-set")) {
            char key[64];
            const char *eq = strchr(v, '=');
            if (!eq || eq - v >= (int)sizeof key) {
                fprintf(stderr, "ballistic-render: %s wants KEY=VALUE\n", a);
                return 2;
            }
            memcpy(key, v, eq - v);
            key[eq - v] = 0;
            if (!strcmp(a, "--set")) {
                if ((st = be_params_set_text(p, key, eq + 1)))
                    return fail(key, st);
            } else if (na < 16) {
                char *copy = (char *)malloc(strlen(key) + 1);
                if (!copy)
                    return fail("start", BE_ERR_MEMORY);
                strcpy(copy, key);
                akeys[na] = copy;
                avals[na++] = atof(eq + 1);
            }
            i++;
        } else if (!strcmp(a, "--seed")) {
            seed = atoi(v); i++;
        } else if (!strcmp(a, "--action")) {
            action = v; i++;
        } else if (!strcmp(a, "--length")) {
            length = atof(v); i++;
        } else if (!strcmp(a, "--peak")) {
            peak = atof(v); i++;
        } else if (!strcmp(a, "--bits")) {
            bits = atoi(v); i++;
        } else if (!strcmp(a, "-o")) {
            out = v; i++;
        } else {
            fprintf(stderr, "ballistic-render: unknown option %s (see the comment at the top of cli/ballistic-render.c)\n", a);
            return 2;
        }
    }
    if (!out) {
        fprintf(stderr, "ballistic-render: say where to write it: -o FILE\n");
        return 2;
    }
    t0 = now();
    if ((st = be_render(p, seed, &s)))
        return fail("render", st);
    if ((action || na) && (st = be_finish(&s, action, akeys, avals, na)))
        return fail("action", st);
    t1 = now();
    if (length > 0)
        be_cut(&s, length, 0.04);
    if (peak > 0)
        be_set_peak(&s, peak);
    if (raw) {
        FILE *f = fopen(out, "wb");
        int k;
        if (!f)
            return fail(out, BE_ERR_IO);
        for (k = 0; k < s.n; k++) {
            fwrite(&s.L[k], 4, 1, f);
            fwrite(&s.R[k], 4, 1, f);
        }
        fclose(f);
    } else if ((st = be_write_wav(&s, out, bits)))
        return fail(out, st);
    if (timing)
        printf("%.3f s of sound in %.2f ms\n", (double)s.n / s.rate, (t1 - t0) * 1000);
    if (events) {
        static const char *names[] = { "launch", "crack", "pass", "impact" };
        int k;
        for (k = 0; k < s.n_events; k++)
            printf("%s %.6f\n", names[s.events[k].kind], s.events[k].t);
    }
    be_sound_free(&s);
    be_params_free(p);
    return 0;
}
