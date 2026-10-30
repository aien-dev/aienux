/* pipe_drive.h -- TEST driver for the pipe primitive (pipe.h): a producer and a consumer task, each a step function,
 * interleaved by the run-queue scheduler (sched.h), exchanging a binary payload (every fifth byte NUL) through one pipe.
 * Shared by the host test (tests/test_pipe.c) and the QEMU self-test (core/pipe_test.c, CK_PIPE_TEST) so both run
 * the same scenario. No libc, no allocation: the caller owns the world, the pool, both tables and the sink buffer.
 *
 * The producer owns the write end; the read end is handed to the consumer's own table with STREAM_DUP (READ, plus
 * REVOKE for the revocation scenario) and the producer's copy is closed, so each task holds one end. Non-blocking mode:
 * a WOULD_BLOCK ends the step with no progress and the scheduler runs the other task. Blocking mode: the wait hook
 * marks the caller blocked and keeps ticking the scheduler, running the other task's steps, until the wake hook fires
 * for that end (OK, the call retries) or the spin bound runs out (INTERRUPTED: a stalled peer). A wake posted
 * before wait runs is kept: woken[] is not cleared on entry. Static functions only. One kernel thread. */
#ifndef AIENOS_CK_PIPE_DRIVE_H
#define AIENOS_CK_PIPE_DRIVE_H

#include "pipe.h"
#include "sched.h"

#define PD_CHUNK_MAX 8192u
#define PD_SPIN_MAX 64u
#define PD_TICK_MAX 2000000u

struct pd_world;
struct pd_task {
    uint32_t id;
    int done, blocked;
    void (*step)(struct pd_world *w, struct pd_task *t);
};

struct pd_world {
    struct ck_pipe_pool *pool;
    struct ck_cap_table *prod_t, *cons_t;
    struct ck_pipe_hooks hooks;
    struct ck_sched sched;
    struct pd_task tasks[2]; /* 0 producer, 1 consumer */
    struct pd_task *current;
    struct ck_handle w, r;
    uint8_t woken[CK_PIPE_POOL_PIPES][2];
    unsigned flags;            /* CK_PIPE_F_BLOCK or 0 */
    size_t total;              /* payload bytes to send */
    size_t chunk_w, chunk_r;   /* bytes per producer write / consumer read */
    unsigned prod_every, cons_every; /* a task moves bytes on one step in this many (a slow side), 0 or 1 = every step */
    size_t revoke_at;          /* consumer revokes the pipe once it has this many bytes (0 = never) */
    uint8_t *sink;             /* total bytes */
    uint8_t wbuf[PD_CHUNK_MAX], rbuf[PD_CHUNK_MAX];
    /* results */
    size_t sent, recvd, reads_after_revoke;
    unsigned setup_err, prod_err, cons_err; /* OSH_E_OK, or the error the task ended on */
    int eof_seen, revoked;
    uint64_t ticks, wait_spins, prod_steps, cons_steps;
};

static inline uint8_t pd_byte(size_t i)
{
    return i % 5 == 0 ? 0 : (uint8_t)(((uint32_t)i * 2654435761u) >> 13);
}

static inline struct pd_task *pd_find(struct pd_world *w, uint32_t id)
{
    return w->tasks[0].id == id ? &w->tasks[0] : w->tasks[1].id == id ? &w->tasks[1] : 0;
}

/* One scheduler tick: run the picked task's step unless it is blocked (parked on the stack) or done. */
static inline int pd_tick(struct pd_world *w)
{
    uint32_t id = 0;
    if (ck_sched_tick(&w->sched, 0, &id) != 1)
        return 0;
    w->ticks++;
    struct pd_task *t = pd_find(w, id);
    if (!t || t->blocked || t->done)
        return 1;
    struct pd_task *prev = w->current;
    w->current = t;
    t->step(w, t);
    w->current = prev;
    if (t->done)
        ck_sched_remove(&w->sched, t->id);
    return 1;
}

static inline unsigned pd_wait(void *ctx, unsigned pipe, unsigned end)
{
    struct pd_world *w = ctx;
    struct pd_task *me = w->current;
    if (!me)
        return OSH_E_INTERRUPTED;
    me->blocked = 1;
    /* Do not clear woken[] here. The pipe publishes the waiter, drops its lock, then calls wait. A peer can
     * wake in that window. Clearing first would drop that wake and the task would sleep with data already queued. */
    for (unsigned s = 0; s < PD_SPIN_MAX; s++) {
        if (__atomic_load_n(&w->woken[pipe][end], __ATOMIC_ACQUIRE))
            break;
        w->wait_spins++;
        if (!pd_tick(w))
            break;
    }
    me->blocked = 0;
    unsigned got = __atomic_exchange_n(&w->woken[pipe][end], 0, __ATOMIC_ACQ_REL);
    return got ? OSH_E_OK : OSH_E_INTERRUPTED;
}

static inline void pd_wake(void *ctx, unsigned pipe, unsigned end)
{
    struct pd_world *w = ctx;
    __atomic_store_n(&w->woken[pipe][end], 1, __ATOMIC_RELEASE);
}

static inline void pd_producer(struct pd_world *w, struct pd_task *t)
{
    w->prod_steps++;
    if (w->prod_every > 1 && w->prod_steps % w->prod_every)
        return; /* thinking: no I/O this step */
    if (w->sent == w->total) {
        ck_stream_close(w->pool, w->prod_t, w->w); /* last writer: the consumer sees EOF once drained */
        t->done = 1;
        return;
    }
    size_t want = w->total - w->sent;
    if (want > w->chunk_w)
        want = w->chunk_w;
    for (size_t i = 0; i < want; i++)
        w->wbuf[i] = pd_byte(w->sent + i);
    size_t n = 0;
    unsigned e = ck_stream_write(w->pool, w->prod_t, w->w, w->wbuf, want, w->flags, &n);
    if (e == OSH_E_OK) {
        w->sent += n;
    } else if (e != OSH_E_WOULD_BLOCK) {
        w->prod_err = e;
        ck_stream_close(w->pool, w->prod_t, w->w);
        t->done = 1;
    }
}

static inline void pd_consumer(struct pd_world *w, struct pd_task *t)
{
    w->cons_steps++;
    if (w->cons_every > 1 && w->cons_steps % w->cons_every)
        return;
    size_t n = 0;
    unsigned e = ck_stream_read(w->pool, w->cons_t, w->r, w->rbuf, w->chunk_r, w->flags, &n);
    if (e == OSH_E_OK && n > 0) {
        if (w->revoked)
            w->reads_after_revoke++;
        for (size_t i = 0; i < n && w->recvd + i < w->total; i++)
            w->sink[w->recvd + i] = w->rbuf[i];
        w->recvd += n;
        if (w->revoke_at && !w->revoked && w->recvd >= w->revoke_at) {
            w->revoked = ck_pipe_revoke(w->pool, w->cons_t, w->r) == OSH_E_OK;
            if (!w->revoked)
                w->cons_err = OSH_E_IO;
        }
    } else if (e == OSH_E_OK) {
        w->eof_seen = 1;
        ck_stream_close(w->pool, w->cons_t, w->r);
        t->done = 1;
    } else if (e != OSH_E_WOULD_BLOCK) {
        w->cons_err = e;
        ck_stream_close(w->pool, w->cons_t, w->r);
        t->done = 1;
    }
}

/* Run one transfer. pool and both tables must be fresh (the pool initialised with &w->hooks by this call). */
static inline void pd_run(struct pd_world *w, struct ck_pipe_pool *pool, struct ck_cap_table *prod_t,
                   struct ck_cap_table *cons_t, uint8_t *sink, size_t total, size_t chunk_w, size_t chunk_r,
                   unsigned flags, unsigned prod_every, unsigned cons_every, size_t revoke_at)
{
    *w = (struct pd_world){ .pool = pool, .prod_t = prod_t, .cons_t = cons_t, .sink = sink, .total = total,
                            .chunk_w = chunk_w > PD_CHUNK_MAX ? PD_CHUNK_MAX : chunk_w,
                            .chunk_r = chunk_r > PD_CHUNK_MAX ? PD_CHUNK_MAX : chunk_r, .flags = flags,
                            .prod_every = prod_every, .cons_every = cons_every, .revoke_at = revoke_at };
    w->hooks = (struct ck_pipe_hooks){ w, pd_wait, pd_wake, 0 };
    ck_pipe_pool_init(pool, &w->hooks);
    struct ck_handle r0;
    w->setup_err = ck_pipe_create(pool, prod_t, &r0, &w->w);
    if (w->setup_err != OSH_E_OK)
        return;
    w->setup_err = ck_stream_dup(pool, prod_t, r0, cons_t, CK_R_READ | (revoke_at ? CK_R_REVOKE : 0), &w->r);
    unsigned c = ck_stream_close(pool, prod_t, r0); /* the producer keeps only the write end */
    if (w->setup_err == OSH_E_OK)
        w->setup_err = c;
    if (w->setup_err != OSH_E_OK) {
        ck_pipe_release_table(pool, prod_t);
        ck_pipe_release_table(pool, cons_t);
        return;
    }
    static const uint8_t classes[1] = { 0 };
    ck_sched_init(&w->sched, 1, classes, CK_SCHED_MAX_TASKS, 1);
    w->tasks[0] = (struct pd_task){ .id = 0x50524f44u, .step = pd_producer }; /* "PROD" */
    w->tasks[1] = (struct pd_task){ .id = 0x434f4e53u, .step = pd_consumer }; /* "CONS" */
    ck_sched_enqueue(&w->sched, w->tasks[0].id, CK_PRIO_NORMAL);
    ck_sched_enqueue(&w->sched, w->tasks[1].id, CK_PRIO_NORMAL);
    while ((!w->tasks[0].done || !w->tasks[1].done) && w->ticks < PD_TICK_MAX)
        if (!pd_tick(w))
            break;
    /* section 12: whatever happened, no end stays open */
    ck_pipe_release_table(pool, prod_t);
    ck_pipe_release_table(pool, cons_t);
}

/* Bytes of the sink that differ from the payload, over what was received. */
static inline size_t pd_mismatches(const struct pd_world *w)
{
    size_t bad = 0, n = w->recvd < w->total ? w->recvd : w->total;
    for (size_t i = 0; i < n; i++)
        bad += w->sink[i] != pd_byte(i);
    return bad;
}

static inline size_t pd_nul_count(const struct pd_world *w)
{
    size_t z = 0, n = w->recvd < w->total ? w->recvd : w->total;
    for (size_t i = 0; i < n; i++)
        z += w->sink[i] == 0;
    return z;
}

/* 1 if the table holds no live slot at all (tombstones included). */
static inline int pd_table_empty(const struct ck_cap_table *t)
{
    for (unsigned i = 0; i < CK_CAP_SLOTS; i++)
        if (t->slots[i].live)
            return 0;
    return 1;
}

#endif
