// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// merge_patch.c — RFC 7386 JSON merge-patch for GET /status (A5).
// Reproduces nlohmann::json::merge_patch: recursively merge the patch object onto
// the base object, a null member deletes that key, and a non-object patch value
// replaces the target outright. Built on the internal DOM (parse + xsqlc_json_dump).

#include "thinclient/http_internal.h"

#include "internal/internal.h"
#include "internal/json.h"
#include "internal/strbuf.h"
#include "libxsqlc/status.h"

#include <string.h>

// Emit into `sb` the RFC 7386 merge of `patch` onto `base` (base may be NULL,
// meaning "merge into an absent value" — an object patch then just strips its own
// null members). A non-object patch is dumped verbatim (it replaces the target).
static void emit_merge(xsqlc_strbuf *sb, const xsqlc_jval *base,
                       const xsqlc_jval *patch) {
    if (!patch || patch->type != XSQLC_JOBJ) {
        xsqlc_json_dump(patch, sb);
        return;
    }

    const bool base_is_obj = base && base->type == XSQLC_JOBJ;
    bool first = true;
    xsqlc_sb_putc(sb, '{');

    // Base members, in base order: merged when the patch touches them, deleted
    // when the patch maps them to null, carried through otherwise.
    if (base_is_obj) {
        for (size_t i = 0; i < base->nmembers; ++i) {
            const char *bk = base->keys[i];
            const xsqlc_jval *pv = xsqlc_json_member(patch, bk);
            if (pv && pv->type == XSQLC_JNULL) {
                continue;  // deleted
            }
            if (!first) {
                xsqlc_sb_putc(sb, ',');
            }
            first = false;
            xsqlc_json_append_string(sb, bk, strlen(bk));
            xsqlc_sb_putc(sb, ':');
            if (pv) {
                emit_merge(sb, base->vals[i], pv);
            } else {
                xsqlc_json_dump(base->vals[i], sb);
            }
        }
    }

    // Patch-only members (keys the base did not have), skipping null deletes.
    for (size_t i = 0; i < patch->nmembers; ++i) {
        const char *pk = patch->keys[i];
        const xsqlc_jval *pv = patch->vals[i];
        if (base_is_obj && xsqlc_json_member(base, pk)) {
            continue;  // already emitted in the base loop
        }
        if (pv->type == XSQLC_JNULL) {
            continue;  // deleting an absent key is a no-op
        }
        if (!first) {
            xsqlc_sb_putc(sb, ',');
        }
        first = false;
        xsqlc_json_append_string(sb, pk, strlen(pk));
        xsqlc_sb_putc(sb, ':');
        emit_merge(sb, NULL, pv);
    }

    xsqlc_sb_putc(sb, '}');
}

int xsqlc_status_merge_patch(const char *base, const char *patch, char **out) {
    *out = NULL;

    xsqlc_jval *jp = xsqlc_json_parse(patch);
    if (!jp) {
        return XSQL_ERROR;  // status_fn produced non-JSON; caller keeps base
    }
    xsqlc_jval *jb = xsqlc_json_parse(base);  // NULL tolerated (treated as absent)

    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    emit_merge(&sb, jb, jp);

    xsqlc_json_free(jb);
    xsqlc_json_free(jp);

    if (xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        return XSQL_NO_MEMORY;
    }
    *out = xsqlc_sb_detach(&sb);
    return *out ? XSQL_OK : XSQL_NO_MEMORY;
}
