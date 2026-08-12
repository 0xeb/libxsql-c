// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// thread_shim.h — cross-compiler threading primitives.
//
// Two surfaces:
//   1. The thread-local storage qualifier (`XSQL_THREAD_LOCAL`), used since
//      Phase 4 for the virtual-table error/interrupt channels.
//   2. A mutex + condition-variable + detached-thread surface (Phase 9), the
//      thin-client request queue's genuine wait/notify need. It is declared here
//      as OPAQUE handles and implemented in thread_shim.c so <windows.h> /
//      <pthread.h> stay out of every shared header (Windows: SRWLOCK +
//      CONDITION_VARIABLE; elsewhere: pthread_mutex_t + pthread_cond_t).
//
// Runtime settings (Phase 7) still guard themselves with sqlite3_mutex
// (SQLITE_MUTEX_FAST) — only the queue needs wait/notify, so only it grows this.
//
// PRIVATE: under src/internal, never installed, never in a public header.

#ifndef XSQLC_INTERNAL_THREAD_SHIM_H
#define XSQLC_INTERNAL_THREAD_SHIM_H

// Thread-local storage. MSVC's C mode does not accept C11 `_Thread_local`
// reliably, so use the vendor extension there; everything else is strict C11.
#if defined(_MSC_VER)
#  define XSQL_THREAD_LOCAL __declspec(thread)
#else
#  define XSQL_THREAD_LOCAL _Thread_local
#endif

// ---------------------------------------------------------------------------
// Mutex + condition variable + detached thread (opaque; heap-backed).
//
// All are no-throw. Init returns XSQL_OK (0) on success, XSQL_NO_MEMORY on
// allocation failure, leaving *out == NULL. Destroy is NULL-safe. Callers must
// re-check their predicate in a loop around wait (spurious wakeups are allowed).
// ---------------------------------------------------------------------------

typedef struct xsqlc_mtx  xsqlc_mtx;
typedef struct xsqlc_cond xsqlc_cond;

int  xsqlc_mtx_init(xsqlc_mtx **out);
void xsqlc_mtx_destroy(xsqlc_mtx *m);
void xsqlc_mtx_lock(xsqlc_mtx *m);
void xsqlc_mtx_unlock(xsqlc_mtx *m);

int  xsqlc_cond_init(xsqlc_cond **out);
void xsqlc_cond_destroy(xsqlc_cond *c);
// Wait until signaled or `timeout_ms` elapses. `timeout_ms == 0` waits
// indefinitely. The mutex must be held on entry; it is held again on return.
// Returns 1 if (possibly spuriously) woken, 0 on timeout.
int  xsqlc_cond_wait_for(xsqlc_cond *c, xsqlc_mtx *m, int timeout_ms);
void xsqlc_cond_signal(xsqlc_cond *c);
void xsqlc_cond_broadcast(xsqlc_cond *c);

// Spawn a detached one-shot worker running fn(arg). Used by the POST /shutdown
// handler (a server can't stop itself from inside a civetweb request thread).
// Returns XSQL_OK if the thread started, XSQL_ERROR otherwise.
int  xsqlc_thread_spawn_detached(void (*fn)(void *), void *arg);

#endif  // XSQLC_INTERNAL_THREAD_SHIM_H
