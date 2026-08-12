// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// thread_shim.c — the mutex + condition-variable + detached-thread surface
// declared in internal/thread_shim.h. This is the ONLY translation unit that
// pulls in <windows.h> / <pthread.h>; keeping them here (never in a shared
// header) mirrors the file-local monotonic-clock pattern in result.c.
//
// POSIX cond timeouts are absolute times from CLOCK_REALTIME, so the feature
// macro must precede every include (same discipline as result.c's mono clock).

#if !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "internal/thread_shim.h"

#include "internal/internal.h"
#include "libxsqlc/status.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

struct xsqlc_mtx {
    SRWLOCK lock;
};
struct xsqlc_cond {
    CONDITION_VARIABLE cv;
};

int xsqlc_mtx_init(xsqlc_mtx **out) {
    if (out) {
        *out = NULL;
    }
    if (!out) {
        return XSQL_MISUSE;
    }
    xsqlc_mtx *m = (xsqlc_mtx *)xsqlc_malloc(sizeof(*m));
    if (!m) {
        return XSQL_NO_MEMORY;
    }
    InitializeSRWLock(&m->lock);
    *out = m;
    return XSQL_OK;
}

void xsqlc_mtx_destroy(xsqlc_mtx *m) {
    // SRWLOCK needs no explicit destruction.
    xsqlc_free(m);
}

void xsqlc_mtx_lock(xsqlc_mtx *m) {
    AcquireSRWLockExclusive(&m->lock);
}

void xsqlc_mtx_unlock(xsqlc_mtx *m) {
    ReleaseSRWLockExclusive(&m->lock);
}

int xsqlc_cond_init(xsqlc_cond **out) {
    if (out) {
        *out = NULL;
    }
    if (!out) {
        return XSQL_MISUSE;
    }
    xsqlc_cond *c = (xsqlc_cond *)xsqlc_malloc(sizeof(*c));
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    InitializeConditionVariable(&c->cv);
    *out = c;
    return XSQL_OK;
}

void xsqlc_cond_destroy(xsqlc_cond *c) {
    // CONDITION_VARIABLE needs no explicit destruction.
    xsqlc_free(c);
}

int xsqlc_cond_wait_for(xsqlc_cond *c, xsqlc_mtx *m, int timeout_ms) {
    DWORD ms = (timeout_ms <= 0) ? INFINITE : (DWORD)timeout_ms;
    BOOL ok = SleepConditionVariableSRW(&c->cv, &m->lock, ms, 0);
    if (ok) {
        return 1;  // woken (possibly spuriously)
    }
    return 0;      // timed out (or an error we treat as a wakeup-worthy retry)
}

void xsqlc_cond_signal(xsqlc_cond *c) {
    WakeConditionVariable(&c->cv);
}

void xsqlc_cond_broadcast(xsqlc_cond *c) {
    WakeAllConditionVariable(&c->cv);
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} detached_ctx;

static DWORD WINAPI detached_trampoline(LPVOID param) {
    detached_ctx *ctx = (detached_ctx *)param;
    void (*fn)(void *) = ctx->fn;
    void *arg = ctx->arg;
    xsqlc_free(ctx);
    fn(arg);
    return 0;
}

int xsqlc_thread_spawn_detached(void (*fn)(void *), void *arg) {
    if (!fn) {
        return XSQL_MISUSE;
    }
    detached_ctx *ctx = (detached_ctx *)xsqlc_malloc(sizeof(*ctx));
    if (!ctx) {
        return XSQL_NO_MEMORY;
    }
    ctx->fn = fn;
    ctx->arg = arg;
    HANDLE h = CreateThread(NULL, 0, detached_trampoline, ctx, 0, NULL);
    if (!h) {
        xsqlc_free(ctx);
        return XSQL_ERROR;
    }
    CloseHandle(h);  // detach: the thread runs to completion on its own
    return XSQL_OK;
}

#else  // POSIX

#include <errno.h>
#include <pthread.h>
#include <time.h>

struct xsqlc_mtx {
    pthread_mutex_t mtx;
};
struct xsqlc_cond {
    pthread_cond_t cv;
};

int xsqlc_mtx_init(xsqlc_mtx **out) {
    if (out) {
        *out = NULL;
    }
    if (!out) {
        return XSQL_MISUSE;
    }
    xsqlc_mtx *m = (xsqlc_mtx *)xsqlc_malloc(sizeof(*m));
    if (!m) {
        return XSQL_NO_MEMORY;
    }
    if (pthread_mutex_init(&m->mtx, NULL) != 0) {
        xsqlc_free(m);
        return XSQL_ERROR;
    }
    *out = m;
    return XSQL_OK;
}

void xsqlc_mtx_destroy(xsqlc_mtx *m) {
    if (!m) {
        return;
    }
    pthread_mutex_destroy(&m->mtx);
    xsqlc_free(m);
}

void xsqlc_mtx_lock(xsqlc_mtx *m) {
    pthread_mutex_lock(&m->mtx);
}

void xsqlc_mtx_unlock(xsqlc_mtx *m) {
    pthread_mutex_unlock(&m->mtx);
}

int xsqlc_cond_init(xsqlc_cond **out) {
    if (out) {
        *out = NULL;
    }
    if (!out) {
        return XSQL_MISUSE;
    }
    xsqlc_cond *c = (xsqlc_cond *)xsqlc_malloc(sizeof(*c));
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    if (pthread_cond_init(&c->cv, NULL) != 0) {
        xsqlc_free(c);
        return XSQL_ERROR;
    }
    *out = c;
    return XSQL_OK;
}

void xsqlc_cond_destroy(xsqlc_cond *c) {
    if (!c) {
        return;
    }
    pthread_cond_destroy(&c->cv);
    xsqlc_free(c);
}

int xsqlc_cond_wait_for(xsqlc_cond *c, xsqlc_mtx *m, int timeout_ms) {
    if (timeout_ms <= 0) {
        pthread_cond_wait(&c->cv, &m->mtx);
        return 1;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long add_sec = timeout_ms / 1000;
    long add_nsec = (long)(timeout_ms % 1000) * 1000000L;
    ts.tv_sec += add_sec;
    ts.tv_nsec += add_nsec;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(&c->cv, &m->mtx, &ts);
    return (rc == ETIMEDOUT) ? 0 : 1;
}

void xsqlc_cond_signal(xsqlc_cond *c) {
    pthread_cond_signal(&c->cv);
}

void xsqlc_cond_broadcast(xsqlc_cond *c) {
    pthread_cond_broadcast(&c->cv);
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} detached_ctx;

static void *detached_trampoline(void *param) {
    detached_ctx *ctx = (detached_ctx *)param;
    void (*fn)(void *) = ctx->fn;
    void *arg = ctx->arg;
    xsqlc_free(ctx);
    fn(arg);
    return NULL;
}

int xsqlc_thread_spawn_detached(void (*fn)(void *), void *arg) {
    if (!fn) {
        return XSQL_MISUSE;
    }
    detached_ctx *ctx = (detached_ctx *)xsqlc_malloc(sizeof(*ctx));
    if (!ctx) {
        return XSQL_NO_MEMORY;
    }
    ctx->fn = fn;
    ctx->arg = arg;
    pthread_t tid;
    if (pthread_create(&tid, NULL, detached_trampoline, ctx) != 0) {
        xsqlc_free(ctx);
        return XSQL_ERROR;
    }
    pthread_detach(tid);
    return XSQL_OK;
}

#endif  // _WIN32
