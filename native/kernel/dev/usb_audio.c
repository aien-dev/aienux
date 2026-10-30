/* usb_audio.c -- audio.output pure logic (see usb_audio.h). Freestanding. */
#include "usb_audio.h"

#define D_CONFIG 2u
#define D_INTERFACE 4u
#define D_ENDPOINT 5u
#define D_CS_INTERFACE 0x24u

static uint32_t le24(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16); }

static int usable(const ck_uac_stream *s)
{
    if (s->channels < 2 || s->subframe_bytes < 2 || s->bit_resolution < 16) return 0;
    if (s->rate_count == 0) return 1;
    for (unsigned i = 0; i < s->rate_count; i++)
        if (s->sample_rates[i] == 48000u) return 1;
    return 0;
}

/* Lexicographic preference key: lower is better. */
static int better(const ck_uac_stream *a, const ck_uac_stream *b)
{
    int ac = a->channels != 2, bc = b->channels != 2;
    if (ac != bc) return ac < bc;
    int ab = a->bit_resolution != 16, bb = b->bit_resolution != 16;
    if (ab != bb) return ab < bb;
    return a->max_packet < b->max_packet;
}

int ck_uac_select_output(const uint8_t *b, size_t len, ck_uac_stream *out)
{
    if (!b || !out || len < 9 || b[0] < 9 || b[1] != D_CONFIG) return -1;
    size_t total = (size_t)b[2] | ((size_t)b[3] << 8);
    if (total > len) total = len;

    uint8_t version = CK_UAC_NONE;
    int have_control = 0, have_iface = 0, have_fmt = 0;
    uint8_t control = 0, if_num = 0, if_alt = 0, if_as = 0;
    uint8_t f_ch = 0, f_sub = 0, f_bits = 0, f_count = 0;
    uint32_t f_rates[CK_UAC_MAX_RATES] = {0};
    uint16_t clock = CK_UAC_NO_CLOCK;
    ck_uac_stream best;
    int have_best = 0;
    unsigned n = 0;

    size_t at = 0;
    while (at + 2 <= total) {
        size_t l = b[at];
        if (l < 2 || at + l > total) return -1;
        const uint8_t *d = b + at;
        if (d[1] == D_INTERFACE && l >= 9) {
            have_iface = 1;
            if_num = d[2];
            if_alt = d[3];
            if_as = d[5] == 1 && d[6] == 2;
            have_fmt = 0;
            if (d[5] == 1 && d[6] == 1) {
                have_control = 1;
                control = d[2];
            }
        } else if (d[1] == D_CS_INTERFACE && l >= 3) {
            int in_as = have_iface && if_as;
            int in_other = have_iface && !if_as;
            if (d[2] == 1 && in_as) {
                /* UAC2 AS_GENERAL: bNrChannels at offset 10. UAC1 has none
                 * there (offset 4 is bDelay); its channels come from
                 * FORMAT_TYPE_I. */
                if (version == CK_UAC2 && l >= 11) {
                    if (!have_fmt) {
                        f_sub = 0;
                        f_bits = 0;
                        f_count = 0;
                        for (unsigned i = 0; i < CK_UAC_MAX_RATES; i++) f_rates[i] = 0;
                        have_fmt = 1;
                    }
                    f_ch = d[10];
                }
            } else if (d[2] == 1 && l >= 8 && in_other) {
                uint16_t bcd = (uint16_t)(d[3] | (d[4] << 8));
                version = bcd >= 0x0200 ? CK_UAC2 : CK_UAC1;
                if (version == CK_UAC2) clock = d[7];
            } else if (d[2] == 2 && in_as && l >= (version == CK_UAC2 ? 6u : 8u)) {
                /* FORMAT_TYPE_I. UAC1: bNrChannels[4] bSubframeSize[5]
                 * bBitResolution[6] bSamFreqType[7]. UAC2: bSubslotSize[4]
                 * bBitResolution[5]. */
                uint8_t ch, sub, bits;
                if (version == CK_UAC2) {
                    ch = have_fmt ? f_ch : 0;
                    sub = d[4];
                    bits = d[5];
                } else {
                    ch = d[4];
                    sub = d[5];
                    bits = d[6];
                }
                uint32_t rates[CK_UAC_MAX_RATES] = {0};
                uint8_t count = 0;
                if (version == CK_UAC1) {
                    size_t nr = d[7];
                    if (nr != 0 && l >= 8 + nr * 3) {
                        count = (uint8_t)(nr < CK_UAC_MAX_RATES ? nr : CK_UAC_MAX_RATES);
                        for (unsigned i = 0; i < count; i++) rates[i] = le24(d + 8 + i * 3);
                    }
                } else if (version == CK_UAC2) {
                    /* Rates belong to the UAC2 clock entity (not queried). */
                    rates[0] = 48000u;
                    count = 1;
                }
                have_fmt = 1;
                f_ch = ch;
                f_sub = sub;
                f_bits = bits;
                f_count = count;
                for (unsigned i = 0; i < CK_UAC_MAX_RATES; i++) f_rates[i] = rates[i];
            } else if (d[2] == 0x0a && l >= 4 && version == CK_UAC2) {
                clock = d[3]; /* CLOCK_SOURCE bClockID */
            }
        } else if (d[1] == D_ENDPOINT && l >= 7) {
            if (have_iface && if_as && have_fmt && version != CK_UAC_NONE && have_control) {
                if (if_alt != 0 && (d[2] & 0x80u) == 0 && (d[3] & 3u) == 1u && n < CK_UAC_MAX_CANDIDATES) {
                    ck_uac_stream s;
                    s.version = version;
                    s.control_interface = control;
                    s.interface = if_num;
                    s.alternate = if_alt;
                    s.endpoint_address = d[2];
                    s.max_packet = (uint16_t)((d[4] | (d[5] << 8)) & 0x7ff);
                    s.interval = d[6];
                    s.channels = f_ch;
                    s.subframe_bytes = f_sub;
                    s.bit_resolution = f_bits;
                    s.clock_source = clock;
                    for (unsigned i = 0; i < CK_UAC_MAX_RATES; i++) s.sample_rates[i] = f_rates[i];
                    s.rate_count = f_count;
                    if (version == CK_UAC2 && f_count == 1) s.sample_rates[0] = 48000u;
                    n++;
                    if (usable(&s) && (!have_best || better(&s, &best))) {
                        best = s;
                        have_best = 1;
                    }
                }
            }
        }
        at += l;
    }
    if (!have_best) return -1;
    *out = best;
    return 0;
}

uint32_t ck_uac_interval_us(int high_speed, uint8_t interval)
{
    if (interval == 0) return 0;
    if (!high_speed) return 1000u;
    return interval <= 16 ? 125u << (interval - 1) : 0;
}

int ck_pcm_framer_init(ck_pcm_framer *f, uint32_t rate, uint8_t channels, uint8_t sample_bytes, uint32_t interval_us)
{
    if (!f) return -1;
    f->rate = rate;
    f->channels = channels;
    f->sample_bytes = sample_bytes;
    f->interval_us = interval_us;
    f->remainder = 0;
    if (!rate || !channels || !sample_bytes || !interval_us) {
        f->rate = 0; /* unusable: every packet is 0 bytes */
        return -1;
    }
    return 0;
}

size_t ck_pcm_peek_bytes(const ck_pcm_framer *f)
{
    uint64_t num = (uint64_t)f->rate * f->interval_us + f->remainder;
    return (size_t)(num / 1000000u) * f->channels * f->sample_bytes;
}

size_t ck_pcm_next_bytes(ck_pcm_framer *f)
{
    uint64_t num = (uint64_t)f->rate * f->interval_us + f->remainder;
    f->remainder = num % 1000000u;
    return (size_t)(num / 1000000u) * f->channels * f->sample_bytes;
}

size_t ck_pcm_fill_i16(ck_pcm_framer *f, const int16_t *ring, size_t ring_len, size_t *cursor, int16_t *out,
                       size_t out_len)
{
    size_t samples = ck_pcm_next_bytes(f) / 2;
    if (samples > out_len) samples = out_len;
    if (!ring || ring_len == 0) {
        for (size_t i = 0; i < samples; i++) out[i] = 0;
        return samples;
    }
    for (size_t i = 0; i < samples; i++) {
        out[i] = ring[*cursor % ring_len];
        *cursor = (*cursor + 1) % ring_len;
    }
    return samples;
}

static ck_uac_trb isoch_trb(uint64_t buf, uint32_t length, uint16_t frame_id, int ioc)
{
    ck_uac_trb t;
    t.d[0] = (uint32_t)buf;
    t.d[1] = (uint32_t)(buf >> 32);
    t.d[2] = length & 0x1ffffu;
    /* One packet per TD (no CHAIN), TBC (bits 8:7) zero, SIA clear. */
    t.d[3] = ((uint32_t)(frame_id & 0x7ffu) << 20) | (CK_UAC_TRB_ISOCH << 10) | (ioc ? CK_UAC_TRB_IOC : 0u);
    return t;
}

size_t ck_uac_schedule(ck_pcm_framer *f, uint16_t controller_frame, uint16_t lead_frames, const ck_uac_packet *packets,
                       size_t npackets, const int16_t *ring, size_t ring_len, size_t *cursor, int16_t *pcm,
                       size_t pcm_len, size_t stride, ck_uac_trb *trbs, size_t ntrbs)
{
    if (!f || !packets || !cursor || !pcm || !trbs || stride == 0) return 0;
    size_t slots = npackets < ntrbs ? npackets : ntrbs;
    if (pcm_len / stride < slots) slots = pcm_len / stride;
    size_t count = 0;
    while (count < slots) {
        /* Check before consuming PCM: never truncate a packet. */
        size_t need = ck_pcm_peek_bytes(f);
        if (need > packets[count].capacity || need > stride * 2) break;
        size_t samples = ck_pcm_fill_i16(f, ring, ring_len, cursor, pcm + count * stride, stride);
        uint16_t frame = (uint16_t)((controller_frame + lead_frames + count) & 0x7ffu);
        trbs[count] = isoch_trb(packets[count].dma, (uint32_t)(samples * 2), frame, 0);
        count++;
    }
    if (count > 0) trbs[count - 1].d[3] |= CK_UAC_TRB_IOC;
    return count;
}

int ck_test_tone_init(ck_test_tone *t, uint32_t frequency_hz, uint32_t sample_rate)
{
    if (!t || sample_rate == 0 || frequency_hz >= sample_rate / 2) return -1;
    t->phase = 0;
    t->step = (uint32_t)(((uint64_t)frequency_hz << 32) / sample_rate);
    return 0;
}

int16_t ck_test_tone_next(ck_test_tone *t)
{
    t->phase += t->step;
    int32_t x = (int32_t)(t->phase >> 16);
    int32_t tri = x < 32768 ? x : 65535 - x;
    int64_t y = (4 * (int64_t)tri * (int64_t)(32767 - tri) * 2) / 32767 - 32767;
    if (y > 32767) y = 32767;
    if (y < -32767) y = -32767;
    return (int16_t)y;
}
