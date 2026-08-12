// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// json_emit.c — the hand-rolled JSON emitter and a small tolerant DOM parser
// (see internal/json.h). No nlohmann dependency; the library stays pure C. The
// emitter produces escaped JSON strings and numbers, including the A6
// integer-when-whole rule. The parser backs xsql_script_result_from_json.

#include "internal/json.h"

#include "internal/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===========================================================================
// Emitter
// ===========================================================================

void xsqlc_json_append_string(xsqlc_strbuf *sb, const char *s, size_t n) {
    xsqlc_sb_putc(sb, '"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  xsqlc_sb_append_len(sb, "\\\"", 2); break;
            case '\\': xsqlc_sb_append_len(sb, "\\\\", 2); break;
            case '\b': xsqlc_sb_append_len(sb, "\\b", 2);  break;
            case '\f': xsqlc_sb_append_len(sb, "\\f", 2);  break;
            case '\n': xsqlc_sb_append_len(sb, "\\n", 2);  break;
            case '\r': xsqlc_sb_append_len(sb, "\\r", 2);  break;
            case '\t': xsqlc_sb_append_len(sb, "\\t", 2);  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)c);
                    xsqlc_sb_append(sb, buf);
                } else {
                    xsqlc_sb_putc(sb, (char)c);
                }
        }
    }
    xsqlc_sb_putc(sb, '"');
}

void xsqlc_json_append_number(xsqlc_strbuf *sb, double v) {
    // Whole-number values serialize as bare integers (no ".0"); everything else
    // uses the default %g form, matching std::ostringstream << double. The
    // isfinite guard avoids UB in the (double)(long long) cast for the non-finite
    // inputs that never occur for real elapsed_ms values but would trap ASan/UBSan.
    if (isfinite(v) && v == (double)(long long)v) {
        xsqlc_sb_append_i64(sb, (long long)v);
    } else {
        xsqlc_sb_printf(sb, "%g", v);
    }
}

// ===========================================================================
// Tolerant DOM parser
// ===========================================================================

typedef struct {
    const char *p;
    const char *end;
} jcursor;

static xsqlc_jval *jparse_value(jcursor *c);

static xsqlc_jval *jval_new(xsqlc_jtype t) {
    xsqlc_jval *v = (xsqlc_jval *)xsqlc_malloc(sizeof(*v));
    if (!v) {
        return NULL;
    }
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

void xsqlc_json_free(xsqlc_jval *v) {
    if (!v) {
        return;
    }
    switch (v->type) {
        case XSQLC_JSTR:
            xsqlc_free(v->str);
            break;
        case XSQLC_JARR:
            for (size_t i = 0; i < v->nitems; ++i) {
                xsqlc_json_free(v->items[i]);
            }
            xsqlc_free(v->items);
            break;
        case XSQLC_JOBJ:
            for (size_t i = 0; i < v->nmembers; ++i) {
                xsqlc_free(v->keys[i]);
                xsqlc_json_free(v->vals[i]);
            }
            xsqlc_free(v->keys);
            xsqlc_free(v->vals);
            break;
        default:
            break;
    }
    xsqlc_free(v);
}

static void jskip_ws(jcursor *c) {
    while (c->p < c->end) {
        char ch = *c->p;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            ++c->p;
        } else {
            break;
        }
    }
}

// Encode a Unicode code point as UTF-8 into `out` (>= 4 bytes). Returns the byte
// count written.
static size_t utf8_encode(unsigned int cp, char out[4]) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse a JSON string literal (cursor is on the opening quote). On success stores
// the decoded bytes (xsqlc heap) + length; returns true. The buffer is always
// NUL-terminated for convenience but `len` is authoritative.
static bool jparse_string_raw(jcursor *c, char **out, size_t *out_len) {
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    ++c->p;  // opening quote
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"') {
            if (xsqlc_sb_oom(&sb)) {
                xsqlc_sb_free(&sb);
                return false;
            }
            *out_len = sb.len;
            char *detached = xsqlc_sb_detach(&sb);
            if (!detached) {
                return false;
            }
            *out = detached;
            return true;
        }
        if (ch == '\\') {
            if (c->p >= c->end) {
                break;
            }
            char esc = *c->p++;
            switch (esc) {
                case '"':  xsqlc_sb_putc(&sb, '"');  break;
                case '\\': xsqlc_sb_putc(&sb, '\\'); break;
                case '/':  xsqlc_sb_putc(&sb, '/');  break;
                case 'b':  xsqlc_sb_putc(&sb, '\b'); break;
                case 'f':  xsqlc_sb_putc(&sb, '\f'); break;
                case 'n':  xsqlc_sb_putc(&sb, '\n'); break;
                case 'r':  xsqlc_sb_putc(&sb, '\r'); break;
                case 't':  xsqlc_sb_putc(&sb, '\t'); break;
                case 'u': {
                    if (c->end - c->p < 4) {
                        xsqlc_sb_free(&sb);
                        return false;
                    }
                    int h0 = hex_nibble(c->p[0]), h1 = hex_nibble(c->p[1]);
                    int h2 = hex_nibble(c->p[2]), h3 = hex_nibble(c->p[3]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                        xsqlc_sb_free(&sb);
                        return false;
                    }
                    unsigned int cp =
                        (unsigned int)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    c->p += 4;
                    // High surrogate: consume a trailing \uXXXX low surrogate.
                    if (cp >= 0xD800 && cp <= 0xDBFF && c->end - c->p >= 6 &&
                        c->p[0] == '\\' && c->p[1] == 'u') {
                        int l0 = hex_nibble(c->p[2]), l1 = hex_nibble(c->p[3]);
                        int l2 = hex_nibble(c->p[4]), l3 = hex_nibble(c->p[5]);
                        if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                            unsigned int lo = (unsigned int)((l0 << 12) |
                                                            (l1 << 8) |
                                                            (l2 << 4) | l3);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) +
                                     (lo - 0xDC00);
                                c->p += 6;
                            }
                        }
                    }
                    char u8[4];
                    size_t n = utf8_encode(cp, u8);
                    xsqlc_sb_append_len(&sb, u8, n);
                    break;
                }
                default:
                    xsqlc_sb_free(&sb);
                    return false;
            }
        } else {
            xsqlc_sb_putc(&sb, ch);
        }
    }
    xsqlc_sb_free(&sb);
    return false;  // unterminated
}

static xsqlc_jval *jparse_string(jcursor *c) {
    char *s = NULL;
    size_t len = 0;
    if (!jparse_string_raw(c, &s, &len)) {
        return NULL;
    }
    xsqlc_jval *v = jval_new(XSQLC_JSTR);
    if (!v) {
        xsqlc_free(s);
        return NULL;
    }
    v->str = s;
    v->strlen = len;
    return v;
}

static xsqlc_jval *jparse_number(jcursor *c) {
    const char *start = c->p;
    if (c->p < c->end && (*c->p == '-' || *c->p == '+')) {
        ++c->p;
    }
    while (c->p < c->end) {
        char ch = *c->p;
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' ||
            ch == '+' || ch == '-') {
            ++c->p;
        } else {
            break;
        }
    }
    if (c->p == start) {
        return NULL;
    }
    // strtod over a NUL-terminated copy (the source is not guaranteed
    // NUL-terminated at `end`, though our callers always pass a C string). A
    // typical numeric literal fits the small stack buffer; an over-long literal
    // (e.g. one padded with many digits/zeros) must NOT be silently truncated —
    // that would misparse the value — so fall back to a heap copy of the full run.
    char stackbuf[64];
    size_t n = (size_t)(c->p - start);
    char *tmp = stackbuf;
    char *heap = NULL;
    if (n >= sizeof(stackbuf)) {
        heap = (char *)xsqlc_malloc(n + 1);
        if (!heap) {
            return NULL;
        }
        tmp = heap;
    }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    xsqlc_jval *v = jval_new(XSQLC_JNUM);
    if (!v) {
        xsqlc_free(heap);
        return NULL;
    }
    v->num = strtod(tmp, NULL);
    xsqlc_free(heap);
    return v;
}

static xsqlc_jval *jparse_literal(jcursor *c, const char *word, xsqlc_jtype t,
                                  bool bval) {
    size_t len = strlen(word);
    if ((size_t)(c->end - c->p) < len || memcmp(c->p, word, len) != 0) {
        return NULL;
    }
    c->p += len;
    xsqlc_jval *v = jval_new(t);
    if (v) {
        v->bval = bval;
    }
    return v;
}

static bool arr_push(xsqlc_jval *arr, xsqlc_jval *item) {
    xsqlc_jval **grown = (xsqlc_jval **)xsqlc_realloc(
        arr->items, (arr->nitems + 1) * sizeof(*grown));
    if (!grown) {
        return false;
    }
    arr->items = grown;
    arr->items[arr->nitems++] = item;
    return true;
}

static xsqlc_jval *jparse_array(jcursor *c) {
    ++c->p;  // '['
    xsqlc_jval *arr = jval_new(XSQLC_JARR);
    if (!arr) {
        return NULL;
    }
    jskip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        ++c->p;
        return arr;
    }
    for (;;) {
        jskip_ws(c);
        xsqlc_jval *item = jparse_value(c);
        if (!item || !arr_push(arr, item)) {
            xsqlc_json_free(item);
            xsqlc_json_free(arr);
            return NULL;
        }
        jskip_ws(c);
        if (c->p >= c->end) {
            xsqlc_json_free(arr);
            return NULL;
        }
        if (*c->p == ',') {
            ++c->p;
            continue;
        }
        if (*c->p == ']') {
            ++c->p;
            return arr;
        }
        xsqlc_json_free(arr);
        return NULL;
    }
}

static bool obj_push(xsqlc_jval *obj, char *key, xsqlc_jval *val) {
    char **gk = (char **)xsqlc_realloc(obj->keys,
                                       (obj->nmembers + 1) * sizeof(*gk));
    if (!gk) {
        return false;
    }
    obj->keys = gk;
    xsqlc_jval **gv = (xsqlc_jval **)xsqlc_realloc(
        obj->vals, (obj->nmembers + 1) * sizeof(*gv));
    if (!gv) {
        return false;
    }
    obj->vals = gv;
    obj->keys[obj->nmembers] = key;
    obj->vals[obj->nmembers] = val;
    ++obj->nmembers;
    return true;
}

static xsqlc_jval *jparse_object(jcursor *c) {
    ++c->p;  // '{'
    xsqlc_jval *obj = jval_new(XSQLC_JOBJ);
    if (!obj) {
        return NULL;
    }
    jskip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        ++c->p;
        return obj;
    }
    for (;;) {
        jskip_ws(c);
        char *key = NULL;
        size_t klen = 0;
        if (!jparse_string_raw(c, &key, &klen)) {
            xsqlc_json_free(obj);
            return NULL;
        }
        jskip_ws(c);
        if (c->p >= c->end || *c->p != ':') {
            xsqlc_free(key);
            xsqlc_json_free(obj);
            return NULL;
        }
        ++c->p;  // ':'
        jskip_ws(c);
        xsqlc_jval *val = jparse_value(c);
        if (!val || !obj_push(obj, key, val)) {
            xsqlc_free(key);
            xsqlc_json_free(val);
            xsqlc_json_free(obj);
            return NULL;
        }
        jskip_ws(c);
        if (c->p >= c->end) {
            xsqlc_json_free(obj);
            return NULL;
        }
        if (*c->p == ',') {
            ++c->p;
            continue;
        }
        if (*c->p == '}') {
            ++c->p;
            return obj;
        }
        xsqlc_json_free(obj);
        return NULL;
    }
}

static xsqlc_jval *jparse_value(jcursor *c) {
    jskip_ws(c);
    if (c->p >= c->end) {
        return NULL;
    }
    char ch = *c->p;
    switch (ch) {
        case '{': return jparse_object(c);
        case '[': return jparse_array(c);
        case '"': return jparse_string(c);
        case 't': return jparse_literal(c, "true", XSQLC_JBOOL, true);
        case 'f': return jparse_literal(c, "false", XSQLC_JBOOL, false);
        case 'n': return jparse_literal(c, "null", XSQLC_JNULL, false);
        default:  return jparse_number(c);
    }
}

xsqlc_jval *xsqlc_json_parse(const char *text) {
    if (!text) {
        return NULL;
    }
    jcursor c = {text, text + strlen(text)};
    jskip_ws(&c);
    xsqlc_jval *v = jparse_value(&c);
    if (!v) {
        return NULL;
    }
    jskip_ws(&c);
    if (c.p != c.end) {  // trailing garbage
        xsqlc_json_free(v);
        return NULL;
    }
    return v;
}

const xsqlc_jval *xsqlc_json_member(const xsqlc_jval *v, const char *key) {
    if (!v || v->type != XSQLC_JOBJ) {
        return NULL;
    }
    for (size_t i = 0; i < v->nmembers; ++i) {
        if (strcmp(v->keys[i], key) == 0) {
            return v->vals[i];
        }
    }
    return NULL;
}

bool xsqlc_json_is(const xsqlc_jval *v, xsqlc_jtype t) {
    return v && v->type == t;
}

bool xsqlc_json_bool(const xsqlc_jval *v, bool dflt) {
    return (v && v->type == XSQLC_JBOOL) ? v->bval : dflt;
}

double xsqlc_json_num(const xsqlc_jval *v, double dflt) {
    return (v && v->type == XSQLC_JNUM) ? v->num : dflt;
}

size_t xsqlc_json_size(const xsqlc_jval *v, size_t dflt) {
    if (!v || v->type != XSQLC_JNUM || v->num < 0) {
        return dflt;
    }
    return (size_t)v->num;
}

const char *xsqlc_json_str(const xsqlc_jval *v) {
    return (v && v->type == XSQLC_JSTR && v->str) ? v->str : "";
}

// ===========================================================================
// DOM serializer (inverse of the parser)
// ===========================================================================

void xsqlc_json_dump(const xsqlc_jval *v, xsqlc_strbuf *sb) {
    if (!v) {
        xsqlc_sb_append_len(sb, "null", 4);
        return;
    }
    switch (v->type) {
        case XSQLC_JNULL:
            xsqlc_sb_append_len(sb, "null", 4);
            break;
        case XSQLC_JBOOL:
            if (v->bval) {
                xsqlc_sb_append_len(sb, "true", 4);
            } else {
                xsqlc_sb_append_len(sb, "false", 5);
            }
            break;
        case XSQLC_JNUM:
            xsqlc_json_append_number(sb, v->num);
            break;
        case XSQLC_JSTR:
            xsqlc_json_append_string(sb, v->str ? v->str : "", v->strlen);
            break;
        case XSQLC_JARR:
            xsqlc_sb_putc(sb, '[');
            for (size_t i = 0; i < v->nitems; ++i) {
                if (i) {
                    xsqlc_sb_putc(sb, ',');
                }
                xsqlc_json_dump(v->items[i], sb);
            }
            xsqlc_sb_putc(sb, ']');
            break;
        case XSQLC_JOBJ:
            xsqlc_sb_putc(sb, '{');
            for (size_t i = 0; i < v->nmembers; ++i) {
                if (i) {
                    xsqlc_sb_putc(sb, ',');
                }
                xsqlc_json_append_string(sb, v->keys[i], strlen(v->keys[i]));
                xsqlc_sb_putc(sb, ':');
                xsqlc_json_dump(v->vals[i], sb);
            }
            xsqlc_sb_putc(sb, '}');
            break;
    }
}
