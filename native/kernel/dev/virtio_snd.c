/* virtio_snd.c -- virtio-sound PCM output for the chirp (virtio_snd.h). */
#include "virtio_snd.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t get64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* struct virtio_snd_query_info: hdr.code, start_id, count, size */
size_t vsnd_cmd_query(uint8_t *out, size_t cap, uint32_t code, uint32_t start, uint32_t count, uint32_t size)
{
    if (!out || cap < 16)
        return 0;
    put32(out, code), put32(out + 4, start), put32(out + 8, count), put32(out + 12, size);
    return 16;
}

/* struct virtio_snd_pcm_set_params: hdr.code, hdr.stream_id, buffer_bytes,
 * period_bytes, features, channels, format, rate, padding */
size_t vsnd_cmd_set_params(uint8_t *out, size_t cap, uint32_t stream, uint32_t buffer_bytes, uint32_t period_bytes,
                           uint8_t channels, uint8_t format, uint8_t rate)
{
    if (!out || cap < 24)
        return 0;
    memset(out, 0, 24);
    put32(out, VSND_R_PCM_SET_PARAMS), put32(out + 4, stream), put32(out + 8, buffer_bytes);
    put32(out + 12, period_bytes);
    out[20] = channels, out[21] = format, out[22] = rate;
    return 24;
}

/* struct virtio_snd_pcm_hdr: hdr.code, stream_id */
size_t vsnd_cmd_pcm(uint8_t *out, size_t cap, uint32_t code, uint32_t stream)
{
    if (!out || cap < 8)
        return 0;
    put32(out, code), put32(out + 4, stream);
    return 8;
}

/* reply: struct virtio_snd_hdr (status), then n struct virtio_snd_pcm_info:
 * hda_fn_nid, features, formats (64), rates (64), direction, channels_min,
 * channels_max, padding[5] */
int vsnd_pick_output(const uint8_t *resp, size_t len, uint32_t n)
{
    if (len < 4 || get32(resp) != VSND_S_OK)
        return -1;
    for (uint32_t i = 0; i < n && 4u + (i + 1) * VSND_INFO_BYTES <= len; i++) {
        const uint8_t *p = resp + 4 + i * VSND_INFO_BYTES;
        if (p[24] == VSND_D_OUTPUT && (get64(p + 8) >> VSND_FMT_S16 & 1) && (get64(p + 16) >> VSND_RATE_48000 & 1) &&
            p[25] <= 2 && p[26] >= 2)
            return (int)i;
    }
    return -1;
}

#ifndef CK_HOSTED
#include "../ui/chirp.h"
#include "ck.h"

#define VSND_SPINS 200000000u
#define VSND_PERIOD_FRAMES 4800u /* 100 ms */
#define VSND_PERIOD_BYTES (VSND_PERIOD_FRAMES * 4u)
#define VSND_SLOTS 4u            /* 400 ms queued ahead: 3 descriptors each */
#define VSND_SLOT_DATA 64u       /* xfer header at 0, status at 16, samples from 64 */
#define VSND_SLOT_BYTES 20480u
#define VSND_LEAD_MS 150u        /* start this long before the chirp comes in */
#define VSND_END_FRAME (CK_CHIRP_FRAMES + VSND_PERIOD_FRAMES) /* the chirp, then 100 ms quiet */

enum { OFF, READY, PLAYING, DONE };

static struct {
    vq_dev *dev;
    int state;
    uint32_t stream;
    uint8_t *cmd, *resp, *slot;
    uint64_t cmd_bus, resp_bus, slot_bus;
    uint8_t busy[VSND_SLOTS];
    uint8_t slot_of_head[VQ_SIZE];
    uint32_t pos, start_ms, start_frame, periods, starved, bad;
} s;

static int control(size_t len, uint32_t resp_len)
{
    vq_buf b[2] = {{s.cmd_bus, (uint32_t)len, 0}, {s.resp_bus, resp_len, 1}};
    uint32_t wrote = 0;
    memset(s.resp, 0, 8);
    int e = vq_call(s.dev, VSND_VQ_CONTROL, b, 2, VSND_SPINS, &wrote);
    return e ? -1000 - e : (wrote >= 4 && get32(s.resp) == VSND_S_OK ? 0 : (int)get32(s.resp));
}

int ck_vsnd_attach(vq_dev *dev, uint8_t *mem, uint64_t bus)
{
    _Static_assert(VSND_EXTRA_BYTES == 2u * 4096u + VSND_SLOTS * VSND_SLOT_BYTES, "VSND_EXTRA_BYTES");
    s.dev = dev;
    vq_ready(s.dev);
    s.cmd = mem, s.cmd_bus = bus;
    s.resp = s.cmd + 4096u, s.resp_bus = s.cmd_bus + 4096u;
    s.slot = s.resp + 4096u, s.slot_bus = s.resp_bus + 4096u;
    /* struct virtio_snd_config: jacks, streams, chmaps */
    uint32_t streams = vq_cfg32(s.dev, 4);
    if (streams == 0 || streams > 64) {
        ck_printf("virtio_snd: no PCM streams (%u)\n", streams);
        return -2;
    }
    int rc = control(vsnd_cmd_query(s.cmd, 4096, VSND_R_PCM_INFO, 0, streams, VSND_INFO_BYTES),
                     4u + streams * VSND_INFO_BYTES);
    int pick = rc ? -1 : vsnd_pick_output(s.resp, 4u + streams * VSND_INFO_BYTES, streams);
    if (pick < 0) {
        ck_printf("virtio_snd: no stereo S16 48 kHz output among %u streams (rc=%d)\n", streams, rc);
        return -3;
    }
    s.stream = (uint32_t)pick;
    if ((rc = control(vsnd_cmd_set_params(s.cmd, 4096, s.stream, VSND_SLOTS * VSND_PERIOD_BYTES, VSND_PERIOD_BYTES, 2,
                                          VSND_FMT_S16, VSND_RATE_48000),
                      8)) ||
        (rc = control(vsnd_cmd_pcm(s.cmd, 4096, VSND_R_PCM_PREPARE, s.stream), 8))) {
        ck_printf("virtio_snd: stream %u refused (status 0x%x)\n", s.stream, (unsigned)rc);
        return -4;
    }
    s.state = READY;
    ck_printf("virtio_snd: stream %u of %u ready, stereo S16 48000 Hz, %u ms periods, %u queued\n", s.stream, streams,
              VSND_PERIOD_FRAMES / 48u, VSND_SLOTS);
    return 0;
}

/* Fills slot i with the next period from the night's position and posts it. */
static int feed(unsigned i)
{
    uint8_t *p = s.slot + i * VSND_SLOT_BYTES;
    uint64_t bus = s.slot_bus + i * VSND_SLOT_BYTES;
    put32(p, s.stream);
    memset(p + 16, 0, 8);
    ck_chirp_fill((int16_t *)(void *)(p + VSND_SLOT_DATA), s.pos, VSND_PERIOD_FRAMES);
    vq_buf b[3] = {{bus, 4, 0}, {bus + VSND_SLOT_DATA, VSND_PERIOD_BYTES, 0}, {bus + 16, 8, 1}};
    uint16_t head;
    if (vq_post(s.dev, VSND_VQ_TX, b, 3, &head))
        return -1;
    s.slot_of_head[head] = (uint8_t)i;
    s.busy[i] = 1;
    s.pos += VSND_PERIOD_FRAMES;
    s.periods++;
    return 0;
}

int ck_vsnd_step(uint32_t night_ms, uint32_t audio_frame)
{
    if (s.state == READY) {
        if (night_ms + VSND_LEAD_MS < CK_CHIRP_START_MS)
            return 1;
        s.pos = s.start_frame = audio_frame, s.start_ms = night_ms;
        for (unsigned i = 0; i < VSND_SLOTS; i++)
            if (feed(i))
                break;
        int rc = control(vsnd_cmd_pcm(s.cmd, 4096, VSND_R_PCM_START, s.stream), 8);
        if (rc) {
            ck_printf("virtio_snd: start refused (status 0x%x)\n", (unsigned)rc);
            s.state = DONE;
            return 0;
        }
        s.state = PLAYING;
        return 1;
    }
    if (s.state != PLAYING)
        return 0;
    unsigned busy = 0;
    for (unsigned i = 0; i < VSND_SLOTS; i++)
        busy += s.busy[i];
    uint16_t head;
    uint32_t wrote;
    int done_any = 0;
    while (vq_reap(s.dev, VSND_VQ_TX, &head, &wrote) == VQ_OK) {
        unsigned i = s.slot_of_head[head];
        uint8_t *p = s.slot + i * VSND_SLOT_BYTES;
        if (wrote < 4 || get32(p + 16) != VSND_S_OK)
            s.bad++;
        s.busy[i] = 0;
        done_any = 1;
        if (busy-- == 1 && s.pos < VSND_END_FRAME)
            s.starved++; /* the device ran out before this turn came */
    }
    if (s.dev->broken) {
        ck_printf("virtio_snd: device broken mid-stream\n");
        s.state = DONE;
        return 0;
    }
    if (done_any)
        for (unsigned i = 0; i < VSND_SLOTS && s.pos < VSND_END_FRAME; i++)
            if (!s.busy[i])
                feed(i);
    for (unsigned i = 0; i < VSND_SLOTS; i++)
        if (s.busy[i])
            return 1;
    if (s.pos < VSND_END_FRAME)
        return 1;
    int rc = control(vsnd_cmd_pcm(s.cmd, 4096, VSND_R_PCM_STOP, s.stream), 8);
    if (!rc)
        rc = control(vsnd_cmd_pcm(s.cmd, 4096, VSND_R_PCM_RELEASE, s.stream), 8);
    ck_printf("sound: virtio played the chirp from night %u ms (frame %u): %u periods of 100 ms, %u starved, %u "
              "refused, stop %s at night %u ms\n",
              s.start_ms, s.start_frame, s.periods, s.starved, s.bad, rc ? "refused" : "ok", night_ms);
    s.state = DONE;
    return 0;
}
#endif

#if !defined(CK_HOSTED) && !defined(CK_X86)
#include "virtio_bind.h"
static vq_bind snd_bind;
int ck_vsnd_open(const pci_system *pci)
{
    /* 0x1059 = 0x1040 + device id 25 (virtio 1.x 4.1.2.1). Queues: control, event, tx. */
    if (vq_bind_open(&snd_bind, "virtio_snd", pci, 0x1059u, 3, VSND_EXTRA_BYTES))
        return -1;
    return ck_vsnd_attach(&snd_bind.dev, snd_bind.mem + snd_bind.rings, snd_bind.phys + snd_bind.rings);
}
#endif
