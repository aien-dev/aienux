/* pipe.h -- bounded byte-stream pipes, a native AIENOS execution primitive (aienos-mv#428, AIENOS-NATIVE-PIPE-1).
 *
 * Contract: aien-protocols-mv specs/osh-platform/OSH_PLATFORM_ABI.md read at commit fb5fca98e667 (v1 DRAFT, unfrozen):
 * section 8.1 (PIPE_CREATE, STREAM_READ / WRITE / CLOSE / DUP; bounded pipes with backpressure, WOULD_BLOCK or block,
 * BROKEN_PIPE when the reader closed), 8.5 (64 KiB per pipe minimum, LIMIT instead of truncation), 8.6 (error codes,
 * shared with osh_op.h through osh_err.h), 4.4 (epoch: an id from another epoch fails STALE) and 9.2 (authority is
 * checked at the moment of the effect). Anything below marked UNVERIFIED is this kernel's own choice where the draft
 * names a thing but does not pin it.
 *
 * This is a byte pipe, separate from the 56-byte typed message ring in ipc.h. It reuses only the capability tables of
 * ipc.h: every stream end is a capability in the caller's ck_cap_table, and every operation looks the handle up with
 * ck_cap_lookup (live slot, matching generation, required right) when the effect happens, not before.
 *
 *   read end    capability with CK_R_READ   (never CK_R_WRITE)
 *   write end   capability with CK_R_WRITE  (never CK_R_READ)
 *   optional    CK_R_DERIVE (dup into the same table), CK_R_GRANT (dup into another table), CK_R_REVOKE (revoke the pipe)
 *
 * Lifetimes. A pipe lives while at least one end capability is open. reader_refs / writer_refs count the open end
 * capabilities (create gives one of each, every dup adds one, every close removes one). Last writer closed: once the
 * buffer is drained a read returns OK with 0 bytes (EOF). No reader: a write returns BROKEN_PIPE. Both counts zero: the
 * pipe slot is scrubbed and freed. Capabilities are independent slots (no derive parent link), so a closed end frees
 * its slot at once and a table can always be emptied; ipc.h cascading revoke is therefore not used for pipes.
 * Every pipe capability must be released through ck_stream_close or ck_pipe_release_table: removing one with
 * ck_cap_remove directly would leave its count held.
 *
 * Epoch. Each pool slot has an epoch, incremented every time the slot is (re)created. The capability resource carries
 * it, so a capability that outlived its pipe never reaches the next pipe in that slot: STALE. A handle whose capability
 * slot was closed (and maybe reused with a new generation) also fails STALE, by the ck_cap generation check.
 *
 * Revocation and cancellation. ck_pipe_revoke (CK_R_REVOKE on either end) kills the whole pipe: the buffered bytes are
 * scrubbed, both ends are woken, and every later read, write, dup or cancel on any end returns REVOKED; no byte is
 * delivered after a revoke. Only close still works, to release the handles. ck_pipe_cancel (the data right of that
 * end) interrupts the end: the next or currently blocked read (or write) on that end returns INTERRUPTED with no
 * bytes moved, once; a later call runs normally (section 10: an interrupted wait is retried without re-running an
 * effect, and an interrupted call here never moved a byte).
 *
 * Blocking. With CK_PIPE_F_BLOCK a read on an empty pipe or a write on a full one parks the caller through the
 * pool's wait hook instead of returning WOULD_BLOCK, and retries (re-checking authority) after the hook returns OK.
 * A blocking call returns as soon as it moved at least one byte (partial I/O; the caller loops). The kernel's hook
 * yields to the scheduler; the host test simulates it. Without a wait hook a blocking call returns WOULD_BLOCK.
 * A blocking write copies the bytes it may commit into a kernel buffer before it parks (at most CK_PIPE_WRITE_SNAP
 * of them) and the retry reads that buffer, not the caller memory. A write that does not park copies once, under
 * the pool lock, straight into the ring.
 *
 * Locking. One ticket lock covers the pool (every pipe, the ref counts, the ring, the stats, the live count).
 * core/smp.c is the kernel's SMP primitive and it synchronises with __atomic_*; there is no separate spinlock
 * type, so the ticket lock uses those same orders. create, read, write, close, dup, revoke, cancel, release and
 * ck_pipe_pool_empty all take it. It is not IRQ-safe: pipe ops are not called from interrupt handlers. If a wake
 * or a cancel is ever called from an IRQ, this lock must mask IRQs, or that CPU deadlocks against itself. It is
 * never held across wait or wake, so a hook may call back into the pipe. The waiter increments waiting[end] under
 * the lock before dropping it; a waker calls the wake hook after dropping the lock, and only when that count was
 * non-zero. The wake hook must be sticky (woken[] stays set until wait has observed it). before_wait, if set,
 * runs in that window; production leaves it NULL. The capability table is covered only by this pool lock. Another
 * core using the same ck_cap_table for a non-pipe operation would race.
 * ck_pipe_pool_init is single-threaded, before the pool is shared.
 *
 * User window. ck_pipe_dispatch rejects a read or a write whose slice lies outside [user_lo, user_hi) with
 * INVALID_ARG, before anything reads or writes the slice. A length of 0 passes. Otherwise the base must sit in
 * [user_lo, user_hi) and the length must be <= user_hi - base. The check never adds base and length, so a length
 * that would wrap the address space fails. ck_stream_read and ck_stream_write do not repeat it; a direct caller
 * passes a kernel buffer it already owns.
 * The syscall layer still has two duties. It must assert, on each architecture, that user_hi is below the user-VA
 * top: below TTBR1 (0xFFFF000000000000) on ARM64, and below the canonical upper half on x86. It must copy the
 * slice in and out through a kernel bounce buffer outside the pool lock, so a user fault never runs while the
 * ticket lock is held.
 *
 * Memory: a fixed pool of CK_PIPE_POOL_PIPES pipes with CK_PIPE_CAPACITY bytes each, all static; nothing grows.
 * Pure logic, no hardware: host-tested in tests/test_pipe.c (fixtures in tests/fixtures/pipe/, plus host threads
 * for concurrent read, write and close), driven in QEMU by core/pipe_test.c (CK_PIPE_TEST). The QEMU driver is
 * one kernel thread. Host and QEMU evidence qualify nothing physical. */
#ifndef AIENOS_CK_PIPE_H
#define AIENOS_CK_PIPE_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"
#include "osh_err.h"

#define CK_PIPE_ABI_VERSION 1u          /* section 3: abi_version of the platform frame */
#define CK_PIPE_FRAME_MAGIC 0x4F534846u /* section 3: "OSHF", platform frame magic */
#define CK_PIPE_CAPACITY 65536u         /* bytes per pipe; section 8.5 minimum 64 KiB (LIMITS_GET max_pipe_bytes) */
#define CK_PIPE_POOL_PIPES 4u           /* pipes per pool; UNVERIFIED: the draft sets no pipe count */
_Static_assert(CK_PIPE_CAPACITY >= 65536u, "section 8.5: at least 64 KiB per pipe");
_Static_assert(CK_PIPE_POOL_PIPES >= 1u && CK_PIPE_POOL_PIPES <= 8u, "the pipe index has three bits in the resource");

/* Capability resource of a stream end: tag 0x50 ("P") in bits 31..24, end in bit 23, pipe index in bits 22..20,
 * epoch in bits 19..0. A slot whose epoch reached CK_PIPE_EPOCH_MAX is retired for good. Layout UNVERIFIED (own). */
#define CK_PIPE_RES_TAG 0x50u
#define CK_PIPE_EPOCH_MAX 0xFFFFFu
static inline uint32_t ck_pipe_resource(unsigned idx, unsigned end, uint32_t epoch)
{
    return (CK_PIPE_RES_TAG << 24) | ((uint32_t)(end & 1u) << 23) | ((uint32_t)(idx & 7u) << 20) |
           (epoch & CK_PIPE_EPOCH_MAX);
}

enum ck_pipe_end { CK_PIPE_END_READ = 0, CK_PIPE_END_WRITE = 1 };

/* Platform op ids of the versioned op table. The draft names the operations; the numbers are UNVERIFIED (own). */
enum ck_pipe_op {
    CK_PIPE_OP_CREATE = 1, /* PIPE_CREATE() -> (read_stream, write_stream) */
    CK_PIPE_OP_READ = 2,   /* STREAM_READ(stream, out_slice) -> n, 0 = EOF */
    CK_PIPE_OP_WRITE = 3,  /* STREAM_WRITE(stream, in_slice) -> n, partial writes allowed */
    CK_PIPE_OP_CLOSE = 4,  /* STREAM_CLOSE(stream) */
    CK_PIPE_OP_DUP = 5,    /* STREAM_DUP(src_stream, rights, target table) -> stream; capability form, UNVERIFIED */
};

#define CK_PIPE_F_BLOCK 0x1u /* read / write flag: park through the wait hook instead of WOULD_BLOCK */
/* Bytes a blocking write snapshots before it parks. Kept under 4 KiB so a nested park still fits a 16 KiB
 * secondary-core stack (core/smp.c). A longer blocking write returns a partial count and the caller loops. */
#define CK_PIPE_WRITE_SNAP 1024u
_Static_assert(CK_PIPE_WRITE_SNAP >= 1u && CK_PIPE_WRITE_SNAP <= 4096u, "snapshot must fit a secondary stack");

/* Rights a create gives each end beyond its data right, and the only rights a dup may carry. */
#define CK_PIPE_CTL_RIGHTS (CK_R_DERIVE | CK_R_GRANT | CK_R_REVOKE)

struct ck_pipe_stats {
    uint64_t bytes_written, bytes_read; /* bytes that moved */
    uint64_t writes, reads;             /* calls that moved at least one byte */
    uint64_t max_resident;              /* most bytes ever buffered at once (never above CK_PIPE_CAPACITY) */
    uint64_t would_block;               /* WOULD_BLOCK results seen, returned or waited on (backpressure) */
    uint64_t waits;                     /* calls into the wait hook */
    uint64_t wakeups;                   /* calls into the wake hook (an end had a waiter) */
    uint64_t cancellations;             /* INTERRUPTED results */
    uint64_t revocations;               /* pipes revoked */
    uint64_t broken_pipe;               /* BROKEN_PIPE results */
    uint64_t eof;                       /* reads that returned EOF */
    uint64_t refused;                   /* DENIED, STALE and REVOKED results at the effect boundary (pool total only) */
};

#define CK_PIPE_LIVE 0x1u
#define CK_PIPE_REVOKED 0x2u
#define CK_PIPE_CANCEL_READ 0x4u
#define CK_PIPE_CANCEL_WRITE 0x8u

struct ck_pipe {
    uint8_t buf[CK_PIPE_CAPACITY]; /* ring: bytes head..head+len-1 (mod capacity) are buffered */
    uint32_t head, len;
    uint32_t epoch; /* 0 = never created */
    uint32_t reader_refs, writer_refs;
    uint32_t flags;
    uint32_t waiting[2]; /* callers parked in the wait hook, per end; only touched under the pool lock */
    struct ck_pipe_stats stats; /* this incarnation; reset at create */
};

/* Wait / wake hook. wait: the caller would block on (pipe, end); return OSH_E_OK to retry the operation (it re-checks
 * authority) or an error (normally OSH_E_INTERRUPTED) to give up; the operation then returns it with no byte moved.
 * Called without the pool lock. The wake flag must be sticky. A wake may already have run after the waiter was
 * published and before wait is called; wait must observe it and must not clear it before that observation
 * (pd_wait leaves woken[] set until it has seen it). A hook that clears the flag on entry loses that wake.
 * wake: (pipe, end) may proceed (bytes, space, close, revoke or cancel); called only when that end has a waiter,
 * and only after the pool lock is dropped. before_wait: optional, same window, NULL in production. */
struct ck_pipe_hooks {
    void *ctx;
    unsigned (*wait)(void *ctx, unsigned pipe, unsigned end);
    void (*wake)(void *ctx, unsigned pipe, unsigned end);
    void (*before_wait)(void *ctx);
};

struct ck_pipe_pool {
    struct ck_pipe pipes[CK_PIPE_POOL_PIPES];
    const struct ck_pipe_hooks *hooks; /* may be 0 */
    struct ck_pipe_stats total;        /* every incarnation since init */
    unsigned live;                     /* pipes currently live */
    /* Ticket lock. ticket is the next number handed out; serving is the number now inside.
     * Both 0 is unlocked. Only __atomic_* touches them. */
    uint32_t lock_ticket, lock_serving;
};

void ck_pipe_pool_init(struct ck_pipe_pool *p, const struct ck_pipe_hooks *hooks);
/* 1 if no pipe is live (every end of every pipe was closed). Takes the pool lock. */
int ck_pipe_pool_empty(struct ck_pipe_pool *p);

/* PIPE_CREATE: a new pipe, both ends inserted into t. LIMIT if the pool or the table is full (nothing is left
 * allocated). The draft's PIPE_CREATE takes no binding, so no prior capability is required (UNVERIFIED). */
unsigned ck_pipe_create(struct ck_pipe_pool *p, struct ck_cap_table *t, struct ck_handle *rd, struct ck_handle *wr);
/* STREAM_READ / STREAM_WRITE: *n = bytes moved (0 with OK on a read = EOF, or max == 0). */
unsigned ck_stream_read(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h, uint8_t *buf,
                        size_t max, unsigned flags, size_t *n);
unsigned ck_stream_write(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h, const uint8_t *buf,
                         size_t len, unsigned flags, size_t *n);
/* STREAM_CLOSE: releases the capability. Works on a revoked pipe. STALE (after releasing the capability) if it named
 * an earlier incarnation of the slot. */
unsigned ck_stream_close(struct ck_pipe_pool *p, struct ck_cap_table *t, struct ck_handle h);
/* STREAM_DUP: a new capability on the same end in `to` with `rights`, which must hold the end's data right, be a
 * subset of the source rights and of data right | CK_PIPE_CTL_RIGHTS. Needs CK_R_DERIVE (to == from) or
 * CK_R_GRANT (another table) on the source. */
unsigned ck_stream_dup(struct ck_pipe_pool *p, struct ck_cap_table *from, struct ck_handle h, struct ck_cap_table *to,
                       unsigned rights, struct ck_handle *out);
/* Revoke the whole pipe (CK_R_REVOKE on either end). */
unsigned ck_pipe_revoke(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h);
/* Interrupt the end h names (its data right needed). */
unsigned ck_pipe_cancel(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h);
/* Section 12 cleanup: closes every pipe capability in t. Returns how many STREAM_CLOSE calls returned OK.
 * A failure is not counted. STALE (the capability was released, but it named a dead incarnation) is a failure. */
unsigned ck_pipe_release_table(struct ck_pipe_pool *p, struct ck_cap_table *t);

/* ---- versioned op table ----
 * A platform call frame is cells (u64): 0 magic CK_PIPE_FRAME_MAGIC, 1 abi_version, 2 op, 3 reserved (0), then the
 * op's arguments; the frame must be exactly 4 + nargs cells. Arguments: READ / WRITE: handle (ck_handle_raw form),
 * flags; CLOSE: handle; DUP: handle, rights, target (0 caller table, 1 peer table); CREATE: none. The byte slice is
 * the read destination or the write source. Check order, first failure wins, all before any effect: frame shorter
 * than 4 cells, bad magic or nonzero reserved -> INVALID_ARG; abi_version not CK_PIPE_ABI_VERSION or an unknown op
 * -> NOT_SUPPORTED; wrong cell count, a generation-0 handle or unknown flag bits -> INVALID_ARG.
 * Result: {error, value} as in section 8; PIPE_CREATE also fills value2 (the write end). value2 is UNVERIFIED.
 * READ and WRITE then require slice[0, slice_len) inside [user_lo, user_hi), user_hi exclusive. A length of 0
 * passes. Any other slice is INVALID_ARG before the buffer is touched (see the user-window note above).
 * user_lo and user_hi are the caller's window; kernel self-tests pass the bounds of their own buffer. */
struct ck_pipe_ret {
    uint32_t error;
    uint64_t value, value2;
};
unsigned ck_pipe_dispatch(struct ck_pipe_pool *p, struct ck_cap_table *caller, struct ck_cap_table *peer,
                          const uint64_t *frame, size_t ncells, uint8_t *slice, size_t slice_len,
                          uintptr_t user_lo, uintptr_t user_hi, struct ck_pipe_ret *ret);

#endif
