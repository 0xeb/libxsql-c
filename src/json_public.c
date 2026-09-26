// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// json_public.c — the public <libxsqlc/json.h> surface over the internal
// tolerant DOM in internal/json.h (parsed by json_emit.c).
//
// This is deliberately a thin adapter rather than a second parser. The internal
// DOM already backs the script envelope's round-trip, so a separate
// implementation would be a second thing to keep correct. What this layer adds
// is a stable opaque type, NULL-tolerant getters, and ordered object access —
// none of which the internal header offered, because its only consumer knew the
// exact schema it was reading.

#include <libxsqlc/json.h>

#include "internal/internal.h"
#include "internal/json.h"

#include <string.h>

// The public handle IS the internal node; the opacity is for callers, not for
// us. Kept as two casts in one place so the rest of the file reads plainly.
static const xsqlc_jval *in(const xsql_json *v) {
    return (const xsqlc_jval *)v;
}
static const xsql_json *out(const xsqlc_jval *v) {
    return (const xsql_json *)v;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

xsql_json *xsql_json_parse(const char *text) {
    if (!text) {
        return NULL;
    }
    return (xsql_json *)xsqlc_json_parse(text);
}

void xsql_json_free(xsql_json *root) {
    xsqlc_json_free((xsqlc_jval *)root);
}

// ---------------------------------------------------------------------------
// Type inspection
// ---------------------------------------------------------------------------

xsql_json_type xsql_json_type_of(const xsql_json *v) {
    if (!v) {
        return XSQL_JSON_NULL;
    }
    switch (in(v)->type) {
        case XSQLC_JBOOL: return XSQL_JSON_BOOL;
        case XSQLC_JNUM:  return XSQL_JSON_NUMBER;
        case XSQLC_JSTR:  return XSQL_JSON_STRING;
        case XSQLC_JARR:  return XSQL_JSON_ARRAY;
        case XSQLC_JOBJ:  return XSQL_JSON_OBJECT;
        case XSQLC_JNULL:
        default:          return XSQL_JSON_NULL;
    }
}

bool xsql_json_is_null(const xsql_json *v) {
    return xsql_json_type_of(v) == XSQL_JSON_NULL;
}

bool xsql_json_is(const xsql_json *v, xsql_json_type t) {
    return xsql_json_type_of(v) == t;
}

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------

bool xsql_json_bool(const xsql_json *v, bool dflt) {
    if (!v || in(v)->type != XSQLC_JBOOL) {
        return dflt;
    }
    return in(v)->bval;
}

double xsql_json_double(const xsql_json *v, double dflt) {
    if (!v || in(v)->type != XSQLC_JNUM) {
        return dflt;
    }
    return in(v)->num;
}

bool xsql_json_is_int(const xsql_json *v) {
    return v && in(v)->type == XSQLC_JNUM && in(v)->is_int;
}

int64_t xsql_json_i64(const xsql_json *v, int64_t dflt) {
    if (!v || in(v)->type != XSQLC_JNUM) {
        return dflt;
    }
    if (in(v)->is_int) {
        return in(v)->i64;
    }
    // Not an exact integer literal. Truncate toward zero rather than refuse:
    // a caller that cares about the distinction has xsql_json_is_int().
    return (int64_t)in(v)->num;
}

const char *xsql_json_string(const xsql_json *v, size_t *len) {
    if (!v || in(v)->type != XSQLC_JSTR) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    if (len) {
        *len = in(v)->strlen;
    }
    return in(v)->str;
}

// ---------------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------------

size_t xsql_json_array_size(const xsql_json *v) {
    if (!v || in(v)->type != XSQLC_JARR) {
        return 0;
    }
    return in(v)->nitems;
}

const xsql_json *xsql_json_array_at(const xsql_json *v, size_t i) {
    if (!v || in(v)->type != XSQLC_JARR || i >= in(v)->nitems) {
        return NULL;
    }
    return out(in(v)->items[i]);
}

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

size_t xsql_json_object_size(const xsql_json *v) {
    if (!v || in(v)->type != XSQLC_JOBJ) {
        return 0;
    }
    return in(v)->nmembers;
}

const char *xsql_json_object_key(const xsql_json *v, size_t i, size_t *len) {
    if (!v || in(v)->type != XSQLC_JOBJ || i >= in(v)->nmembers) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    const char *k = in(v)->keys[i];
    if (len) {
        *len = k ? strlen(k) : 0;
    }
    return k;
}

const xsql_json *xsql_json_object_at(const xsql_json *v, size_t i) {
    if (!v || in(v)->type != XSQLC_JOBJ || i >= in(v)->nmembers) {
        return NULL;
    }
    return out(in(v)->vals[i]);
}

const xsql_json *xsql_json_member(const xsql_json *v, const char *key) {
    if (!v || !key || in(v)->type != XSQLC_JOBJ) {
        return NULL;
    }
    // Linear scan in document order, so a duplicate key resolves to the first
    // occurrence as documented. Objects in this domain are small; an index
    // would cost more than it saves.
    for (size_t i = 0; i < in(v)->nmembers; ++i) {
        const char *k = in(v)->keys[i];
        if (k && strcmp(k, key) == 0) {
            return out(in(v)->vals[i]);
        }
    }
    return NULL;
}
