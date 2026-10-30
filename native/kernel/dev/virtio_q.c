/* virtio_q.c -- polled virtio 1.x request/response core (virtio_q.h). */
#include "virtio_q.h"
#include <string.h>

#define VQ_RESET_POLLS 1000000u
#define VQ_DESC_F_NEXT 1u
#define VQ_DESC_F_WRITE 2u
#define VQ_AVAIL_F_NO_INTERRUPT 1u
#define VQ_ALIGN_UP(x, a) (((x) + (a)-1) & ~(size_t)((a)-1))

/* The chain behind each descriptor id, kept on the driver side (never read
 * back from the shared ring). */
static uint16_t chain_next[VQ_MAX_QUEUES][VQ_SIZE];
static uint8_t chain_len[VQ_MAX_QUEUES][VQ_SIZE];
static uint32_t chain_wlen[VQ_MAX_QUEUES][VQ_SIZE]; /* bytes the device may write into the chain */

const char *vq_err_name(int e)
{
    switch (e) {
    case VQ_OK: return "ok";
    case VQ_E_ARG: return "bad argument";
    case VQ_E_MEM: return "ring memory";
    case VQ_E_RESET: return "reset timeout";
    case VQ_E_NO_VERSION_1: return "no VERSION_1";
    case VQ_E_FEATURES: return "features refused";
    case VQ_E_QUEUES: return "queues";
    case VQ_E_FULL: return "queue full";
    case VQ_E_DEVICE: return "device broken";
    case VQ_E_TIMEOUT: return "timeout";
    case VQ_E_NO_ACCESS_PLATFORM: return "no ACCESS_PLATFORM";
    }
    return "?";
}

static uint32_t rd(vq_dev *d, vnet_window w, uint32_t off, unsigned width) { return d->ops.read(d->ops.ctx, w, off, width); }
static void wr(vq_dev *d, vnet_window w, uint32_t off, unsigned width, uint32_t v) { d->ops.write(d->ops.ctx, w, off, width, v); }
static void wr64(vq_dev *d, uint32_t lo, uint64_t v)
{
    wr(d, VNET_WIN_COMMON, lo, 4, (uint32_t)v);
    wr(d, VNET_WIN_COMMON, lo + 4, 4, (uint32_t)(v >> 32));
}

/* One queue: descriptors, available ring, used ring, each 4 KiB aligned. */
static size_t queue_bytes(void) { return 3u * 4096u; }
size_t vq_mem_bytes(unsigned nq) { return (size_t)nq * queue_bytes(); }

static int fail(vq_dev *d, int e)
{
    wr(d, VNET_WIN_COMMON, VNET_CC_STATUS, 1, VNET_S_FAILED);
    d->broken = 1;
    return e;
}

int vq_init(vq_dev *d, const vnet_ops *ops, const virtio_pci_caps *caps, uint8_t *mem, uint64_t dma, size_t len,
            unsigned nq, unsigned flags)
{
    if (!d)
        return VQ_E_ARG;
    memset(d, 0, sizeof *d);
    d->broken = 1;
    if (!ops || !caps || !mem || nq == 0 || nq > VQ_MAX_QUEUES || (flags & ~VQ_INIT_REQUIRE_ACCESS_PLATFORM))
        return VQ_E_ARG;
    if (!caps->common_cfg.present || caps->common_cfg.length < VNET_CC_LEN || !caps->notify_cfg.present ||
        !caps->has_notify_off_multiplier)
        return VQ_E_ARG;
    if (len < vq_mem_bytes(nq) || ((uintptr_t)mem & 4095u) || (dma & 4095u))
        return VQ_E_MEM;
    d->ops = *ops, d->mem = mem, d->dma = dma, d->len = len, d->nq = nq;
    d->notify_mult = caps->notify_off_multiplier;
    memset(mem, 0, vq_mem_bytes(nq));

    /* 3.1.1: reset, ACKNOWLEDGE, DRIVER, features, FEATURES_OK */
    wr(d, VNET_WIN_COMMON, VNET_CC_STATUS, 1, 0);
    for (uint32_t polls = 0; rd(d, VNET_WIN_COMMON, VNET_CC_STATUS, 1) != 0;)
        if (++polls >= VQ_RESET_POLLS)
            return VQ_E_RESET;
    wr(d, VNET_WIN_COMMON, VNET_CC_STATUS, 1, VNET_S_ACK);
    wr(d, VNET_WIN_COMMON, VNET_CC_STATUS, 1, VNET_S_ACK | VNET_S_DRIVER);
    wr(d, VNET_WIN_COMMON, VNET_CC_DFSELECT, 4, 0);
    uint64_t offered = rd(d, VNET_WIN_COMMON, VNET_CC_DF, 4);
    wr(d, VNET_WIN_COMMON, VNET_CC_DFSELECT, 4, 1);
    offered |= (uint64_t)rd(d, VNET_WIN_COMMON, VNET_CC_DF, 4) << 32;
    if (!(offered >> VNET_F_VERSION_1 & 1))
        return fail(d, VQ_E_NO_VERSION_1);
    if ((flags & VQ_INIT_REQUIRE_ACCESS_PLATFORM) && !(offered >> VNET_F_ACCESS_PLATFORM & 1))
        return fail(d, VQ_E_NO_ACCESS_PLATFORM);
    uint64_t accept = (1ull << VNET_F_VERSION_1) | (offered & (1ull << VNET_F_ACCESS_PLATFORM));
    wr(d, VNET_WIN_COMMON, VNET_CC_GFSELECT, 4, 0);
    wr(d, VNET_WIN_COMMON, VNET_CC_GF, 4, (uint32_t)accept);
    wr(d, VNET_WIN_COMMON, VNET_CC_GFSELECT, 4, 1);
    wr(d, VNET_WIN_COMMON, VNET_CC_GF, 4, (uint32_t)(accept >> 32));
    wr(d, VNET_WIN_COMMON, VNET_CC_STATUS, 1, VNET_S_ACK | VNET_S_DRIVER | VNET_S_FEATURES_OK);
    if (!(rd(d, VNET_WIN_COMMON, VNET_CC_STATUS, 1) & VNET_S_FEATURES_OK))
        return fail(d, VQ_E_FEATURES);
    d->features = accept;

    wr(d, VNET_WIN_COMMON, VNET_CC_MSIX, 2, 0xffffu);
    if (rd(d, VNET_WIN_COMMON, VNET_CC_NUMQ, 2) < nq)
        return fail(d, VQ_E_QUEUES);
    for (unsigned i = 0; i < nq; i++) {
        vq_queue *q = &d->q[i];
        size_t base = (size_t)i * queue_bytes();
        q->desc_off = base, q->avail_off = base + 4096u, q->used_off = base + 8192u;
        wr(d, VNET_WIN_COMMON, VNET_CC_Q_SELECT, 2, i);
        uint32_t max = rd(d, VNET_WIN_COMMON, VNET_CC_Q_SIZE, 2);
        if (max == 0 || rd(d, VNET_WIN_COMMON, VNET_CC_Q_ENABLE, 2) != 0)
            return fail(d, VQ_E_QUEUES);
        uint32_t n = VQ_SIZE;
        while (n > max)
            n >>= 1;
        if (n < 2)
            return fail(d, VQ_E_QUEUES);
        q->size = (uint16_t)n;
        wr(d, VNET_WIN_COMMON, VNET_CC_Q_SIZE, 2, n);
        wr(d, VNET_WIN_COMMON, VNET_CC_Q_MSIX, 2, 0xffffu);
        uint64_t noff = (uint64_t)rd(d, VNET_WIN_COMMON, VNET_CC_Q_NOFF, 2) * d->notify_mult;
        if (noff + 2 > caps->notify_cfg.length)
            return fail(d, VQ_E_QUEUES);
        q->notify_off = (uint32_t)noff;
        wr64(d, VNET_CC_Q_DESCLO, dma + q->desc_off);
        wr64(d, VNET_CC_Q_AVAILLO, dma + q->avail_off);
        wr64(d, VNET_CC_Q_USEDLO, dma + q->used_off);
        uint16_t f = VQ_AVAIL_F_NO_INTERRUPT;
        memcpy(mem + q->avail_off, &f, 2);
        wr(d, VNET_WIN_COMMON, VNET_CC_Q_ENABLE, 2, 1);
    }
    d->broken = 0;
    return VQ_OK;
}

void vq_ready(vq_dev *d)
{
    wr(d, VNET_WIN_COMMON, VNET_CC_STATUS, 1, VNET_S_ACK | VNET_S_DRIVER | VNET_S_FEATURES_OK | VNET_S_DRIVER_OK);
    uint32_t s = rd(d, VNET_WIN_COMMON, VNET_CC_STATUS, 1);
    if ((s & (VNET_S_NEEDS_RESET | VNET_S_FAILED)) || !(s & VNET_S_DRIVER_OK))
        d->broken = 1;
}

uint32_t vq_cfg32(vq_dev *d, uint32_t off)
{
    for (unsigned t = 0; t < 16; t++) {
        uint32_t g = rd(d, VNET_WIN_COMMON, VNET_CC_CFGGEN, 1);
        uint32_t v = rd(d, VNET_WIN_DEVICE, off, 4);
        if (rd(d, VNET_WIN_COMMON, VNET_CC_CFGGEN, 1) == g)
            return v;
    }
    return 0;
}

int vq_post(vq_dev *d, unsigned qi, const vq_buf *bufs, unsigned n, uint16_t *head)
{
    if (d->broken)
        return VQ_E_DEVICE;
    if (qi >= d->nq || !bufs || n == 0 || n > VQ_SIZE)
        return VQ_E_ARG;
    vq_queue *q = &d->q[qi];
    /* free descriptor ids: not part of any outstanding chain */
    uint8_t used[VQ_SIZE] = {0};
    for (uint16_t h = 0; h < q->size; h++)
        if (q->posted[h])
            for (uint16_t c = h, k = 0; k < chain_len[qi][h]; k++, c = chain_next[qi][c])
                used[c] = 1;
    uint16_t ids[VQ_SIZE];
    unsigned got = 0;
    for (uint16_t i = 0; i < q->size && got < n; i++)
        if (!used[i])
            ids[got++] = i;
    if (got < n)
        return VQ_E_FULL;
    uint32_t wlen = 0;
    for (unsigned k = 0; k < n; k++) {
        uint8_t *e = d->mem + q->desc_off + 16u * ids[k];
        uint16_t fl = (uint16_t)((bufs[k].device_writes ? VQ_DESC_F_WRITE : 0) | (k + 1 < n ? VQ_DESC_F_NEXT : 0));
        uint16_t nx = k + 1 < n ? ids[k + 1] : 0;
        memcpy(e, &bufs[k].addr, 8);
        memcpy(e + 8, &bufs[k].len, 4);
        memcpy(e + 12, &fl, 2);
        memcpy(e + 14, &nx, 2);
        chain_next[qi][ids[k]] = nx;
        if (bufs[k].device_writes)
            wlen += bufs[k].len;
    }
    uint16_t h = ids[0];
    chain_len[qi][h] = (uint8_t)n;
    chain_wlen[qi][h] = wlen;
    q->posted[h] = 1;
    q->in_flight++;
    uint8_t *a = d->mem + q->avail_off;
    memcpy(a + 4 + 2u * (q->avail_idx & (q->size - 1)), &h, 2);
    __atomic_thread_fence(__ATOMIC_RELEASE); /* ring entry before idx */
    q->avail_idx++;
    memcpy(a + 2, &q->avail_idx, 2);
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* idx visible before the notify */
    d->ops.notify(d->ops.ctx, q->notify_off, (uint16_t)qi);
    if (head)
        *head = h;
    return VQ_OK;
}

int vq_reap(vq_dev *d, unsigned qi, uint16_t *head, uint32_t *written)
{
    if (d->broken)
        return VQ_E_DEVICE;
    if (qi >= d->nq)
        return VQ_E_ARG;
    vq_queue *q = &d->q[qi];
    uint8_t *u = d->mem + q->used_off;
    uint16_t idx = __atomic_load_n((uint16_t *)(void *)(u + 2), __ATOMIC_ACQUIRE);
    uint16_t pending = (uint16_t)(idx - q->last_used);
    if (pending == 0)
        return VQ_E_FULL;
    if (pending > q->in_flight) {
        d->broken = 1;
        return VQ_E_DEVICE;
    }
    const volatile uint8_t *e = u + 4 + 8u * (q->last_used & (q->size - 1));
    uint32_t id = 0, len = 0;
    for (int i = 0; i < 4; i++) {
        id |= (uint32_t)e[i] << (8 * i);
        len |= (uint32_t)e[4 + i] << (8 * i);
    }
    if (id >= q->size || !q->posted[id] || len > chain_wlen[qi][id]) {
        d->broken = 1;
        return VQ_E_DEVICE;
    }
    q->posted[id] = 0;
    q->in_flight--;
    q->last_used++;
    if (head)
        *head = (uint16_t)id;
    if (written)
        *written = len;
    return VQ_OK;
}

int vq_call(vq_dev *d, unsigned q, const vq_buf *bufs, unsigned n, uint32_t spins, uint32_t *written)
{
    uint16_t h, got;
    int e = vq_post(d, q, bufs, n, &h);
    if (e)
        return e;
    for (uint32_t i = 0; i < spins; i++) {
        e = vq_reap(d, q, &got, written);
        if (e == VQ_E_FULL)
            continue;
        if (e)
            return e;
        if (got == h)
            return VQ_OK;
        /* another chain finished first (not expected for a lone caller): keep waiting for ours */
    }
    return VQ_E_TIMEOUT;
}

void vq_reset(vq_dev *d)
{
    wr(d, VNET_WIN_COMMON, VNET_CC_STATUS, 1, 0);
    d->broken = 1;
}
