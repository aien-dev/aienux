/* usb_audio_test.c -- host tests for the audio.output pure logic
 * (dev/usb_audio.c, issue #85): the Rust reference vectors and regressions of
 * crates/aienos-kernel/src/usb/audio.rs, plus hostile descriptors. Hosted
 * test code only. Run by make usb-audio-test (and stage-test). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usb_audio.h"

static int checks, failures;
#define CHECK(c)                                                                 \
    do {                                                                         \
        checks++;                                                                \
        if (!(c)) {                                                              \
            failures++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                \
        }                                                                        \
    } while (0)

/* Real UAC1 shape: AC, AS alt zero, stereo Type I PCM 16-bit/48 kHz, OUT. */
static const uint8_t UAC1[] = {
    9, 2, 73, 0, 2, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 1, 1, 0, 0, 9, 0x24, 1, 0, 1, 0x26, 0, 1, 1,
    9, 4, 1, 0, 0, 1, 2, 0, 0, 9, 4, 1, 1, 1, 1, 2, 0, 0, 7, 0x24, 1, 1, 2, 3, 0, 11, 0x24, 2,
    1, 2, 2, 16, 1, 0x80, 0xbb, 0, 9, 5, 1, 1, 192, 0, 1, 0, 0,
};
/* UAC2 clock source and Type I 16-bit stereo format, one 48 kHz clock. */
static const uint8_t UAC2[] = {
    9, 2, 85, 0, 2, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 1, 1, 0, 0, 9, 0x24, 1, 0, 2, 9, 0, 1, 1, 8,
    0x24, 0x0a, 3, 7, 0, 0, 0, 9, 4, 1, 0, 0, 1, 2, 0, 0, 9, 4, 1, 1, 1, 1, 2, 0, 0, 16, 0x24,
    1, 1, 0, 1, 1, 0, 0, 0, 2, 3, 0, 0, 0, 0, 6, 0x24, 2, 1, 2, 16, 9, 5, 1, 1, 192, 0, 1, 0,
    0,
};

static size_t find(const uint8_t *v, size_t n, const uint8_t *pat, size_t pn)
{
    for (size_t i = 0; i + pn <= n; i++)
        if (!memcmp(v + i, pat, pn)) return i;
    return (size_t)-1;
}

static void test_parse(void)
{
    ck_uac_stream s;
    CHECK(ck_uac_select_output(UAC1, sizeof UAC1, &s) == 0);
    CHECK(s.version == CK_UAC1 && s.interface == 1 && s.alternate == 1 && s.channels == 2 && s.bit_resolution == 16);
    CHECK(s.control_interface == 0 && s.endpoint_address == 1 && s.max_packet == 192 && s.interval == 1);
    CHECK(s.rate_count == 1 && s.sample_rates[0] == 48000u && s.clock_source == CK_UAC_NO_CLOCK);
    CHECK(ck_uac_select_output(UAC2, sizeof UAC2, &s) == 0);
    CHECK(s.version == CK_UAC2 && s.clock_source == 3 && s.channels == 2 && s.subframe_bytes == 2);
}

static void test_uac1_channels_from_format_type(void)
{
    /* UAC1 AS_GENERAL offset 4 is bDelay, not a channel count. */
    uint8_t v[sizeof UAC1];
    memcpy(v, UAC1, sizeof v);
    static const uint8_t asg[] = {7, 0x24, 1, 1, 2, 3, 0};
    size_t at = find(v, sizeof v, asg, sizeof asg);
    CHECK(at != (size_t)-1);
    v[at + 4] = 7;
    ck_uac_stream s;
    CHECK(ck_uac_select_output(v, sizeof v, &s) == 0 && s.channels == 2);
}

static void test_hostile(void)
{
    ck_uac_stream s;
    uint8_t v[sizeof UAC1];
    static const uint8_t ft[] = {11, 0x24, 2};
    size_t at = find(UAC1, sizeof UAC1, ft, sizeof ft);
    CHECK(at != (size_t)-1);
    /* Short UAC1 FORMAT_TYPE_I (bLength 7 < 8): no read past it, no crash. */
    memcpy(v, UAC1, sizeof v);
    v[at] = 7;
    (void)ck_uac_select_output(v, sizeof v, &s);
    /* Every truncation of the descriptor is refused, in an exact-size heap
     * buffer so ASan catches any read past it. */
    for (size_t n = 0; n < sizeof UAC1; n++) {
        uint8_t *cut = (uint8_t *)malloc(n ? n : 1);
        if (!cut) continue;
        memcpy(cut, UAC1, n);
        CHECK(ck_uac_select_output(cut, n, &s) == -1);
        free(cut);
    }
    /* Zero bLength inside the walk is malformed. */
    memcpy(v, UAC1, sizeof v);
    v[9] = 0;
    CHECK(ck_uac_select_output(v, sizeof v, &s) == -1);
    /* An element running past wTotalLength is malformed. */
    memcpy(v, UAC1, sizeof v);
    v[sizeof v - 9] = 40;
    CHECK(ck_uac_select_output(v, sizeof v, &s) == -1);
    /* Not a configuration descriptor, NULL arguments. */
    memcpy(v, UAC1, sizeof v);
    v[1] = 1;
    CHECK(ck_uac_select_output(v, sizeof v, &s) == -1);
    CHECK(ck_uac_select_output(NULL, 9, &s) == -1);
    CHECK(ck_uac_select_output(UAC1, sizeof UAC1, NULL) == -1);
    /* An IN endpoint (a capture stream) is never selected for output. */
    memcpy(v, UAC1, sizeof v);
    v[sizeof v - 7] = 0x81;
    CHECK(ck_uac_select_output(v, sizeof v, &s) == -1);
    /* A mono stream is refused. */
    memcpy(v, UAC1, sizeof v);
    v[at + 4] = 1;
    CHECK(ck_uac_select_output(v, sizeof v, &s) == -1);
    /* 44.1 kHz only (48 kHz not offered) is refused. */
    memcpy(v, UAC1, sizeof v);
    v[at + 8] = 0x44;
    v[at + 9] = 0xac;
    CHECK(ck_uac_select_output(v, sizeof v, &s) == -1);
}

static void test_interval(void)
{
    CHECK(ck_uac_interval_us(0, 1) == 1000u && ck_uac_interval_us(0, 4) == 1000u);
    CHECK(ck_uac_interval_us(1, 1) == 125u && ck_uac_interval_us(1, 4) == 1000u);
    CHECK(ck_uac_interval_us(1, 16) == (125u << 15) && ck_uac_interval_us(1, 17) == 0);
    CHECK(ck_uac_interval_us(0, 0) == 0 && ck_uac_interval_us(1, 0) == 0);
}

static void test_framer(void)
{
    ck_pcm_framer f;
    CHECK(ck_pcm_framer_init(&f, 44100, 1, 2, 1000) == 0);
    size_t frames = 0;
    int saw44 = 0, saw45 = 0;
    for (int i = 0; i < 1000; i++) {
        size_t n = ck_pcm_next_bytes(&f) / 2;
        frames += n;
        saw44 |= n == 44;
        saw45 |= n == 45;
    }
    CHECK(frames == 44100 && saw44 && saw45);

    CHECK(ck_pcm_framer_init(&f, 44100, 2, 2, 1000) == 0);
    int16_t src[256], out[90];
    for (unsigned i = 0; i < 256; i++) src[i] = 7;
    size_t cursor = 0, total = 0;
    for (int i = 0; i < 1000; i++) total += ck_pcm_fill_i16(&f, src, 256, &cursor, out, 90) * 2;
    CHECK(total == 176400);

    CHECK(ck_pcm_framer_init(&f, 48000, 2, 2, 1000) == 0);
    const int16_t ring[5] = {1, 2, 3, 4, 5};
    int16_t o[192];
    cursor = 3;
    CHECK(ck_pcm_fill_i16(&f, ring, 5, &cursor, o, 192) == 96);
    CHECK(o[0] == 4 && o[1] == 5 && o[2] == 1 && o[3] == 2 && cursor == 4);
    /* Empty ring: silence. */
    for (unsigned i = 0; i < 192; i++) o[i] = 9;
    CHECK(ck_pcm_fill_i16(&f, NULL, 0, &cursor, o, 192) == 96 && o[0] == 0 && o[95] == 0 && o[96] == 9);

    CHECK(ck_pcm_framer_init(&f, 0, 2, 2, 1000) == -1);
    CHECK(ck_pcm_framer_init(&f, 48000, 0, 2, 1000) == -1);
    CHECK(ck_pcm_framer_init(&f, 48000, 2, 0, 1000) == -1);
    CHECK(ck_pcm_framer_init(&f, 48000, 2, 2, 0) == -1);
    CHECK(ck_pcm_next_bytes(&f) == 0);
}

static void test_schedule(void)
{
    ck_pcm_framer f;
    ck_pcm_framer_init(&f, 48000, 2, 2, 1000);
    const ck_uac_packet pk[2] = {{0x1000, 192}, {0x2000, 192}};
    int16_t pcm[192];
    ck_uac_trb t[2];
    memset(t, 0xa5, sizeof t);
    size_t cursor = 0;
    CHECK(ck_uac_schedule(&f, 2046, 2, pk, 2, NULL, 0, &cursor, pcm, 192, 96, t, 2) == 2);
    CHECK(((t[0].d[3] >> 20) & 0x7ff) == 0 && ((t[1].d[3] >> 20) & 0x7ff) == 1);
    CHECK(t[0].d[2] == 192 && t[1].d[2] == 192);
    CHECK(t[0].d[0] == 0x1000 && t[1].d[0] == 0x2000 && t[0].d[1] == 0);
    CHECK(((t[0].d[3] >> 10) & 0x3f) == CK_UAC_TRB_ISOCH);
    CHECK((t[0].d[3] & CK_UAC_TRB_IOC) == 0 && (t[1].d[3] & CK_UAC_TRB_IOC) != 0);
    /* No SIA, no cycle (the ring writer sets it), no CHAIN, TBC zero. */
    CHECK((t[0].d[3] & (CK_UAC_TRB_SIA | 1u | 0x10u | (7u << 7))) == 0);

    /* 96 kHz stereo needs 384 B per 1 ms packet: it does not fit 192 B, so
     * nothing is scheduled and no sample is consumed. */
    ck_pcm_framer_init(&f, 96000, 2, 2, 1000);
    int16_t ring[512];
    for (unsigned i = 0; i < 512; i++) ring[i] = 7;
    int16_t pcm2[256];
    ck_uac_trb t1[1];
    const ck_uac_packet small[1] = {{0x1000, 192}};
    cursor = 0;
    CHECK(ck_uac_schedule(&f, 0, 1, small, 1, ring, 512, &cursor, pcm2, 256, 256, t1, 1) == 0);
    CHECK(cursor == 0);
    const ck_uac_packet big[1] = {{0x1000, 1024}};
    CHECK(ck_uac_schedule(&f, 0, 1, big, 1, ring, 512, &cursor, pcm2, 256, 0, t1, 1) == 0);
    /* Fits now: one packet, IOC set. */
    CHECK(ck_uac_schedule(&f, 0, 1, big, 1, ring, 512, &cursor, pcm2, 256, 256, t1, 1) == 1);
    CHECK(t1[0].d[2] == 384 && cursor == 192 && (t1[0].d[3] & CK_UAC_TRB_IOC));
}

static void test_tone(void)
{
    ck_test_tone t;
    CHECK(ck_test_tone_init(&t, 440, 48000) == 0);
    int lo = 0, hi = 0, ok = 1;
    for (int i = 0; i < 48000; i++) {
        int16_t x = ck_test_tone_next(&t);
        ok &= x >= -32767 && x <= 32767;
        lo |= x < -30000;
        hi |= x > 30000;
    }
    CHECK(ok && lo && hi);
    CHECK(ck_test_tone_init(&t, 24000, 48000) == -1);
    CHECK(ck_test_tone_init(&t, 440, 0) == -1);
}

int main(void)
{
    test_parse();
    test_uac1_channels_from_format_type();
    test_hostile();
    test_interval();
    test_framer();
    test_schedule();
    test_tone();
    printf("CK_USB_AUDIO_TEST: %s checks=%d failures=%d\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
