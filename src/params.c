/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Daniel Sandner */
/* Parameters by name, presets, and what there is. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static int key_index(const char *key)
{
    int i;
    if (!key)
        return -1;
    for (i = 0; i < K_COUNT; i++)
        if (strcmp(KEYS[i].name, key) == 0)
            return i;
    return -1;
}

static void put_text(be_params *p, int k, const char *v)
{
    strncpy(p->s[k], v, BE_TEXT_MAX - 1);
    p->s[k][BE_TEXT_MAX - 1] = 0;
}

void be_params_reset(be_params *p)
{
    int i;
    for (i = 0; i < K_COUNT; i++) {
        p->v[i] = KEYS[i].type == BE_TEXT ? 0.0 : KEYS[i].num;
        p->s[i][0] = 0;
        if (KEYS[i].type == BE_TEXT)
            put_text(p, i, KEYS[i].text);
    }
}

be_params *be_params_new(void)
{
    be_params *p = (be_params *)malloc(sizeof *p);
    if (p)
        be_params_reset(p);
    return p;
}

be_params *be_params_copy(const be_params *p)
{
    be_params *q = (be_params *)malloc(sizeof *q);
    if (q && p)
        memcpy(q, p, sizeof *q);
    return q;
}

void be_params_free(be_params *p) { free(p); }

int be_params_preset(be_params *p, const char *id)
{
    int i, j;
    for (i = 0; i < PRESET_COUNT; i++) {
        if (strcmp(PRESETS[i].id, id ? id : "") != 0)
            continue;
        be_params_reset(p);
        for (j = 0; j < PRESETS[i].n; j++) {
            const be_kv *kv = &PRESETS[i].kv[j];
            if (kv->text)
                put_text(p, kv->key, kv->text);
            else
                p->v[kv->key] = kv->num;
        }
        return BE_OK;
    }
    return BE_ERR_PRESET;
}

int be_params_set(be_params *p, const char *key, double value)
{
    int k = key_index(key);
    if (k < 0)
        return BE_ERR_KEY;
    if (KEYS[k].type == BE_TEXT || value != value)
        return BE_ERR_VALUE;
    p->v[k] = KEYS[k].type == BE_FLAG ? (value != 0) : value;
    return BE_OK;
}

int be_params_set_text(be_params *p, const char *key, const char *value)
{
    int k = key_index(key);
    if (k < 0)
        return BE_ERR_KEY;
    if (!value)
        return BE_ERR_VALUE;
    if (KEYS[k].type != BE_TEXT) {
        char *end;
        double v;
        if (KEYS[k].type == BE_FLAG && (!strcmp(value, "true") || !strcmp(value, "false")))
            return be_params_set(p, key, value[0] == 't');
        v = strtod(value, &end);
        if (end == value || *end)
            return BE_ERR_VALUE;
        return be_params_set(p, key, v);
    }
    if (strlen(value) >= BE_TEXT_MAX)
        return BE_ERR_VALUE;
    put_text(p, k, value);
    return BE_OK;
}

int be_params_get(const be_params *p, const char *key, double *value)
{
    int k = key_index(key);
    if (k < 0)
        return BE_ERR_KEY;
    if (KEYS[k].type == BE_TEXT)
        return BE_ERR_VALUE;
    *value = p->v[k];
    return BE_OK;
}

const char *be_params_get_text(const be_params *p, const char *key)
{
    int k = key_index(key);
    return k >= 0 && KEYS[k].type == BE_TEXT ? p->s[k] : 0;
}

const be_material *be_material_find(const char *id)
{
    int i;
    for (i = 0; i < MATERIAL_COUNT; i++)
        if (strcmp(MATERIALS[i].id, id) == 0)
            return &MATERIALS[i];
    return 0;
}

const be_env *be_env_find(const char *id)
{
    int i;
    for (i = 0; i < ENV_COUNT; i++)
        if (strcmp(ENVS[i].id, id) == 0)
            return &ENVS[i];
    return 0;
}

int be_preset_count(void) { return PRESET_COUNT; }
const char *be_preset_id(int i) { return i >= 0 && i < PRESET_COUNT ? PRESETS[i].id : 0; }
const char *be_preset_name(int i) { return i >= 0 && i < PRESET_COUNT ? PRESETS[i].name : 0; }

const char *be_preset_kind(int i)
{
    int j;
    if (i < 0 || i >= PRESET_COUNT)
        return 0;
    for (j = 0; j < PRESETS[i].n; j++)
        if (PRESETS[i].kv[j].key == K_kind)
            return PRESETS[i].kv[j].text;
    return KEYS[K_kind].text;
}

int be_key_count(void) { return K_COUNT; }
const char *be_key_name(int i) { return i >= 0 && i < K_COUNT ? KEYS[i].name : 0; }
int be_key_is_text(int i) { return i >= 0 && i < K_COUNT && KEYS[i].type == BE_TEXT; }
int be_action_count(void) { return ACTION_COUNT; }
const char *be_action_id(int i) { return i >= 0 && i < ACTION_COUNT ? ACTIONS[i].id : 0; }
int be_action_key_count(void) { return A_COUNT; }
const char *be_action_key_name(int i) { return i >= 0 && i < A_COUNT ? ACTION_KEYS[i] : 0; }
const char *be_version(void) { return BE_VERSION; }
const char *be_reference(void) { return BE_REFERENCE; }

const char *be_status_text(int status)
{
    switch (status) {
    case BE_OK: return "ok";
    case BE_ERR_KEY: return "no parameter of that name";
    case BE_ERR_VALUE: return "a value that parameter cannot take";
    case BE_ERR_PRESET: return "no preset of that name";
    case BE_ERR_KIND: return "this version cannot render that kind of sound yet";
    case BE_ERR_UNSUPPORTED: return "that needs a recording, which this engine does not take";
    case BE_ERR_MEMORY: return "out of memory";
    case BE_ERR_IO: return "cannot write the file";
    }
    return "unknown status";
}
