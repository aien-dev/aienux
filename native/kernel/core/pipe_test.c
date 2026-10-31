/* pipe_test.c -- AIENOS_CK_PIPE kernel self-test (TEST ONLY, CK_PIPE_TEST=1; gate scripts/qemu_ck_pipe_test.sh).
 *
 * Drives the pipe primitive (core/pipe.c, aienos-mv#428) inside the kernel: a producer and a consumer task, step
 * functions interleaved by the run-queue scheduler (core/sched.c), each holding one end in its own capability table,
 * through the same driver the host test uses (core/pipe_drive.h). The kernel's wait hook yields to the scheduler
 * (it keeps ticking it, running the other task) until the wake hook fires; this is cooperative scheduling inside one
 * kernel thread, not a context switch between M3 threads. Lines on the serial console:
 *     pipe_check: name=<check> result=PASS|FAIL <details>; <label>
 *     pipe_test: done checks=<n> failed=<n> capacity=<bytes> pool_pipes=<n>; <label>
 * QEMU (aarch64 virt), TEST build, not physical: qualifies nothing physical. */
#ifndef CK_PIPE_TEST
#error "pipe_test is test-only: build with make CK_PIPE_TEST=1"
#endif
#ifdef CK_HARDWARE_STAGING
#error "CK_PIPE_TEST cannot be combined with CK_HARDWARE_STAGING"
#endif
#include "ck_internal.h"
#include "pipe.h"
#include "pipe_drive.h"

#define LABEL "QEMU (aarch64 virt), TEST build, not physical"
#define PAYLOAD (160u * 1024u) /* 2.5 pipe capacities */

static struct ck_pipe_pool pool;
static struct ck_cap_table ta, tb;
static struct pd_world world;
static uint8_t sink[PAYLOAD];
static unsigned checks, failed;

static void fresh(void)
{
    ck_cap_init(&ta, 0x41, CK_CAP_SLOTS);
    ck_cap_init(&tb, 0x42, CK_CAP_SLOTS);
}

static int empty(void)
{
    return pd_table_empty(&ta) && pd_table_empty(&tb) && ck_pipe_pool_empty(&pool);
}

static void verdict(const char *name, int ok)
{
    checks++;
    failed += !ok;
    ck_printf("pipe_check: name=%s result=%s tables_empty=%d; %s\n", name, ok ? "PASS" : "FAIL", empty(), LABEL);
}

static size_t expected_nuls(size_t n)
{
    size_t z = 0;
    for (size_t i = 0; i < n; i++)
        z += pd_byte(i) == 0;
    return z;
}

static void transfer(const char *name, unsigned flags)
{
    fresh();
    pd_run(&world, &pool, &ta, &tb, sink, PAYLOAD, 8192, 1000, flags, 1, 4, 0);
    const struct ck_pipe_stats *s = &pool.total;
    size_t bad = pd_mismatches(&world), nul = pd_nul_count(&world);
    ck_printf("pipe_detail: %s bytes=%lu sent=%lu nul=%lu expected_nul=%lu mismatches=%lu would_block=%llu waits=%llu "
              "wakeups=%llu max_resident=%llu capacity=%u eof=%d prod_steps=%llu cons_steps=%llu ticks=%llu\n",
              name, (unsigned long)world.recvd, (unsigned long)world.sent, (unsigned long)nul,
              (unsigned long)expected_nuls(PAYLOAD), (unsigned long)bad, (unsigned long long)s->would_block,
              (unsigned long long)s->waits, (unsigned long long)s->wakeups, (unsigned long long)s->max_resident,
              CK_PIPE_CAPACITY, world.eof_seen, (unsigned long long)world.prod_steps,
              (unsigned long long)world.cons_steps, (unsigned long long)world.ticks);
    int moved = world.setup_err == OSH_E_OK && world.prod_err == OSH_E_OK && world.cons_err == OSH_E_OK &&
                world.recvd == PAYLOAD && world.sent == PAYLOAD && bad == 0 && nul == expected_nuls(PAYLOAD) &&
                nul > 0 && s->bytes_read == PAYLOAD;
    if (!flags) {
        verdict("payload_nul_byte_for_byte", moved);
        verdict("backpressure_would_block", moved && s->would_block > 0 && s->max_resident == CK_PIPE_CAPACITY &&
                                                world.prod_steps > 1 && world.cons_steps > 1);
        verdict("eof_after_last_writer", moved && world.eof_seen && s->eof == 1);
        verdict("bounded_memory", s->max_resident <= CK_PIPE_CAPACITY && empty());
    } else {
        verdict("blocking_wait_wake_through_scheduler", moved && s->waits > 0 && s->wakeups > 0 &&
                                                            s->cancellations == 0 && world.eof_seen && empty());
    }
}

static void broken_pipe(void)
{
    fresh();
    ck_pipe_pool_init(&pool, 0);
    struct ck_handle r, w;
    size_t n = 1;
    static const uint8_t b[3] = { 'a', 0, 'b' };
    int ok = ck_pipe_create(&pool, &ta, &r, &w) == OSH_E_OK;
    ok = ok && ck_stream_close(&pool, &ta, r) == OSH_E_OK;
    unsigned e = ck_stream_write(&pool, &ta, w, b, 3, 0, &n);
    ok = ok && e == OSH_E_BROKEN_PIPE && n == 0 && pool.total.broken_pipe == 1;
    ok = ok && ck_stream_close(&pool, &ta, w) == OSH_E_OK;
    ck_printf("pipe_detail: broken_pipe write_after_reader_close=%s n=%lu\n", osh_err_label(e), (unsigned long)n);
    verdict("broken_pipe_no_reader", ok && empty());
}

static void revoked_write(void)
{
    fresh();
    ck_pipe_pool_init(&pool, 0);
    struct ck_handle r, w;
    size_t n = 0, m = 9;
    uint8_t got[4];
    static const uint8_t b1[2] = { 'o', 'k' }, b2[2] = { 0, 'x' };
    int ok = ck_pipe_create(&pool, &ta, &r, &w) == OSH_E_OK;
    ok = ok && ck_stream_write(&pool, &ta, w, b1, 2, 0, &n) == OSH_E_OK && n == 2;
    ok = ok && ck_pipe_revoke(&pool, &ta, w) == OSH_E_OK;
    unsigned ew = ck_stream_write(&pool, &ta, w, b2, 2, 0, &n);
    unsigned er = ck_stream_read(&pool, &ta, r, got, 4, 0, &m);
    ok = ok && ew == OSH_E_REVOKED && n == 0 && er == OSH_E_REVOKED && m == 0 && pool.total.bytes_read == 0;
    ok = ok && ck_stream_close(&pool, &ta, r) == OSH_E_OK && ck_stream_close(&pool, &ta, w) == OSH_E_OK;
    ck_printf("pipe_detail: revoked write=%s n=%lu read=%s n=%lu bytes_delivered=%llu\n", osh_err_label(ew),
              (unsigned long)n, osh_err_label(er), (unsigned long)m, (unsigned long long)pool.total.bytes_read);
    verdict("revoked_write_refused_no_bytes_after", ok && empty());

    /* revoked mid-stream by the consumer through the scheduler: nothing is read after the revoke */
    fresh();
    pd_run(&world, &pool, &ta, &tb, sink, PAYLOAD, 8192, 1000, CK_PIPE_F_BLOCK, 1, 4, 50000);
    ck_printf("pipe_detail: revoke_midstream recvd=%lu reads_after_revoke=%lu prod=%s cons=%s bytes_read=%llu\n",
              (unsigned long)world.recvd, (unsigned long)world.reads_after_revoke, osh_err_label(world.prod_err),
              osh_err_label(world.cons_err), (unsigned long long)pool.total.bytes_read);
    verdict("revoke_midstream_no_data_after", world.revoked && world.reads_after_revoke == 0 &&
                                                  world.prod_err == OSH_E_REVOKED && world.cons_err == OSH_E_REVOKED &&
                                                  pool.total.bytes_read == world.recvd && pd_mismatches(&world) == 0 &&
                                                  empty());
}

static void stale_and_ops(void)
{
    fresh();
    ck_pipe_pool_init(&pool, 0);
    struct ck_handle r, w, r2, w2;
    size_t n = 0;
    uint8_t b[2] = { 0, 0 };
    int ok = ck_pipe_create(&pool, &ta, &r, &w) == OSH_E_OK;
    ok = ok && ck_stream_close(&pool, &ta, r) == OSH_E_OK && ck_stream_close(&pool, &ta, w) == OSH_E_OK;
    ok = ok && ck_pipe_create(&pool, &ta, &r2, &w2) == OSH_E_OK && r2.index == r.index;
    unsigned es = ck_stream_read(&pool, &ta, r, b, 2, 0, &n);
    ok = ok && es == OSH_E_STALE && ck_pipe_release_table(&pool, &ta) == 2;
    verdict("stale_handle_after_close", ok && empty());

    struct ck_pipe_ret ret;
    uint64_t unknown[4] = { CK_PIPE_FRAME_MAGIC, CK_PIPE_ABI_VERSION, 9, 0 };
    uint64_t v2[4] = { CK_PIPE_FRAME_MAGIC, 2, CK_PIPE_OP_CREATE, 0 };
    /* No slice. A length of 0 does not consult the window. */
    unsigned eu = ck_pipe_dispatch(&pool, &ta, &tb, unknown, 4, 0, 0, 0, 0, &ret);
    unsigned ev = ck_pipe_dispatch(&pool, &ta, &tb, v2, 4, 0, 0, 0, 0, &ret);
    ck_printf("pipe_detail: op_table abi_version=%u unknown_op=%s version_2=%s\n", CK_PIPE_ABI_VERSION,
              osh_err_label(eu), osh_err_label(ev));
    verdict("unknown_op_not_supported", eu == OSH_E_NOT_SUPPORTED && ev == OSH_E_NOT_SUPPORTED && empty());

    ok = ck_pipe_create(&pool, &ta, &r, &w) == OSH_E_OK && ck_pipe_cancel(&pool, &ta, w) == OSH_E_OK;
    unsigned ei = ck_stream_write(&pool, &ta, w, b, 2, 0, &n);
    ok = ok && ei == OSH_E_INTERRUPTED && n == 0 && ck_stream_write(&pool, &ta, w, b, 2, 0, &n) == OSH_E_OK && n == 2;
    ok = ok && ck_pipe_release_table(&pool, &ta) == 2;
    verdict("cancel_interrupts_once", ok && empty());
}

void ck_pipe_selftest(void)
{
    ck_printf("pipe_test: TEST-ONLY pipe self-test begin abi_version=%u capacity=%u pool_pipes=%u payload=%u; %s\n",
              CK_PIPE_ABI_VERSION, CK_PIPE_CAPACITY, CK_PIPE_POOL_PIPES, PAYLOAD, LABEL);
    transfer("nonblocking", 0);
    transfer("blocking", CK_PIPE_F_BLOCK);
    broken_pipe();
    revoked_write();
    stale_and_ops();
    ck_printf("pipe_test: done checks=%u failed=%u capacity=%u pool_pipes=%u; %s\n", checks, failed, CK_PIPE_CAPACITY,
              CK_PIPE_POOL_PIPES, LABEL);
}
