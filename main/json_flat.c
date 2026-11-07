/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Evergo-Mainline
 */

#include "json_flat.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
} parser_t;

static void skip_ws(parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') {
        ps->p++;
    }
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Consume a JSON string literal. Rejects control characters and the \u escape
 * (the API only accepts short printable values here, and rejecting is safer
 * than guessing). Strings longer than dst are a parse error. */
static bool parse_string(parser_t *ps, char *dst, size_t cap)
{
    if (*ps->p != '"') {
        return false;
    }
    ps->p++;
    size_t n = 0;
    while (*ps->p != '\0' && *ps->p != '"') {
        char c = *ps->p++;
        if ((unsigned char)c < 0x20) {
            return false;
        }
        if (c == '\\') {
            char e = *ps->p++;
            switch (e) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            default:   return false;   /* \u and unknown escapes unsupported */
            }
        }
        if (n + 1 >= cap) {
            return false;
        }
        dst[n++] = c;
    }
    if (*ps->p != '"') {
        return false;
    }
    ps->p++;
    dst[n] = '\0';
    return true;
}

/* Integer numbers only (a fractional part is accepted but truncated to long). */
static bool parse_number(parser_t *ps, long *out)
{
    const char *start = ps->p;
    if (*ps->p == '-') {
        ps->p++;
    }
    if (!is_digit(*ps->p)) {
        return false;
    }
    while (is_digit(*ps->p)) {
        ps->p++;
    }
    if (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E') {
        if (*ps->p == '.') {
            ps->p++;
            if (!is_digit(*ps->p)) {
                return false;
            }
            while (is_digit(*ps->p)) {
                ps->p++;
            }
        }
        if (*ps->p == 'e' || *ps->p == 'E') {
            ps->p++;
            if (*ps->p == '+' || *ps->p == '-') {
                ps->p++;
            }
            if (!is_digit(*ps->p)) {
                return false;
            }
            while (is_digit(*ps->p)) {
                ps->p++;
            }
        }
        double d = strtod(start, NULL);
        if (d > 2147483647.0 || d < -2147483648.0) {
            return false;
        }
        *out = (long)d;
    } else {
        char tmp[24];
        size_t len = (size_t)(ps->p - start);
        if (len >= sizeof(tmp)) {
            return false;
        }
        memcpy(tmp, start, len);
        tmp[len] = '\0';
        *out = strtol(tmp, NULL, 10);
    }
    return true;
}

static bool parse_value(parser_t *ps, jflat_field_t *f)
{
    skip_ws(ps);
    switch (*ps->p) {
    case '"':
        f->type = JFLAT_STRING;
        return parse_string(ps, f->str, sizeof(f->str));
    case 't':
        if (strncmp(ps->p, "true", 4) != 0) {
            return false;
        }
        ps->p += 4;
        f->type = JFLAT_TRUE;
        return true;
    case 'f':
        if (strncmp(ps->p, "false", 5) != 0) {
            return false;
        }
        ps->p += 5;
        f->type = JFLAT_FALSE;
        return true;
    case 'n':
        if (strncmp(ps->p, "null", 4) != 0) {
            return false;
        }
        ps->p += 4;
        f->type = JFLAT_NULL;
        return true;
    default:
        f->type = JFLAT_NUMBER;
        return parse_number(ps, &f->num);
    }
}

bool jflat_parse(const char *text, jflat_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    out->count = 0;

    parser_t ps = { text };
    skip_ws(&ps);
    if (*ps.p != '{') {
        return false;
    }
    ps.p++;
    skip_ws(&ps);
    if (*ps.p == '}') {
        ps.p++;
    } else {
        for (;;) {
            char key[JFLAT_KEY_MAX];
            skip_ws(&ps);
            if (!parse_string(&ps, key, sizeof(key))) {
                return false;
            }
            skip_ws(&ps);
            if (*ps.p != ':') {
                return false;
            }
            ps.p++;

            jflat_field_t field;
            memset(&field, 0, sizeof(field));
            memcpy(field.key, key, sizeof(field.key));
            if (!parse_value(&ps, &field)) {
                return false;
            }
            /* first occurrence wins; extra fields are ignored (forward compat) */
            if (jflat_find(out, field.key) == NULL && out->count < JFLAT_MAX_FIELDS) {
                out->fields[out->count++] = field;
            }

            skip_ws(&ps);
            if (*ps.p == ',') {
                ps.p++;
                continue;
            }
            if (*ps.p == '}') {
                ps.p++;
                break;
            }
            return false;
        }
    }

    skip_ws(&ps);
    return *ps.p == '\0';
}

const jflat_field_t *jflat_find(const jflat_t *obj, const char *key)
{
    for (int i = 0; i < obj->count; i++) {
        if (strcmp(obj->fields[i].key, key) == 0) {
            return &obj->fields[i];
        }
    }
    return NULL;
}

bool jflat_str(const jflat_t *obj, const char *key, const char **value)
{
    const jflat_field_t *f = jflat_find(obj, key);
    if (f == NULL || f->type != JFLAT_STRING) {
        return false;
    }
    *value = f->str;
    return true;
}

bool jflat_int(const jflat_t *obj, const char *key, long *value)
{
    const jflat_field_t *f = jflat_find(obj, key);
    if (f == NULL || f->type != JFLAT_NUMBER) {
        return false;
    }
    *value = f->num;
    return true;
}

bool jflat_bool(const jflat_t *obj, const char *key, bool *value)
{
    const jflat_field_t *f = jflat_find(obj, key);
    if (f == NULL) {
        return false;
    }
    switch (f->type) {
    case JFLAT_TRUE:   *value = true;  return true;
    case JFLAT_FALSE:  *value = false; return true;
    case JFLAT_NUMBER:
        if (f->num == 0 || f->num == 1) {
            *value = f->num != 0;
            return true;
        }
        return false;
    default:
        return false;
    }
}
