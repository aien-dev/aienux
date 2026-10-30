/* usb_audio.h -- audio.output (issue #85, ADR 0012 rule 15), C kernel slice 1:
 * the pure logic of USB Audio Class output. No MMIO, no DMA, no controller
 * access; host tested in dev/tests/usb_audio_test.c (make usb-audio-test).
 *
 * Port of the Rust reference crates/aienos-kernel/src/usb/audio.rs (#96,
 * #102), which the C kernel (ADR 0024) does not link:
 *   - ck_uac_select_output: walk one configuration descriptor (UAC1 or UAC2)
 *     and pick the best isochronous OUT stream for 48 kHz 16-bit stereo;
 *   - ck_uac_interval_us: endpoint service interval from bInterval;
 *   - ck_pcm_framer: bytes per service interval with the fractional frame
 *     carried, so 44.1 kHz at 1 ms totals exactly 44100 frames per second;
 *   - ck_uac_schedule: one isochronous TD (one Isoch TRB) per packet, Frame
 *     IDs wrapping at the 11-bit field, IOC on the last scheduled packet only;
 *   - ck_test_tone: deterministic integer sine-like tone (no floating point).
 *
 * Behaviour matches the Rust reference, including its known limit: UAC2
 * sample rates belong to the clock entity and are not queried here, so a UAC2
 * stream is taken to offer 48 kHz. Every descriptor length is bounds checked
 * and the walk never reads past min(len, wTotalLength).
 *
 * Not in this slice (later #85 cuts): the xHCI isochronous endpoint inside
 * the DMA fence, the QEMU usb-audio gate, and any speaker policy. Output
 * only: nothing here records audio or touches a microphone.
 * Freestanding. QEMU is not hardware: nothing here is physically qualified. */
#ifndef AIENOS_CK_USB_AUDIO_H
#define AIENOS_CK_USB_AUDIO_H
#include <stddef.h>
#include <stdint.h>

enum ck_uac_version { CK_UAC_NONE = 0, CK_UAC1 = 1, CK_UAC2 = 2 };

#define CK_UAC_MAX_RATES 8u
#define CK_UAC_MAX_CANDIDATES 16u
#define CK_UAC_NO_CLOCK 0xffffu

typedef struct {
    uint8_t version;           /* enum ck_uac_version */
    uint8_t control_interface; /* AudioControl bInterfaceNumber */
    uint8_t interface;         /* AudioStreaming bInterfaceNumber */
    uint8_t alternate;         /* bAlternateSetting carrying the endpoint */
    uint8_t endpoint_address;  /* isochronous OUT bEndpointAddress */
    uint16_t max_packet;       /* wMaxPacketSize bits 10:0 */
    uint8_t interval;          /* bInterval */
    uint8_t channels;
    uint8_t subframe_bytes;
    uint8_t bit_resolution;
    uint16_t clock_source;     /* UAC2 clock entity id, or CK_UAC_NO_CLOCK */
    uint32_t sample_rates[CK_UAC_MAX_RATES]; /* rate_count 0 = continuous */
    uint8_t rate_count;
} ck_uac_stream;

/* 0 and *out filled, or -1: no usable stream (at least 2 channels, 2-byte
 * subframes, 16-bit resolution, 48 kHz offered or continuous) or a malformed
 * descriptor. Preference: exactly 2 channels, then exactly 16 bits, then the
 * smallest max_packet; ties keep the first stream found. */
int ck_uac_select_output(const uint8_t *cfg, size_t len, ck_uac_stream *out);

/* Service interval in microseconds: full speed 1000, high speed
 * 125 << (bInterval - 1) for bInterval 1..16. 0 when bInterval is 0 or out
 * of range. */
uint32_t ck_uac_interval_us(int high_speed, uint8_t interval);

typedef struct {
    uint32_t rate;
    uint8_t channels;
    uint8_t sample_bytes;
    uint32_t interval_us;
    uint64_t remainder;
} ck_pcm_framer;
/* 0, or -1 when any argument is zero (the framer is then unusable). */
int ck_pcm_framer_init(ck_pcm_framer *f, uint32_t rate, uint8_t channels, uint8_t sample_bytes, uint32_t interval_us);
/* Bytes of the next packet, advancing the fractional carry. */
size_t ck_pcm_next_bytes(ck_pcm_framer *f);
/* Bytes the next packet will need, without advancing. */
size_t ck_pcm_peek_bytes(const ck_pcm_framer *f);
/* Advance one packet and copy its samples (at most out_len) from the
 * interleaved ring, wrapping *cursor. An empty ring writes silence. Returns
 * the samples written. */
size_t ck_pcm_fill_i16(ck_pcm_framer *f, const int16_t *ring, size_t ring_len, size_t *cursor, int16_t *out,
                       size_t out_len);

/* One isochronous packet buffer: DMA address and capacity in bytes. */
typedef struct {
    uint64_t dma;
    uint16_t capacity;
} ck_uac_packet;
/* Isoch TRB dwords (xHCI 1.2 section 6.4.1.3); the cycle bit is left 0 for
 * the ring writer to set. */
typedef struct {
    uint32_t d[4];
} ck_uac_trb;
#define CK_UAC_TRB_ISOCH 5u
#define CK_UAC_TRB_IOC (1u << 5)
#define CK_UAC_TRB_SIA (1u << 31)
/* Prepare up to min(npackets, ntrbs, pcm_len / stride) packets, Frame ID
 * (controller_frame + lead_frames + i) & 0x7ff. Stops, without consuming any
 * PCM, at the first packet that would not fit its buffer capacity or its
 * stride-sample PCM chunk (never truncates). stride 0 schedules nothing.
 * Returns the number of TRBs written; the last one carries IOC. */
size_t ck_uac_schedule(ck_pcm_framer *f, uint16_t controller_frame, uint16_t lead_frames, const ck_uac_packet *packets,
                       size_t npackets, const int16_t *ring, size_t ring_len, size_t *cursor, int16_t *pcm,
                       size_t pcm_len, size_t stride, ck_uac_trb *trbs, size_t ntrbs);

typedef struct {
    uint32_t phase;
    uint32_t step;
} ck_test_tone;
/* 0, or -1 when sample_rate is 0 or frequency_hz is not below Nyquist. */
int ck_test_tone_init(ck_test_tone *t, uint32_t frequency_hz, uint32_t sample_rate);
/* Next sample, always within [-32767, 32767]. */
int16_t ck_test_tone_next(ck_test_tone *t);
#endif
