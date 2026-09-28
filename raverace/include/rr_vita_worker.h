#ifndef RR_VITA_WORKER_H
#define RR_VITA_WORKER_H
/* One in-flight job, owned by one dispatching thread. Each completion is
 * acquired before the caller reads results or resumes the emulated CPU. */
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#endif

typedef struct {
    SceUID thread, start, done;
    atomic_bool ready, complete;
    void (*run)(void *);
    void *arg;
    bool pending;
#ifdef __vita__
    uint64_t dispatched_us, started_us, finished_us;
    int cpu_id;
#endif
} rr_vita_worker;

static int rr_worker_entry(SceSize size, void *data) {
    (void)size;
    rr_vita_worker *w = *(rr_vita_worker **)data;
    for (;;) {
        if (sceKernelWaitSema(w->start, 1, NULL) < 0) return 0;
        if (!atomic_load_explicit(&w->ready, memory_order_acquire)) return 0;
#ifdef __vita__
        w->started_us = sceKernelGetProcessTimeWide();
        w->cpu_id = sceKernelGetCpuId();
#endif
        w->run(w->arg);
#ifdef __vita__
        w->finished_us = sceKernelGetProcessTimeWide();
#endif
        atomic_store_explicit(&w->complete, true, memory_order_release);
        if (sceKernelSignalSema(w->done, 1) < 0) abort();
    }
}

static bool rr_worker_init(rr_vita_worker *w, const char *name, int affinity) {
    w->thread = w->start = w->done = -1;
    w->pending = false;
    atomic_init(&w->ready, false); atomic_init(&w->complete, false);
    w->start = sceKernelCreateSema(name, 0, 0, 1, NULL);
    w->done = sceKernelCreateSema(name, 0, 0, 1, NULL);
    if (w->start >= 0 && w->done >= 0) {
        w->thread = sceKernelCreateThread(name, rr_worker_entry, 0x10000100,
                                         128 * 1024, 0, affinity, NULL);
        if (w->thread >= 0 && sceKernelStartThread(w->thread, sizeof w, &w) >= 0) return true;
    }
    if (w->thread >= 0) sceKernelDeleteThread(w->thread);
    if (w->start >= 0) sceKernelDeleteSema(w->start);
    if (w->done >= 0) sceKernelDeleteSema(w->done);
    w->thread = w->start = w->done = -1;
    return false;
}

static void rr_worker_dispatch(rr_vita_worker *w, void (*run)(void *), void *arg) {
    if (w->pending) abort();
    if (w->thread < 0) { run(arg); return; }
    w->run = run; w->arg = arg; w->pending = true;
#ifdef __vita__
    w->dispatched_us = sceKernelGetProcessTimeWide();
#endif
    atomic_store_explicit(&w->complete, false, memory_order_relaxed);
    atomic_store_explicit(&w->ready, true, memory_order_release);
    if (sceKernelSignalSema(w->start, 1) < 0) abort();
}

static void rr_worker_join(rr_vita_worker *w) {
    if (!w->pending) return;
    if (sceKernelWaitSema(w->done, 1, NULL) < 0 ||
        !atomic_load_explicit(&w->complete, memory_order_acquire)) abort();
    w->pending = false;
}

static void rr_worker_close(rr_vita_worker *w) {
    rr_worker_join(w);
    if (w->thread < 0) return;
    atomic_store_explicit(&w->ready, false, memory_order_release);
    if (sceKernelSignalSema(w->start, 1) < 0) abort();
    sceKernelWaitThreadEnd(w->thread, NULL, NULL);
    sceKernelDeleteThread(w->thread);
    sceKernelDeleteSema(w->start); sceKernelDeleteSema(w->done);
    w->thread = w->start = w->done = -1;
}
#endif
