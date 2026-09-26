// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// regex.c — recursive-descent parser + backtracking matcher for the ECMAScript
// subset documented in <libxsqlc/regex.h>.
//
// SHAPE. The pattern parses into a tree of `node`s chained by `next` (a
// concatenation). A group holds an array of alternative chains; a repeat holds
// one child chain. Matching walks that tree with an explicit CONTINUATION
// (`cont`) rather than compiling to a program, because the continuation makes
// the two hard cases — "finish this alternative, then carry on after the
// group" and "take another repetition, then carry on" — fall out directly.
//
// Every node comes from an arena owned by the handle, so the whole tree frees
// in one pass and no node has an individual destructor.
//
// EMPTY-MATCH GUARD. `(a*)*` and friends can iterate forever on a child that
// consumes nothing. Each repetition records where it started; if the next one
// begins at the same position, the repeat stops instead of looping. This is
// the standard guard and it is why `count`/`last` ride on the continuation.

#include <libxsqlc/regex.h>

#include "internal/internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Node tree
// ---------------------------------------------------------------------------

typedef enum {
    N_CHAR,    // one literal byte
    N_ANY,     // '.'
    N_CLASS,   // [...] or an escape class; negation is baked into the bitmap
    N_BOL,     // ^
    N_EOL,     // $
    N_WORDB,   // \b
    N_NWORDB,  // \B
    N_GROUP,   // ( ... ) / (?: ... ) -- holds alternatives
    N_REPEAT   // quantified child
} nkind;

typedef struct node {
    nkind        kind;
    struct node *next;

    unsigned char ch;        // N_CHAR
    unsigned char set[32];   // N_CLASS: 256-bit membership bitmap

    struct node **alts;      // N_GROUP: array of chain heads
    size_t        nalts;

    struct node *child;      // N_REPEAT
    int          min, max;   // N_REPEAT (max == INT_MAX => unbounded)
    bool         greedy;     // N_REPEAT
} node;

// Arena block. Nodes and the small alternative arrays both come from here.
typedef struct arena_block {
    struct arena_block *next;
    size_t              used, cap;
    unsigned char      *mem;
} arena_block;

struct xsql_regex {
    arena_block *blocks;
    node        *root;  // top-level chain (a single N_GROUP holding the alts)
};

#define ARENA_BLOCK_MIN 1024

static void *arena_alloc(xsql_regex *re, size_t n) {
    n = (n + 15u) & ~(size_t)15u;  // keep everything 16-byte aligned
    arena_block *b = re->blocks;
    if (!b || b->cap - b->used < n) {
        size_t cap = n > ARENA_BLOCK_MIN ? n : ARENA_BLOCK_MIN;
        arena_block *nb = (arena_block *)xsqlc_malloc(sizeof(arena_block));
        if (!nb) {
            return NULL;
        }
        nb->mem = (unsigned char *)xsqlc_malloc(cap);
        if (!nb->mem) {
            xsqlc_free(nb);
            return NULL;
        }
        nb->used = 0;
        nb->cap = cap;
        nb->next = re->blocks;
        re->blocks = nb;
        b = nb;
    }
    void *p = b->mem + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

static node *node_new(xsql_regex *re, nkind k) {
    node *n = (node *)arena_alloc(re, sizeof(node));
    if (n) {
        n->kind = k;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Character-class helpers
// ---------------------------------------------------------------------------

static void set_add(unsigned char *set, unsigned char c) {
    set[c >> 3] |= (unsigned char)(1u << (c & 7u));
}
static bool set_has(const unsigned char *set, unsigned char c) {
    return (set[c >> 3] & (1u << (c & 7u))) != 0;
}
static void set_add_range(unsigned char *set, unsigned lo, unsigned hi) {
    for (unsigned c = lo; c <= hi && c < 256u; ++c) {
        set_add(set, (unsigned char)c);
    }
}
static void set_invert(unsigned char *set) {
    for (int i = 0; i < 32; ++i) {
        set[i] = (unsigned char)~set[i];
    }
}
static bool is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
// The five escape classes, shared by bare escapes and class interiors.
static bool add_escape_class(unsigned char *set, char e) {
    switch (e) {
        case 'd': set_add_range(set, '0', '9'); return true;
        case 'w':
            set_add_range(set, 'a', 'z');
            set_add_range(set, 'A', 'Z');
            set_add_range(set, '0', '9');
            set_add(set, '_');
            return true;
        case 's':
            set_add(set, ' ');
            set_add(set, '\t');
            set_add(set, '\n');
            set_add(set, '\r');
            set_add(set, '\f');
            set_add(set, '\v');
            return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

typedef struct {
    xsql_regex *re;
    const char *p;
    const char *end;
    char       *err;
    size_t      errlen;
    bool        failed;
} parser;

static void fail(parser *ps, const char *msg) {
    if (!ps->failed) {
        ps->failed = true;
        if (ps->err && ps->errlen > 0) {
            snprintf(ps->err, ps->errlen, "%s", msg);
        }
    }
}

static bool at_end(const parser *ps) { return ps->p >= ps->end; }
static char peek(const parser *ps) { return at_end(ps) ? '\0' : *ps->p; }

static node *parse_alternation(parser *ps);

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode the escape at ps->p (which is just past the backslash) into a literal
// byte sequence. Returns the number of bytes written to `out` (up to 4), or -1
// if this escape is not a literal (a class escape or an assertion).
static int decode_literal_escape(parser *ps, unsigned char *out) {
    char e = peek(ps);
    switch (e) {
        case 'n': ps->p++; out[0] = '\n'; return 1;
        case 'r': ps->p++; out[0] = '\r'; return 1;
        case 't': ps->p++; out[0] = '\t'; return 1;
        case 'f': ps->p++; out[0] = '\f'; return 1;
        case 'v': ps->p++; out[0] = '\v'; return 1;
        case '0': ps->p++; out[0] = '\0'; return 1;
        case 'x': {
            if (ps->end - ps->p < 3) {
                fail(ps, "truncated \\xHH escape");
                return -1;
            }
            int hi = hexval(ps->p[1]), lo = hexval(ps->p[2]);
            if (hi < 0 || lo < 0) {
                fail(ps, "bad \\xHH escape");
                return -1;
            }
            ps->p += 3;
            out[0] = (unsigned char)((hi << 4) | lo);
            return 1;
        }
        case 'u': {
            if (ps->end - ps->p < 5) {
                fail(ps, "truncated \\uHHHH escape");
                return -1;
            }
            int v = 0;
            for (int i = 1; i <= 4; ++i) {
                int h = hexval(ps->p[i]);
                if (h < 0) {
                    fail(ps, "bad \\uHHHH escape");
                    return -1;
                }
                v = (v << 4) | h;
            }
            ps->p += 5;
            // Encode the BMP code point as UTF-8 so it compares against UTF-8
            // subject bytes. Surrogates are passed through as-is; this engine
            // does not attempt to pair them.
            if (v < 0x80) {
                out[0] = (unsigned char)v;
                return 1;
            }
            if (v < 0x800) {
                out[0] = (unsigned char)(0xC0 | (v >> 6));
                out[1] = (unsigned char)(0x80 | (v & 0x3F));
                return 2;
            }
            out[0] = (unsigned char)(0xE0 | (v >> 12));
            out[1] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
            out[2] = (unsigned char)(0x80 | (v & 0x3F));
            return 3;
        }
        default:
            break;
    }
    if (e >= '1' && e <= '9') {
        fail(ps, "backreferences are not supported");
        return -1;
    }
    if (e == 'p' || e == 'P') {
        fail(ps, "unicode property escapes are not supported");
        return -1;
    }
    if (e == '\0') {
        fail(ps, "trailing backslash");
        return -1;
    }
    // Any other escaped byte is that literal byte (\. \\ \+ \( ...).
    ps->p++;
    out[0] = (unsigned char)e;
    return 1;
}

// [ ... ] — ps->p is just past the '['.
static node *parse_class(parser *ps) {
    node *n = node_new(ps->re, N_CLASS);
    if (!n) {
        fail(ps, "out of memory");
        return NULL;
    }
    bool negate = false;
    if (peek(ps) == '^') {
        negate = true;
        ps->p++;
    }
    // No POSIX "']' first is a literal" allowance: in ECMA-262 `[]` is an EMPTY
    // class (matches nothing) and `[^]` is its complement (matches anything).
    // MSVC's std::regex takes the POSIX reading here and libstdc++ does not;
    // the spec, and libstdc++, win.
    while (!at_end(ps) && peek(ps) != ']') {
        int lo = -1;
        if (peek(ps) == '\\') {
            ps->p++;
            char e = peek(ps);
            if (e == 'd' || e == 'w' || e == 's') {
                ps->p++;
                add_escape_class(n->set, e);
                continue;
            }
            if (e == 'D' || e == 'W' || e == 'S') {
                // A negated class inside a set: expand into a temp, invert,
                // then union. [^\d] and [\D] must agree.
                unsigned char tmp[32];
                memset(tmp, 0, sizeof(tmp));
                ps->p++;
                add_escape_class(tmp, (char)(e - 'A' + 'a'));
                set_invert(tmp);
                for (int i = 0; i < 32; ++i) {
                    n->set[i] |= tmp[i];
                }
                continue;
            }
            if (e == 'b') {  // \b inside a class is a backspace in ECMAScript
                ps->p++;
                set_add(n->set, 0x08);
                continue;
            }
            unsigned char buf[4];
            int nb = decode_literal_escape(ps, buf);
            if (nb < 0) {
                return NULL;
            }
            for (int i = 0; i < nb; ++i) {
                set_add(n->set, buf[i]);
            }
            if (nb == 1) {
                lo = buf[0];
            }
        } else {
            lo = (unsigned char)*ps->p++;
            set_add(n->set, (unsigned char)lo);
        }
        // Range: only when a single byte preceded '-' and the '-' is not last.
        if (lo >= 0 && peek(ps) == '-' && ps->p + 1 < ps->end && ps->p[1] != ']') {
            ps->p++;  // consume '-'
            int hi;
            if (peek(ps) == '\\') {
                ps->p++;
                unsigned char buf[4];
                int nb = decode_literal_escape(ps, buf);
                if (nb < 0) {
                    return NULL;
                }
                if (nb != 1) {
                    fail(ps, "multi-byte escape as a class range bound");
                    return NULL;
                }
                hi = buf[0];
            } else {
                hi = (unsigned char)*ps->p++;
            }
            if (hi < lo) {
                fail(ps, "class range is out of order");
                return NULL;
            }
            set_add_range(n->set, (unsigned)lo, (unsigned)hi);
        }
    }
    if (at_end(ps)) {
        fail(ps, "unterminated character class");
        return NULL;
    }
    ps->p++;  // ']'
    if (negate) {
        set_invert(n->set);
    }
    return n;
}

// One atom, with no quantifier applied yet.
static node *parse_atom(parser *ps) {
    char c = peek(ps);
    if (c == '(') {
        ps->p++;
        if (ps->end - ps->p >= 2 && ps->p[0] == '?') {
            if (ps->p[1] == ':') {
                ps->p += 2;  // non-capturing: same shape, we track no captures
            } else if (ps->p[1] == '=' || ps->p[1] == '!' || ps->p[1] == '<') {
                fail(ps, "lookaround and named groups are not supported");
                return NULL;
            } else {
                fail(ps, "inline flags / extended groups are not supported");
                return NULL;
            }
        }
        node *g = parse_alternation(ps);
        if (!g) {
            return NULL;
        }
        if (peek(ps) != ')') {
            fail(ps, "missing )");
            return NULL;
        }
        ps->p++;
        return g;
    }
    if (c == '[') {
        ps->p++;
        return parse_class(ps);
    }
    if (c == '.') {
        ps->p++;
        return node_new(ps->re, N_ANY);
    }
    if (c == '^') {
        ps->p++;
        return node_new(ps->re, N_BOL);
    }
    if (c == '$') {
        ps->p++;
        return node_new(ps->re, N_EOL);
    }
    if (c == ')') {
        fail(ps, "unmatched )");
        return NULL;
    }
    if (c == '*' || c == '+' || c == '?') {
        fail(ps, "quantifier with nothing to repeat");
        return NULL;
    }
    if (c == '\\') {
        ps->p++;
        char e = peek(ps);
        if (e == 'b') { ps->p++; return node_new(ps->re, N_WORDB); }
        if (e == 'B') { ps->p++; return node_new(ps->re, N_NWORDB); }
        if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
            node *n = node_new(ps->re, N_CLASS);
            if (!n) {
                fail(ps, "out of memory");
                return NULL;
            }
            bool neg = (e >= 'A' && e <= 'Z');
            add_escape_class(n->set, neg ? (char)(e - 'A' + 'a') : e);
            if (neg) {
                set_invert(n->set);
            }
            ps->p++;
            return n;
        }
        unsigned char buf[4];
        int nb = decode_literal_escape(ps, buf);
        if (nb < 0) {
            return NULL;
        }
        // A multi-byte \u escape becomes a chain of literal bytes. Quantifying
        // it would then apply to the LAST byte only, which is wrong, so wrap it
        // in a group and let the caller quantify the group.
        node *head = NULL, *tail = NULL;
        for (int i = 0; i < nb; ++i) {
            node *b = node_new(ps->re, N_CHAR);
            if (!b) {
                fail(ps, "out of memory");
                return NULL;
            }
            b->ch = buf[i];
            if (tail) {
                tail->next = b;
            } else {
                head = b;
            }
            tail = b;
        }
        if (nb == 1) {
            return head;
        }
        node *g = node_new(ps->re, N_GROUP);
        node **alts = (node **)arena_alloc(ps->re, sizeof(node *));
        if (!g || !alts) {
            fail(ps, "out of memory");
            return NULL;
        }
        alts[0] = head;
        g->alts = alts;
        g->nalts = 1;
        return g;
    }
    if (c == '\0' && at_end(ps)) {
        return NULL;  // caller decides whether that is an error
    }
    node *n = node_new(ps->re, N_CHAR);
    if (!n) {
        fail(ps, "out of memory");
        return NULL;
    }
    n->ch = (unsigned char)*ps->p++;
    return n;
}

// {n} {n,} {n,m} — ps->p is on '{'. Returns false when this brace is NOT a
// quantifier, in which case nothing is consumed and '{' is a literal (which is
// what ECMAScript's Annex B does, and what std::regex accepts).
static bool parse_brace(parser *ps, int *lo, int *hi) {
    const char *save = ps->p;
    const char *q = ps->p + 1;
    int a = 0;
    bool any = false;
    while (q < ps->end && *q >= '0' && *q <= '9') {
        a = a * 10 + (*q - '0');
        if (a > 1000000) {
            a = 1000000;
        }
        ++q;
        any = true;
    }
    if (!any) {
        ps->p = save;
        return false;
    }
    int b = a;
    if (q < ps->end && *q == ',') {
        ++q;
        if (q < ps->end && *q == '}') {
            b = INT_MAX;
        } else {
            b = 0;
            bool any2 = false;
            while (q < ps->end && *q >= '0' && *q <= '9') {
                b = b * 10 + (*q - '0');
                if (b > 1000000) {
                    b = 1000000;
                }
                ++q;
                any2 = true;
            }
            if (!any2) {
                ps->p = save;
                return false;
            }
        }
    }
    if (q >= ps->end || *q != '}') {
        ps->p = save;
        return false;
    }
    ps->p = q + 1;
    *lo = a;
    *hi = b;
    return true;
}

// One concatenation term: atom + optional quantifier.
static node *parse_term(parser *ps) {
    node *atom = parse_atom(ps);
    if (!atom || ps->failed) {
        return NULL;
    }
    int lo = -1, hi = -1;
    char c = peek(ps);
    if (c == '*') { lo = 0; hi = INT_MAX; ps->p++; }
    else if (c == '+') { lo = 1; hi = INT_MAX; ps->p++; }
    else if (c == '?') { lo = 0; hi = 1; ps->p++; }
    else if (c == '{') {
        if (!parse_brace(ps, &lo, &hi)) {
            return atom;  // a literal '{'; parse_atom will pick it up next
        }
    } else {
        return atom;
    }
    if (hi < lo) {
        fail(ps, "quantifier range is out of order");
        return NULL;
    }
    // An anchor or assertion cannot meaningfully repeat, and ECMAScript rejects
    // it; refusing here beats matching something surprising.
    if (atom->kind == N_BOL || atom->kind == N_EOL || atom->kind == N_WORDB ||
        atom->kind == N_NWORDB) {
        fail(ps, "quantifier applied to an assertion");
        return NULL;
    }
    node *rep = node_new(ps->re, N_REPEAT);
    if (!rep) {
        fail(ps, "out of memory");
        return NULL;
    }
    rep->child = atom;
    rep->min = lo;
    rep->max = hi;
    rep->greedy = true;
    if (peek(ps) == '?') {
        rep->greedy = false;
        ps->p++;
    } else if (peek(ps) == '+') {
        fail(ps, "possessive quantifiers are not supported");
        return NULL;
    }
    return rep;
}

// A concatenation, up to '|' or ')' or end.
static node *parse_concat(parser *ps) {
    node *head = NULL, *tail = NULL;
    while (!at_end(ps) && peek(ps) != '|' && peek(ps) != ')') {
        node *t = parse_term(ps);
        if (ps->failed) {
            return NULL;
        }
        if (!t) {
            break;
        }
        if (tail) {
            tail->next = t;
        } else {
            head = t;
        }
        tail = t;
    }
    return head;  // NULL head == the empty alternative, which is legal
}

// alt ('|' alt)*, wrapped in a group node so the caller has one handle.
static node *parse_alternation(parser *ps) {
    node  *first = parse_concat(ps);
    if (ps->failed) {
        return NULL;
    }
    size_t cap = 4, n = 0;
    node **alts = (node **)arena_alloc(ps->re, sizeof(node *) * cap);
    if (!alts) {
        fail(ps, "out of memory");
        return NULL;
    }
    alts[n++] = first;
    while (peek(ps) == '|') {
        ps->p++;
        node *a = parse_concat(ps);
        if (ps->failed) {
            return NULL;
        }
        if (n == cap) {
            size_t ncap = cap * 2;
            node **grown = (node **)arena_alloc(ps->re, sizeof(node *) * ncap);
            if (!grown) {
                fail(ps, "out of memory");
                return NULL;
            }
            memcpy(grown, alts, sizeof(node *) * n);
            alts = grown;
            cap = ncap;
        }
        alts[n++] = a;
    }
    node *g = node_new(ps->re, N_GROUP);
    if (!g) {
        fail(ps, "out of memory");
        return NULL;
    }
    g->alts = alts;
    g->nalts = n;
    return g;
}

// ---------------------------------------------------------------------------
// Matcher
// ---------------------------------------------------------------------------

// A continuation is either "carry on with chain `n`" or "resume repeat `rep`".
// `rep` is the discriminator, NOT `n`: a NULL `n` is the ordinary end-of-chain
// case and is extremely common, so keying on it sent every completed chain into
// the repeat path with a NULL repeat node.
typedef struct cont {
    const node  *n;    // chain to continue (may be NULL: end of chain)
    const node  *rep;  // non-NULL => this is a repeat frame, resume it
    int          count;
    const char  *last;  // where the previous repetition began (empty-match guard)
    const struct cont *up;
} cont;

typedef struct {
    const char *begin;
    const char *end;
    int         steps;  // budget: pathological patterns must not hang a query
} mctx;

// Backtracking is exponential on adversarial patterns like (a+)+b. A SQL
// predicate must never wedge the engine, so the walk is bounded; exhausting the
// budget reports "no match" rather than spinning.
#define REGEX_STEP_BUDGET 2000000

static bool m_seq(mctx *m, const node *n, const char *s, const cont *k);

static bool m_cont(mctx *m, const cont *k, const char *s);

static bool m_repeat(mctx *m, const node *rep, const char *s, const cont *k,
                     int count, const char *last) {
    if (--m->steps <= 0) {
        return false;
    }
    // The child matched nothing last time round; another go would be identical.
    if (count > 0 && s == last) {
        return m_seq(m, rep->next, s, k);
    }
    if (count < rep->min) {
        cont kk = {NULL, rep, count + 1, s, k};
        return m_seq(m, rep->child, s, &kk);
    }
    if (rep->greedy) {
        if (count < rep->max) {
            cont kk = {NULL, rep, count + 1, s, k};
            if (m_seq(m, rep->child, s, &kk)) {
                return true;
            }
        }
        return m_seq(m, rep->next, s, k);
    }
    if (m_seq(m, rep->next, s, k)) {
        return true;
    }
    if (count < rep->max) {
        cont kk = {NULL, rep, count + 1, s, k};
        return m_seq(m, rep->child, s, &kk);
    }
    return false;
}

static bool m_cont(mctx *m, const cont *k, const char *s) {
    if (!k) {
        return true;  // nothing left to satisfy: the whole pattern matched
    }
    if (k->rep) {
        return m_repeat(m, k->rep, s, k->up, k->count, k->last);
    }
    return m_seq(m, k->n, s, k->up);
}

static bool m_seq(mctx *m, const node *n, const char *s, const cont *k) {
    if (--m->steps <= 0) {
        return false;
    }
    if (!n) {
        return m_cont(m, k, s);
    }
    switch (n->kind) {
        case N_CHAR:
            if (s < m->end && (unsigned char)*s == n->ch) {
                return m_seq(m, n->next, s + 1, k);
            }
            return false;
        case N_ANY:
            if (s < m->end && *s != '\n' && *s != '\r') {
                return m_seq(m, n->next, s + 1, k);
            }
            return false;
        case N_CLASS:
            if (s < m->end && set_has(n->set, (unsigned char)*s)) {
                return m_seq(m, n->next, s + 1, k);
            }
            return false;
        // INPUT anchors, not line anchors: ECMA-262's `m` flag is off by
        // default. This is a place where std::regex implementations genuinely
        // disagree -- MSVC matches `^.$` against "a\nb", libstdc++ does not --
        // so "match the oracle" has no single answer and the spec wins.
        case N_BOL:
            return (s == m->begin) ? m_seq(m, n->next, s, k) : false;
        case N_EOL:
            return (s == m->end) ? m_seq(m, n->next, s, k) : false;
        case N_WORDB:
        case N_NWORDB: {
            bool before = s > m->begin && is_word_byte((unsigned char)s[-1]);
            bool after = s < m->end && is_word_byte((unsigned char)s[0]);
            bool boundary = before != after;
            bool want = (n->kind == N_WORDB);
            return boundary == want ? m_seq(m, n->next, s, k) : false;
        }
        case N_GROUP: {
            cont kk = {n->next, NULL, 0, NULL, k};
            for (size_t i = 0; i < n->nalts; ++i) {
                if (m_seq(m, n->alts[i], s, &kk)) {
                    return true;
                }
            }
            return false;
        }
        case N_REPEAT:
            return m_repeat(m, n, s, k, 0, NULL);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

xsql_regex *xsql_regex_compile(const char *pattern, char *errbuf,
                               size_t errbuf_len) {
    if (errbuf && errbuf_len > 0) {
        errbuf[0] = '\0';
    }
    if (!pattern) {
        if (errbuf && errbuf_len > 0) {
            snprintf(errbuf, errbuf_len, "null pattern");
        }
        return NULL;
    }
    xsql_regex *re = (xsql_regex *)xsqlc_malloc(sizeof(xsql_regex));
    if (!re) {
        if (errbuf && errbuf_len > 0) {
            snprintf(errbuf, errbuf_len, "out of memory");
        }
        return NULL;
    }
    re->blocks = NULL;
    re->root = NULL;

    parser ps;
    ps.re = re;
    ps.p = pattern;
    ps.end = pattern + strlen(pattern);
    ps.err = errbuf;
    ps.errlen = errbuf_len;
    ps.failed = false;

    node *root = parse_alternation(&ps);
    if (!root || ps.failed || !at_end(&ps)) {
        if (!ps.failed) {
            // Parsing stopped early without an explicit error: the only way in
            // is a stray ')'.
            fail(&ps, "unmatched )");
        }
        xsql_regex_free(re);
        return NULL;
    }
    re->root = root;
    return re;
}

void xsql_regex_free(xsql_regex *re) {
    if (!re) {
        return;
    }
    arena_block *b = re->blocks;
    while (b) {
        arena_block *nx = b->next;
        xsqlc_free(b->mem);
        xsqlc_free(b);
        b = nx;
    }
    xsqlc_free(re);
}

bool xsql_regex_search(const xsql_regex *re, const char *text) {
    if (!re || !re->root || !text) {
        return false;
    }
    mctx m;
    m.begin = text;
    m.end = text + strlen(text);
    m.steps = REGEX_STEP_BUDGET;
    // Try every start offset, including one past the last byte so an
    // empty-matching pattern still succeeds on "".
    for (const char *s = text; s <= m.end; ++s) {
        if (m_seq(&m, re->root, s, NULL)) {
            return true;
        }
        if (m.steps <= 0) {
            return false;
        }
    }
    return false;
}

bool xsql_regex_match(const xsql_regex *re, const char *text) {
    if (!re || !re->root || !text) {
        return false;
    }
    mctx m;
    m.begin = text;
    m.end = text + strlen(text);
    m.steps = REGEX_STEP_BUDGET;
    // A trailing N_EOL continuation is how "the whole thing" is expressed: run
    // the root, then require that we landed exactly at the end.
    node eol;
    memset(&eol, 0, sizeof(eol));
    eol.kind = N_EOL;
    cont k = {&eol, NULL, 0, NULL, NULL};
    return m_seq(&m, re->root, text, &k);
}
