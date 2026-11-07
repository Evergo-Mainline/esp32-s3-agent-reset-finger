/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Evergo-Mainline
 *
 * json_flat.h - tiny flat-JSON object parser for the /api request body.
 *
 * The POST /api body is a flat object, e.g.
 *   {"action":"fastboot","power":4000,"hold":1000,"off":true,
 *    "wait":true,"request_id":"boot-001"}
 *
 * No nesting, no arrays: enough for the API and small enough to keep the
 * firmware dependency-free (ESP-IDF 6.1 no longer ships cJSON).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define JFLAT_MAX_FIELDS 10
#define JFLAT_KEY_MAX    16
#define JFLAT_STR_MAX    48

typedef enum {
    JFLAT_STRING,
    JFLAT_NUMBER,
    JFLAT_TRUE,
    JFLAT_FALSE,
    JFLAT_NULL,
} jflat_type_t;

typedef struct {
    char         key[JFLAT_KEY_MAX];
    jflat_type_t type;
    char         str[JFLAT_STR_MAX];  /* JFLAT_STRING only */
    long         num;                 /* JFLAT_NUMBER only */
} jflat_field_t;

typedef struct {
    jflat_field_t fields[JFLAT_MAX_FIELDS];
    int           count;
} jflat_t;

/* Parse one flat JSON object. Trailing whitespace is allowed, anything else is
 * a syntax error. Returns false on malformed input. */
bool jflat_parse(const char *text, jflat_t *out);

/* NULL when the key is absent. */
const jflat_field_t *jflat_find(const jflat_t *obj, const char *key);

/* Typed lookups: return false when the key is absent OR has another type. */
bool jflat_str(const jflat_t *obj, const char *key, const char **value);
bool jflat_int(const jflat_t *obj, const char *key, long *value);
bool jflat_bool(const jflat_t *obj, const char *key, bool *value); /* bool or 0/1 */
