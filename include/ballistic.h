/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Daniel Sandner */
/* Ballistic Engine: the sound engine of Ballistic Foley Lab in portable C.

   Physically modelled sound events - shots, ricochets, blasts, impacts - rendered from parameters, the same as the
   JavaScript engine renders them (reference/engine.js), sample for sample within rounding. No dependencies beyond
   the C standard library and libm; no global state, so any number of threads may render at once.

     be_params *p = be_params_new();
     be_params_preset(p, "pistol");
     be_params_set(p, "D", 0);                  // the listener is the shooter
     be_params_set_text(p, "env", "indoor");
     be_sound s;
     if (be_render(p, 7, &s) == BE_OK) {        // seed 7: one take; another seed, another take
         be_finish(&s, "game", 0, 0, 0);        // Ballistic's finishing for games (optional)
         be_write_wav(&s, "pistol.wav", 16);
         be_sound_free(&s);
     }
     be_params_free(p);

   Sounds are stereo, 48 kHz, 32-bit float. */
#ifndef BALLISTIC_ENGINE_H
#define BALLISTIC_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(BE_BUILD_SHARED)
#define BE_API __declspec(dllexport)
#elif defined(_WIN32) && defined(BE_USE_SHARED)
#define BE_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(BE_BUILD_SHARED)
#define BE_API __attribute__((visibility("default")))
#else
#define BE_API
#endif

#define BE_RATE 48000

enum be_status {
    BE_OK = 0,
    BE_ERR_KEY = 1,          /* no parameter of that name */
    BE_ERR_VALUE = 2,        /* a value the parameter cannot take */
    BE_ERR_PRESET = 3,       /* no preset of that name */
    BE_ERR_KIND = 4,         /* a kind of sound this version cannot render yet (see be_kind_supported) */
    BE_ERR_UNSUPPORTED = 5,  /* needs a recording (a recorded room, events triggered by a recording) */
    BE_ERR_MEMORY = 6,
    BE_ERR_IO = 7
};

enum be_event_kind { BE_EVENT_LAUNCH = 0, BE_EVENT_CRACK = 1, BE_EVENT_PASS = 2, BE_EVENT_IMPACT = 3 };

typedef struct { int kind; double t; } be_event;   /* t: seconds from the start of the sound */

typedef struct {
    float *L, *R;            /* n samples each */
    int n, rate;
    double peak_pa;          /* the loudest point at the listener before normalising, in pascals */
    double spl;              /* the same in dB SPL */
    be_event *events;        /* what happens when: the shot leaves, the crack, the pass, the impact */
    int n_events;
} be_sound;

typedef struct be_params be_params;

/* Parameters: Ballistic's own names (see README.md); numbers and words ("env": "indoor"). */
BE_API be_params *be_params_new(void);                          /* Ballistic's defaults */
BE_API be_params *be_params_copy(const be_params *p);
BE_API void be_params_free(be_params *p);
BE_API void be_params_reset(be_params *p);
BE_API int be_params_preset(be_params *p, const char *id);      /* the defaults, then the preset's values */
BE_API int be_params_set(be_params *p, const char *key, double value);
BE_API int be_params_set_text(be_params *p, const char *key, const char *value);
BE_API int be_params_get(const be_params *p, const char *key, double *value);
BE_API const char *be_params_get_text(const be_params *p, const char *key);   /* NULL for a number */

/* Rendering. The physics, peak-normalised to 1, the silent tail cut. Thread-safe. */
BE_API int be_render(const be_params *p, int seed, be_sound *out);
BE_API int be_kind_supported(const char *kind);                 /* 1 if this version renders that kind */

/* Ballistic's finishing for action, in place: an action preset ("game", "block", "slow", "far", "none" or NULL),
   then n named action parameters over it (punch, weight, drive, bite, tail, swell, width, loud, slowmo). */
BE_API int be_finish(be_sound *s, const char *preset, const char *const *keys, const double *values, int n);

/* Small things a game wants: cut to a length with a short fade, set the peak, write a WAV (16 or 32 bits). */
BE_API void be_cut(be_sound *s, double seconds, double fade);
BE_API void be_set_peak(be_sound *s, double peak);
BE_API int be_write_wav(const be_sound *s, const char *path, int bits);
BE_API void be_sound_free(be_sound *s);

/* What there is. */
BE_API int be_preset_count(void);
BE_API const char *be_preset_id(int i);
BE_API const char *be_preset_name(int i);
BE_API const char *be_preset_kind(int i);
BE_API int be_key_count(void);
BE_API const char *be_key_name(int i);
BE_API int be_key_is_text(int i);
BE_API int be_action_count(void);
BE_API const char *be_action_id(int i);
BE_API int be_action_key_count(void);
BE_API const char *be_action_key_name(int i);
BE_API const char *be_status_text(int status);
BE_API const char *be_version(void);
BE_API const char *be_reference(void);                          /* the engine.js this follows */

#ifdef __cplusplus
}
#endif
#endif
