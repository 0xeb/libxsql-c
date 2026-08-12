// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// graph.h — a general-purpose directed-graph algorithm utility (the C port of the
// C++ xsql::graph header and the Rust xsql::graph module).
//
// This is a STOCK graph library, not a reverse-engineering facility: it knows
// nothing about addresses, basic blocks, control flow, or any application concept. A
// graph is a node count plus a directed edge list over opaque node ids in
// [0, node_count). Any caller that can map its objects onto integer ids can use it.
//
// Fixed-size results (immediate dominators, immediate post-dominators, SCC ids,
// topological order) are written into a caller-provided `out` buffer of node_count
// elements. Variable-size results (dominator sets, natural loops) return a
// heap-allocated result struct that the caller frees with the matching *_free.

#ifndef XSQLC_GRAPH_H
#define XSQLC_GRAPH_H

#include <libxsqlc/common.h>
#include <libxsqlc/status.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sentinel for "no node" (an unreachable node's dominator, a node that
// post-dominates straight to the virtual exit, etc.).
#define XSQL_GRAPH_NO_NODE ((size_t)-1)

// Opaque directed graph over node ids [0, node_count).
typedef struct xsql_graph xsql_graph;

// Create a graph with `node_count` nodes and no edges. Returns NULL on OOM.
XSQL_API xsql_graph *xsql_graph_create(size_t node_count);

// Destroy a graph created by xsql_graph_create. NULL-safe.
XSQL_API void xsql_graph_destroy(xsql_graph *g);

// Number of nodes.
XSQL_API size_t xsql_graph_node_count(const xsql_graph *g);

// Add a directed edge from -> to. Both ids must be < node_count(). Returns XSQL_OK,
// or XSQL_ERROR on OOM / out-of-range ids.
XSQL_API int xsql_graph_add_edge(xsql_graph *g, size_t from, size_t to);

// Immediate dominators relative to `entry`. Writes node_count ids into `out`:
// out[entry] == entry; a node unreachable from entry gets XSQL_GRAPH_NO_NODE.
XSQL_API int xsql_graph_immediate_dominators(const xsql_graph *g, size_t entry,
                                              size_t *out);

// Immediate post-dominators (via a virtual exit that all sinks feed). Writes
// node_count ids into `out`; a node that post-dominates straight to the exit (a sink,
// or one that only reaches a sink) or cannot reach any sink gets XSQL_GRAPH_NO_NODE.
XSQL_API int xsql_graph_immediate_post_dominators(const xsql_graph *g, size_t *out);

// Strongly connected components (Tarjan). Writes a component id per node into
// `out_comp` (node_count elements); nodes in the same SCC share an id. If
// `out_component_count` is non-NULL it receives the number of components.
XSQL_API int xsql_graph_strongly_connected_components(const xsql_graph *g,
                                                       size_t *out_comp,
                                                       size_t *out_component_count);

// Topological order (Kahn). On a DAG, writes a full ordering into `out_order`
// (node_count elements, smallest-id-first among ready nodes) and returns XSQL_OK. If
// the graph has a directed cycle, returns XSQL_ERROR and leaves `out_order` unspecified.
XSQL_API int xsql_graph_topological_order(const xsql_graph *g, size_t *out_order);

// One natural loop: header (dominates the whole body), latch (back-edge tail), and a
// sorted body array (header + latch included).
typedef struct xsql_natural_loop {
    size_t header;
    size_t latch;
    size_t *body;       // owned by the containing xsql_natural_loops
    size_t body_count;
} xsql_natural_loop;

typedef struct xsql_natural_loops {
    xsql_natural_loop *loops;  // heap array, `count` entries
    size_t count;
} xsql_natural_loops;

// Natural loops relative to `entry`. Fills `out` with a heap-allocated array; free it
// with xsql_graph_natural_loops_free. Returns XSQL_OK (0 loops is a valid result).
XSQL_API int xsql_graph_natural_loops(const xsql_graph *g, size_t entry,
                                       xsql_natural_loops *out);

// Free the arrays owned by an xsql_natural_loops filled by xsql_graph_natural_loops.
// NULL-safe; leaves the struct zeroed.
XSQL_API void xsql_graph_natural_loops_free(xsql_natural_loops *loops);

// Full dominator sets. sets[n] is a sorted array of the `set_sizes[n]` nodes that
// dominate n (including n); an unreachable node gets sets[n] == NULL, set_sizes[n] == 0.
typedef struct xsql_dominator_sets {
    size_t node_count;
    size_t **sets;       // node_count pointers, each owned
    size_t *set_sizes;   // node_count counts
} xsql_dominator_sets;

// Compute dominator sets relative to `entry`. Fills `out`; free with
// xsql_graph_dominator_sets_free. Returns XSQL_OK, or XSQL_ERROR on OOM.
XSQL_API int xsql_graph_dominator_sets(const xsql_graph *g, size_t entry,
                                        xsql_dominator_sets *out);

// Free the arrays owned by an xsql_dominator_sets. NULL-safe; leaves it zeroed.
XSQL_API void xsql_graph_dominator_sets_free(xsql_dominator_sets *sets);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XSQLC_GRAPH_H
