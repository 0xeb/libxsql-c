// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// generator_internal.h — concrete layout of the generator-table definition:
// columns (type-erased getters/setters over a `const void *`/`void *` row),
// single-column / parametric / constraint filters, and the runtime plan
// constants. Shared privately by table_generator.c only. PRIVATE: under
// src/internal, never installed, never in a public header.

#ifndef XSQLC_INTERNAL_GENERATOR_INTERNAL_H
#define XSQLC_INTERNAL_GENERATOR_INTERNAL_H

#include <libxsqlc/table_generator.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Column get/set dispatch kinds (no closures in C). GGET_NULL is a hidden column
// (always emits NULL); the rest mirror the cached table's kinds.
enum { GGET_NULL, GGET_I64, GGET_INT, GGET_DOUBLE, GGET_EMIT };
enum { GSET_NONE, GSET_I64, GSET_INT, GSET_TEXT, GSET_VALUE };

typedef struct {
    char            *name;
    xsql_column_type type;
    bool             writable;
    bool             hidden;

    int get_kind;
    union {
        xsql_grow_i64_fn    i64;
        xsql_grow_int_fn    i;
        xsql_grow_double_fn d;
        xsql_grow_emit_fn   emit;
    } get;

    int set_kind;
    union {
        xsql_grow_set_i64_fn   i64;
        xsql_grow_set_int_fn   i;
        xsql_grow_set_text_fn  text;
        xsql_grow_set_value_fn value;
    } set;

    void *user;
} gen_column;

// Single-column equality pushdown (filter_eq / filter_eq_text). filter_id 1..n.
typedef struct {
    int                    column_index;
    int                    filter_id;
    double                 cost;
    double                 est_rows;
    xsql_filter_op         op;  // always EQ for the generator's single-column path
    xsql_filter_factory_fn factory;
    void                  *user;
} gen_filter;

// Parametric filter: all listed columns must be EQ-constrained. filter_id 500+n.
typedef struct {
    int                          *column_indices;  // resolved, owned
    size_t                        ncolumns;
    int                           filter_id;
    double                        cost;
    double                        est_rows;
    xsql_generator_parametric_fn  factory;
    void                         *user;
} gen_parametric_filter;

// One required/optional constraint within a constraint filter.
typedef struct {
    int                column_index;
    xsql_constraint_op op;
    bool               required;
    char              *missing_error;  // owned copy (may be NULL)
} gen_constraint_spec;

// General multi-operator constraint filter. filter_id 2000+n. `order_by_consumed`
// tags this (the most-recently-added) filter so xBestIndex can elide the sort.
typedef struct {
    gen_constraint_spec         *specs;
    size_t                       nspecs;
    int                          filter_id;
    double                       cost;
    double                       est_rows;
    bool                         has_order;
    int                          ordered_column;
    bool                         ordered_desc;
    xsql_generator_constraint_fn factory;
    void                        *user;
} gen_constraint_filter;

struct xsql_generator_table {
    char  *name;
    size_t row_size;

    xsql_estimate_rows_fn  estimate_fn;
    void                  *estimate_user;
    xsql_row_count_fn      count_fn;
    void                  *count_user;
    xsql_generator_make_fn generator_fn;  // full-scan factory (required to build)
    void                  *generator_user;
    // Optional projection-aware full-scan factory (colUsed): overrides generator_fn on
    // the full-scan plan so it can skip expensive unused columns. NULL => plain factory.
    xsql_generator_make_projection_fn projection_generator_fn;
    void                             *projection_generator_user;
    char                  *full_scan_error;  // owned; NULL => full scan allowed

    gen_column *columns;
    size_t      ncols, cols_cap;
    gen_filter *filters;
    size_t      nfilters, filters_cap;
    gen_parametric_filter *parametric;
    size_t                 nparam, param_cap;
    gen_constraint_filter *constraints;
    size_t                 nconstr, constr_cap;

    xsql_generator_modify_fn     before_modify_fn;
    void                        *before_user;
    xsql_generator_modify_fn     after_modify_fn;
    void                        *after_user;
    xsql_transaction_hooks       transaction_hooks;
    void                        *transaction_user;
    xsql_generator_delete_fn     delete_fn;
    void                        *delete_user;
    bool                         supports_delete;
    xsql_generator_insert_fn     insert_fn;
    void                        *insert_user;
    bool                         supports_insert;
    xsql_generator_row_lookup_fn lookup_fn;
    void                        *lookup_user;
};

#endif  // XSQLC_INTERNAL_GENERATOR_INTERNAL_H
