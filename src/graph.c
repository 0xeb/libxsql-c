// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// graph.c — C port of the general-purpose xsql::graph directed-graph algorithms
// (C++ header + Rust module). Stock, RE-agnostic: opaque integer node ids only.

#include <libxsqlc/graph.h>

#include "internal/internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Growable adjacency list per node.
typedef struct adj_list {
    size_t *items;
    size_t len;
    size_t cap;
} adj_list;

struct xsql_graph {
    size_t node_count;
    adj_list *succ;  // node_count lists
    adj_list *pred;  // node_count lists
};

static void *graph_alloc(size_t count, size_t item_size) {
    size_t bytes = 0;
    if (!xsqlc_mul_ok(count, item_size, &bytes)) return NULL;
    return xsqlc_malloc(bytes ? bytes : 1);
}

static void *graph_calloc(size_t count, size_t item_size) {
    size_t bytes = 0;
    if (!xsqlc_mul_ok(count, item_size, &bytes)) return NULL;
    void *out = xsqlc_malloc(bytes ? bytes : 1);
    if (out) memset(out, 0, bytes ? bytes : 1);
    return out;
}

static int adj_reserve_one(adj_list *a) {
    if (a->len < a->cap) return 1;
    if (a->cap > SIZE_MAX / 2) return 0;
    size_t ncap = a->cap ? a->cap * 2 : 4;
    size_t bytes = 0;
    if (!xsqlc_mul_ok(ncap, sizeof(size_t), &bytes)) return 0;
    size_t *ni = (size_t *)xsqlc_realloc(a->items, bytes);
    if (!ni) return 0;
    a->items = ni;
    a->cap = ncap;
    return 1;
}

static int adj_push(adj_list *a, size_t v) {
    if (!adj_reserve_one(a)) {
        return 0;
    }
    a->items[a->len++] = v;
    return 1;
}

xsql_graph *xsql_graph_create(size_t node_count) {
    xsql_graph *g = (xsql_graph *)graph_calloc(1, sizeof(xsql_graph));
    if (!g) return NULL;
    g->node_count = node_count;
    if (node_count > 0) {
        g->succ = (adj_list *)graph_calloc(node_count, sizeof(adj_list));
        g->pred = (adj_list *)graph_calloc(node_count, sizeof(adj_list));
        if (!g->succ || !g->pred) {
            xsqlc_free(g->succ);
            xsqlc_free(g->pred);
            xsqlc_free(g);
            return NULL;
        }
    }
    return g;
}

void xsql_graph_destroy(xsql_graph *g) {
    if (!g) return;
    for (size_t i = 0; i < g->node_count; ++i) {
        xsqlc_free(g->succ[i].items);
        xsqlc_free(g->pred[i].items);
    }
    xsqlc_free(g->succ);
    xsqlc_free(g->pred);
    xsqlc_free(g);
}

size_t xsql_graph_node_count(const xsql_graph *g) { return g ? g->node_count : 0; }

int xsql_graph_add_edge(xsql_graph *g, size_t from, size_t to) {
    if (!g || from >= g->node_count || to >= g->node_count) return XSQL_ERROR;
    adj_list *succ = &g->succ[from];
    adj_list *pred = &g->pred[to];
    if (!adj_reserve_one(succ) || !adj_reserve_one(pred)) return XSQL_ERROR;
    succ->items[succ->len++] = to;
    pred->items[pred->len++] = from;
    return XSQL_OK;
}

// ---------------------------------------------------------------------------
// Shared internals operating on raw adjacency (so the same idom routine serves a
// forward graph and a virtual-exit-augmented reverse graph).
// ---------------------------------------------------------------------------

// Iterative DFS from `entry` producing a postorder listing into `order`
// (capacity node_count). Returns 1 on success and writes the number of reachable
// nodes to `out_count`; returns 0 on allocation failure.
static int postorder_from(size_t node_count, size_t entry, const adj_list *succ,
                          size_t *order, size_t *out_count) {
    *out_count = 0;
    if (entry >= node_count) return 1;
    char *visited = (char *)graph_calloc(node_count, 1);
    size_t *stk_node = (size_t *)graph_alloc(node_count, sizeof(size_t));
    size_t *stk_idx = (size_t *)graph_alloc(node_count, sizeof(size_t));
    size_t count = 0;
    if (!visited || !stk_node || !stk_idx) {
        xsqlc_free(visited);
        xsqlc_free(stk_node);
        xsqlc_free(stk_idx);
        return 0;
    }
    size_t sp = 0;
    visited[entry] = 1;
    stk_node[sp] = entry;
    stk_idx[sp] = 0;
    ++sp;
    while (sp > 0) {
        size_t n = stk_node[sp - 1];
        size_t i = stk_idx[sp - 1];
        if (i < succ[n].len) {
            stk_idx[sp - 1] = i + 1;
            size_t m = succ[n].items[i];
            if (m < node_count && !visited[m]) {
                visited[m] = 1;
                stk_node[sp] = m;
                stk_idx[sp] = 0;
                ++sp;
            }
        } else {
            order[count++] = n;
            --sp;
        }
    }
    xsqlc_free(visited);
    xsqlc_free(stk_node);
    xsqlc_free(stk_idx);
    *out_count = count;
    return 1;
}

// Cooper-Harvey-Kennedy immediate dominators over raw succ/pred adjacency. Writes
// node_count ids into `idom`. Returns 1 on success, 0 on OOM.
static int idom_over(size_t node_count, size_t entry, const adj_list *succ,
                     const adj_list *pred, size_t *idom) {
    for (size_t i = 0; i < node_count; ++i) idom[i] = XSQL_GRAPH_NO_NODE;
    if (entry >= node_count) return 1;

    size_t *post = (size_t *)graph_alloc(node_count, sizeof(size_t));
    size_t *postnum = (size_t *)graph_alloc(node_count, sizeof(size_t));
    if (!post || !postnum) {
        xsqlc_free(post);
        xsqlc_free(postnum);
        return 0;
    }
    size_t pcount = 0;
    if (!postorder_from(node_count, entry, succ, post, &pcount)) {
        xsqlc_free(post);
        xsqlc_free(postnum);
        return 0;
    }
    for (size_t i = 0; i < node_count; ++i) postnum[i] = XSQL_GRAPH_NO_NODE;
    for (size_t i = 0; i < pcount; ++i) postnum[post[i]] = i;

    idom[entry] = entry;
    int changed = 1;
    while (changed) {
        changed = 0;
        // Reverse postorder: iterate post[] from the back (entry first).
        for (size_t ri = pcount; ri-- > 0;) {
            size_t b = post[ri];
            if (b == entry) continue;
            size_t new_idom = XSQL_GRAPH_NO_NODE;
            for (size_t k = 0; k < pred[b].len; ++k) {
                size_t p = pred[b].items[k];
                if (p >= node_count || postnum[p] == XSQL_GRAPH_NO_NODE) continue;
                if (idom[p] == XSQL_GRAPH_NO_NODE && p != entry) continue;
                if (new_idom == XSQL_GRAPH_NO_NODE) {
                    new_idom = p;
                } else {
                    // intersect(p, new_idom) using postorder numbers.
                    size_t a = p, c = new_idom;
                    while (a != c) {
                        while (postnum[a] < postnum[c]) a = idom[a];
                        while (postnum[c] < postnum[a]) c = idom[c];
                    }
                    new_idom = a;
                }
            }
            if (new_idom != XSQL_GRAPH_NO_NODE && idom[b] != new_idom) {
                idom[b] = new_idom;
                changed = 1;
            }
        }
    }
    xsqlc_free(post);
    xsqlc_free(postnum);
    return 1;
}

int xsql_graph_immediate_dominators(const xsql_graph *g, size_t entry, size_t *out) {
    if (!g || !out) return XSQL_ERROR;
    return idom_over(g->node_count, entry, g->succ, g->pred, out) ? XSQL_OK : XSQL_ERROR;
}

int xsql_graph_immediate_post_dominators(const xsql_graph *g, size_t *out) {
    if (!g || !out) return XSQL_ERROR;
    size_t n = g->node_count;
    if (n == SIZE_MAX) return XSQL_ERROR;
    size_t virt = n;  // virtual exit id
    size_t m = n + 1;

    adj_list *rsucc = (adj_list *)graph_calloc(m, sizeof(adj_list));
    adj_list *rpred = (adj_list *)graph_calloc(m, sizeof(adj_list));
    size_t *ipdom = (size_t *)graph_alloc(m, sizeof(size_t));
    int ok = rsucc && rpred && ipdom;
    if (ok) {
        for (size_t u = 0; u < n && ok; ++u) {
            for (size_t k = 0; k < g->succ[u].len && ok; ++k) {
                size_t v = g->succ[u].items[k];
                if (!adj_push(&rsucc[v], u) || !adj_push(&rpred[u], v)) ok = 0;
            }
            if (ok && g->succ[u].len == 0) {
                if (!adj_push(&rsucc[virt], u) || !adj_push(&rpred[u], virt)) ok = 0;
            }
        }
    }
    if (ok) ok = idom_over(m, virt, rsucc, rpred, ipdom);
    if (ok) {
        for (size_t x = 0; x < n; ++x) {
            size_t d = ipdom[x];
            out[x] = (d == XSQL_GRAPH_NO_NODE || d == virt) ? XSQL_GRAPH_NO_NODE : d;
        }
    }
    if (rsucc)
        for (size_t i = 0; i < m; ++i) xsqlc_free(rsucc[i].items);
    if (rpred)
        for (size_t i = 0; i < m; ++i) xsqlc_free(rpred[i].items);
    xsqlc_free(rsucc);
    xsqlc_free(rpred);
    xsqlc_free(ipdom);
    return ok ? XSQL_OK : XSQL_ERROR;
}

// ---------------------------------------------------------------------------
// Strongly connected components (iterative Tarjan).
// ---------------------------------------------------------------------------
int xsql_graph_strongly_connected_components(const xsql_graph *g, size_t *out_comp,
                                             size_t *out_component_count) {
    if (!g || !out_comp) return XSQL_ERROR;
    size_t n = g->node_count;
    if (out_component_count) *out_component_count = 0;

    size_t *index = (size_t *)graph_alloc(n, sizeof(size_t));
    size_t *low = (size_t *)graph_alloc(n, sizeof(size_t));
    char *on_stack = (char *)graph_calloc(n, 1);
    size_t *scc_stack = (size_t *)graph_alloc(n, sizeof(size_t));
    size_t *wk_node = (size_t *)graph_alloc(n, sizeof(size_t));
    size_t *wk_idx = (size_t *)graph_alloc(n, sizeof(size_t));
    if (!index || !low || !on_stack || !scc_stack || !wk_node || !wk_idx) {
        xsqlc_free(index); xsqlc_free(low); xsqlc_free(on_stack);
        xsqlc_free(scc_stack); xsqlc_free(wk_node); xsqlc_free(wk_idx);
        return XSQL_ERROR;
    }
    for (size_t i = 0; i < n; ++i) {
        index[i] = XSQL_GRAPH_NO_NODE;
        out_comp[i] = XSQL_GRAPH_NO_NODE;
    }
    size_t ssp = 0, next_index = 0, next_comp = 0;

    for (size_t root = 0; root < n; ++root) {
        if (index[root] != XSQL_GRAPH_NO_NODE) continue;
        size_t wsp = 0;
        wk_node[wsp] = root;
        wk_idx[wsp] = 0;
        ++wsp;
        while (wsp > 0) {
            size_t v = wk_node[wsp - 1];
            size_t i = wk_idx[wsp - 1];
            if (i == 0) {
                index[v] = low[v] = next_index++;
                scc_stack[ssp++] = v;
                on_stack[v] = 1;
            }
            if (i < g->succ[v].len) {
                wk_idx[wsp - 1] = i + 1;
                size_t w = g->succ[v].items[i];
                if (w >= n) continue;
                if (index[w] == XSQL_GRAPH_NO_NODE) {
                    wk_node[wsp] = w;
                    wk_idx[wsp] = 0;
                    ++wsp;
                } else if (on_stack[w]) {
                    if (index[w] < low[v]) low[v] = index[w];
                }
            } else {
                if (low[v] == index[v]) {
                    for (;;) {
                        size_t w = scc_stack[--ssp];
                        on_stack[w] = 0;
                        out_comp[w] = next_comp;
                        if (w == v) break;
                    }
                    ++next_comp;
                }
                --wsp;
                if (wsp > 0) {
                    size_t parent = wk_node[wsp - 1];
                    if (low[v] < low[parent]) low[parent] = low[v];
                }
            }
        }
    }
    if (out_component_count) *out_component_count = next_comp;
    xsqlc_free(index); xsqlc_free(low); xsqlc_free(on_stack);
    xsqlc_free(scc_stack); xsqlc_free(wk_node); xsqlc_free(wk_idx);
    return XSQL_OK;
}

// ---------------------------------------------------------------------------
// Topological order (Kahn). Smallest-id-first among ready nodes.
// ---------------------------------------------------------------------------
int xsql_graph_topological_order(const xsql_graph *g, size_t *out_order) {
    if (!g || !out_order) return XSQL_ERROR;
    size_t n = g->node_count;
    size_t *indeg = (size_t *)graph_calloc(n, sizeof(size_t));
    char *done = (char *)graph_calloc(n, 1);
    if (!indeg || !done) {
        xsqlc_free(indeg);
        xsqlc_free(done);
        return XSQL_ERROR;
    }
    for (size_t u = 0; u < n; ++u)
        for (size_t k = 0; k < g->succ[u].len; ++k)
            if (g->succ[u].items[k] < n) ++indeg[g->succ[u].items[k]];

    // Repeatedly emit the smallest-id ready node (O(n^2), fine for these sizes and
    // avoids a heap dependency).
    size_t emitted = 0;
    for (size_t step = 0; step < n; ++step) {
        size_t pick = XSQL_GRAPH_NO_NODE;
        for (size_t u = 0; u < n; ++u) {
            if (!done[u] && indeg[u] == 0) {
                pick = u;
                break;
            }
        }
        if (pick == XSQL_GRAPH_NO_NODE) break;  // cycle: no ready node
        done[pick] = 1;
        out_order[emitted++] = pick;
        for (size_t k = 0; k < g->succ[pick].len; ++k) {
            size_t v = g->succ[pick].items[k];
            if (v < n && indeg[v] > 0) --indeg[v];
        }
    }
    xsqlc_free(indeg);
    xsqlc_free(done);
    return emitted == n ? XSQL_OK : XSQL_ERROR;  // ERROR == cycle
}

// ---------------------------------------------------------------------------
// Natural loops.
// ---------------------------------------------------------------------------
static int size_cmp(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int xsql_graph_natural_loops(const xsql_graph *g, size_t entry, xsql_natural_loops *out) {
    if (!g || !out) return XSQL_ERROR;
    out->loops = NULL;
    out->count = 0;
    size_t n = g->node_count;

    size_t *idom = (size_t *)graph_alloc(n, sizeof(size_t));
    if (!idom) return XSQL_ERROR;
    if (!idom_over(n, entry, g->succ, g->pred, idom)) {
        xsqlc_free(idom);
        return XSQL_ERROR;
    }

    size_t cap = 0;
    for (size_t latch = 0; latch < n; ++latch) {
        if (idom[latch] == XSQL_GRAPH_NO_NODE) continue;
        for (size_t k = 0; k < g->succ[latch].len; ++k) {
            size_t header = g->succ[latch].items[k];
            if (header >= n) continue;
            // dominates(header, latch)?
            int dominated = 0;
            size_t cur = latch;
            for (;;) {
                if (cur == header) { dominated = 1; break; }
                if (cur == entry) { dominated = (header == entry); break; }
                cur = idom[cur];
            }
            if (!dominated) continue;

            // Backward walk from latch, bounded by header.
            char *in_body = (char *)graph_calloc(n, 1);
            size_t *stack = (size_t *)graph_alloc(n, sizeof(size_t));
            if (!in_body || !stack) {
                xsqlc_free(in_body); xsqlc_free(stack); xsqlc_free(idom);
                xsql_graph_natural_loops_free(out);
                return XSQL_ERROR;
            }
            size_t sp = 0;
            in_body[header] = 1;
            if (!in_body[latch]) { in_body[latch] = 1; stack[sp++] = latch; }
            while (sp > 0) {
                size_t mnode = stack[--sp];
                for (size_t j = 0; j < g->pred[mnode].len; ++j) {
                    size_t p = g->pred[mnode].items[j];
                    if (p < n && idom[p] != XSQL_GRAPH_NO_NODE && !in_body[p]) {
                        in_body[p] = 1;
                        stack[sp++] = p;
                    }
                }
            }
            size_t bcount = 0;
            for (size_t x = 0; x < n; ++x) if (in_body[x]) ++bcount;
            size_t *body = (size_t *)graph_alloc(bcount, sizeof(size_t));
            if (!body) {
                xsqlc_free(in_body); xsqlc_free(stack); xsqlc_free(idom);
                xsql_graph_natural_loops_free(out);
                return XSQL_ERROR;
            }
            size_t bi = 0;
            for (size_t x = 0; x < n; ++x) if (in_body[x]) body[bi++] = x;
            qsort(body, bcount, sizeof(size_t), size_cmp);
            xsqlc_free(in_body);
            xsqlc_free(stack);

            if (out->count == cap) {
                if (cap > SIZE_MAX / 2) {
                    xsqlc_free(body); xsqlc_free(idom);
                    xsql_graph_natural_loops_free(out);
                    return XSQL_ERROR;
                }
                size_t ncap = cap ? cap * 2 : 4;
                size_t bytes = 0;
                if (!xsqlc_mul_ok(ncap, sizeof(xsql_natural_loop), &bytes)) {
                    xsqlc_free(body); xsqlc_free(idom);
                    xsql_graph_natural_loops_free(out);
                    return XSQL_ERROR;
                }
                xsql_natural_loop *nl =
                    (xsql_natural_loop *)xsqlc_realloc(out->loops, bytes);
                if (!nl) {
                    xsqlc_free(body); xsqlc_free(idom);
                    xsql_graph_natural_loops_free(out);
                    return XSQL_ERROR;
                }
                out->loops = nl;
                cap = ncap;
            }
            out->loops[out->count].header = header;
            out->loops[out->count].latch = latch;
            out->loops[out->count].body = body;
            out->loops[out->count].body_count = bcount;
            ++out->count;
        }
    }
    xsqlc_free(idom);
    return XSQL_OK;
}

void xsql_graph_natural_loops_free(xsql_natural_loops *loops) {
    if (!loops) return;
    for (size_t i = 0; i < loops->count; ++i) xsqlc_free(loops->loops[i].body);
    xsqlc_free(loops->loops);
    loops->loops = NULL;
    loops->count = 0;
}

// ---------------------------------------------------------------------------
// Dominator sets.
// ---------------------------------------------------------------------------
int xsql_graph_dominator_sets(const xsql_graph *g, size_t entry, xsql_dominator_sets *out) {
    if (!g || !out) return XSQL_ERROR;
    size_t n = g->node_count;
    out->node_count = n;
    out->sets = (size_t **)graph_calloc(n, sizeof(size_t *));
    out->set_sizes = (size_t *)graph_calloc(n, sizeof(size_t));
    size_t *idom = (size_t *)graph_alloc(n, sizeof(size_t));
    if (!out->sets || !out->set_sizes || !idom) {
        xsqlc_free(idom);
        xsql_graph_dominator_sets_free(out);
        return XSQL_ERROR;
    }
    if (!idom_over(n, entry, g->succ, g->pred, idom)) {
        xsqlc_free(idom);
        xsql_graph_dominator_sets_free(out);
        return XSQL_ERROR;
    }
    for (size_t node = 0; node < n; ++node) {
        if (idom[node] == XSQL_GRAPH_NO_NODE) continue;  // unreachable -> NULL/0
        // Walk the idom chain to count, then fill.
        size_t cnt = 1, cur = node;
        while (cur != entry) { cur = idom[cur]; ++cnt; }
        size_t *set = (size_t *)graph_alloc(cnt, sizeof(size_t));
        if (!set) {
            xsqlc_free(idom);
            xsql_graph_dominator_sets_free(out);
            return XSQL_ERROR;
        }
        size_t si = 0;
        cur = node;
        set[si++] = cur;
        while (cur != entry) { cur = idom[cur]; set[si++] = cur; }
        qsort(set, cnt, sizeof(size_t), size_cmp);
        out->sets[node] = set;
        out->set_sizes[node] = cnt;
    }
    xsqlc_free(idom);
    return XSQL_OK;
}

void xsql_graph_dominator_sets_free(xsql_dominator_sets *sets) {
    if (!sets) return;
    if (sets->sets) {
        for (size_t i = 0; i < sets->node_count; ++i) xsqlc_free(sets->sets[i]);
        xsqlc_free(sets->sets);
    }
    xsqlc_free(sets->set_sizes);
    sets->sets = NULL;
    sets->set_sizes = NULL;
    sets->node_count = 0;
}
