/* Host tests for aienos_wpa: EAPOL-Key codec, KDF-SHA256 against an
 * independent reference, the native table's refusals, and the supplicant's
 * 4-way and group handshakes against a test authenticator that uses the
 * test doubles in wpa_toy.h (not cryptography). */
#include <stdio.h>
#include <string.h>

#include "../aienos_wpa.h"
#include "wpa_toy.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t AA[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}, SPA[6] = {0x02, 0x00, 0x00, 0x00, 0x79, 0x25};
/* RSNE: version 1, group CCMP, 1 pairwise CCMP, 1 AKM PSK-SHA256, caps MFPC. */
static const uint8_t RSNE[] = {0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                               0x01, 0x00, 0x00, 0x0f, 0xac, 0x06, 0x80, 0x00};
static const uint8_t GTK1[16] = {0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70};
static const uint8_t GTK2[16] = {0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80};

/* ---- test authenticator ---- */
typedef struct {
    wifi_sec sec;
    uint8_t pmk[32], anonce[32], snonce[32], ptk[48];
    uint64_t replay;
    uint8_t version;
    uint8_t rsc[8]; /* Key RSC sent in message 3 and group message 1 */
} authn;

static void put_replay(uint8_t r[8], uint64_t v) {
    for (int i = 7; i >= 0; i--) { r[i] = (uint8_t)v; v >>= 8; }
}

static uint32_t a_frame(authn *a, uint16_t info, const uint8_t *nonce, const uint8_t *plain, uint32_t pl, int mic,
                        uint8_t *out) {
    uint8_t wrapped[WPA_KEYDATA_MAX];
    wpa_key_frame k = {0};
    uint32_t n = 0;
    k.version = a->version;
    k.key_info = (uint16_t)(info | (uint16_t)wpa_desc_version(a->sec));
    k.key_len = 16;
    put_replay(k.replay, a->replay);
    memcpy(k.rsc, a->rsc, 8);
    if (nonce) memcpy(k.nonce, nonce, 32);
    if (plain) {
        k.data_len = (uint16_t)toy_wrap(a->ptk + 16, plain, pl, wrapped);
        k.data = wrapped;
    }
    if (wpa_key_build(&k, out, WPA_FRAME_MAX, &n) != WPA_OK) return 0;
    if (mic) toy_mic(0, a->ptk, out, n, out + WPA_MIC_OFF);
    return n;
}

static uint32_t a_m1(authn *a, uint8_t *out) {
    a->replay++;
    return a_frame(a, WPA_KI_PAIRWISE | WPA_KI_ACK, a->anonce, 0, 0, 0, out);
}

/* The authenticator side of message 2: derive the PTK from its SNonce, check the MIC. */
static int a_take_m2(authn *a, const uint8_t *m2, uint32_t n) {
    wpa_key_frame k;
    uint8_t data[76], copy[WPA_FRAME_MAX], mic[16];
    if (wpa_key_parse(m2, n, &k) != WPA_OK) return 0;
    memcpy(a->snonce, k.nonce, 32);
    int af = memcmp(AA, SPA, 6) < 0, nf = memcmp(a->anonce, a->snonce, 32) < 0;
    memcpy(data, af ? AA : SPA, 6);
    memcpy(data + 6, af ? SPA : AA, 6);
    memcpy(data + 12, nf ? a->anonce : a->snonce, 32);
    memcpy(data + 44, nf ? a->snonce : a->anonce, 32);
    wpa_kdf_sha256(a->pmk, "Pairwise key expansion", data, sizeof data, a->ptk, 48);
    memcpy(copy, m2, n);
    memset(copy + WPA_MIC_OFF, 0, 16);
    toy_mic(0, a->ptk, copy, n, mic);
    return memcmp(mic, m2 + WPA_MIC_OFF, 16) == 0 && k.data_len == sizeof RSNE && memcmp(k.data, RSNE, sizeof RSNE) == 0 &&
           k.key_info == (WPA_KI_PAIRWISE | WPA_KI_MIC | (uint16_t)wpa_desc_version(a->sec));
}

/* Key data: RSNE || GTK KDE || padding to a multiple of 8. */
static uint32_t kd_build(uint8_t *kd, const uint8_t *rsne, uint32_t rl, const uint8_t *gtk, uint8_t idx) {
    uint32_t n = 0;
    if (rsne) { memcpy(kd, rsne, rl); n = rl; }
    if (gtk) {
        uint8_t kde[] = {0xdd, 22, 0x00, 0x0f, 0xac, 0x01, idx, 0x00};
        memcpy(kd + n, kde, sizeof kde);
        memcpy(kd + n + sizeof kde, gtk, 16);
        n += sizeof kde + 16u;
    }
    if (n % 8u) { kd[n++] = 0xdd; while (n % 8u) kd[n++] = 0; }
    return n;
}

static uint32_t a_m3(authn *a, uint8_t *out, const uint8_t *gtk) {
    uint8_t kd[128];
    uint32_t kl = kd_build(kd, RSNE, sizeof RSNE, gtk, 1);
    a->replay++;
    return a_frame(a, WPA_KI_PAIRWISE | WPA_KI_INSTALL | WPA_KI_ACK | WPA_KI_MIC | WPA_KI_SECURE | WPA_KI_ENC,
                   a->anonce, kd, kl, 1, out);
}

static void setup(wpa_supp *w, authn *a, toy_ctx *t, wpa_crypto *c) {
    memset(a, 0, sizeof *a);
    a->sec = WIFI_SEC_WPA2_PSK_SHA256;
    a->version = 2;
    for (int i = 0; i < 32; i++) { a->pmk[i] = (uint8_t)(0x40 + i); a->anonce[i] = (uint8_t)(0xa0 ^ i); }
    t->counter = 1; t->salt = 0x3c;
    *c = toy_crypto(WIFI_SEC_WPA2_PSK_SHA256, t);
    CHECK(wpa_supp_start(w, c, WIFI_SEC_WPA2_PSK_SHA256, a->pmk, AA, SPA, RSNE, sizeof RSNE, RSNE, sizeof RSNE) == WPA_OK);
}

/* Run messages 1 to 4; leaves the supplicant in DONE. */
static void handshake(wpa_supp *w, authn *a) {
    uint8_t f[WPA_FRAME_MAX], r[WPA_FRAME_MAX];
    wpa_action act;
    uint32_t n = a_m1(a, f);
    CHECK(wpa_supp_rx(w, f, n, r, sizeof r, &act) == WPA_OK && act.reply_len > 0 && !act.install_ptk && !act.done);
    CHECK(a_take_m2(a, r, act.reply_len));
    n = a_m3(a, f, GTK1);
    CHECK(wpa_supp_rx(w, f, n, r, sizeof r, &act) == WPA_OK && act.install_ptk && act.install_gtk && act.done);
    wpa_key_frame k;
    CHECK(wpa_key_parse(r, act.reply_len, &k) == WPA_OK &&
          k.key_info == (WPA_KI_PAIRWISE | WPA_KI_MIC | WPA_KI_SECURE | WPA_VER_CMAC_AES) && k.data_len == 0);
    uint8_t want[8];
    put_replay(want, a->replay);
    CHECK(memcmp(k.replay, want, 8) == 0);
    CHECK(memcmp(wpa_supp_tk(w), a->ptk + 32, 16) == 0 && w->gtk_len == 16 && w->gtk_idx == 1 &&
          memcmp(w->gtk, GTK1, 16) == 0 && w->st == WPA_ST_DONE);
}

static void test_codec(void) {
    uint8_t f[WPA_FRAME_MAX], data[40];
    wpa_key_frame k = {0}, p;
    uint32_t n = 0;
    for (int i = 0; i < 40; i++) data[i] = (uint8_t)i;
    k.version = 2; k.key_info = 0x13ca; k.key_len = 16;
    for (int i = 0; i < 8; i++) k.replay[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 32; i++) k.nonce[i] = (uint8_t)(0x80 + i);
    k.data = data; k.data_len = 40;
    CHECK(wpa_key_build(&k, f, sizeof f, &n) == WPA_OK && n == 99u + 40u);
    CHECK(f[1] == 3 && f[2] == 0 && f[3] == 95u + 40u && f[4] == 2 && f[5] == 0x13 && f[6] == 0xca && f[97] == 0 && f[98] == 40);
    for (int i = 0; i < 16; i++) CHECK(f[81 + i] == 0);
    CHECK(wpa_key_parse(f, n, &p) == WPA_OK && p.key_info == 0x13ca && p.key_len == 16 && p.data_len == 40 &&
          memcmp(p.data, data, 40) == 0 && memcmp(p.nonce, k.nonce, 32) == 0 && memcmp(p.replay, k.replay, 8) == 0 &&
          p.total == n);
    CHECK(wpa_key_build(&k, f, n - 1, &n) == WPA_E_SPACE);
    CHECK(wpa_key_build(&k, f, sizeof f, &n) == WPA_OK);
    /* padding after the frame is fine; every truncation is refused */
    CHECK(wpa_key_parse(f, n + 3, &p) == WPA_OK && p.total == n);
    for (uint32_t l = 0; l < n; l++) CHECK(wpa_key_parse(f, l, &p) != WPA_OK);
    uint8_t g[WPA_FRAME_MAX];
    memcpy(g, f, n); g[1] = 0; CHECK(wpa_key_parse(g, n, &p) == WPA_E_FORMAT);   /* EAP packet, not Key */
    memcpy(g, f, n); g[4] = 254; CHECK(wpa_key_parse(g, n, &p) == WPA_E_FORMAT); /* WPA1 descriptor */
    memcpy(g, f, n); g[98] = 41; CHECK(wpa_key_parse(g, n, &p) == WPA_E_SHORT);  /* key data past body */
    memcpy(g, f, n); g[98] = 39; CHECK(wpa_key_parse(g, n, &p) == WPA_E_SHORT);  /* body and data disagree */
    memcpy(g, f, n); g[3] = (uint8_t)(g[3] + 1); CHECK(wpa_key_parse(g, n, &p) == WPA_E_SHORT);
    memcpy(g, f, n); g[2] = 0xff; CHECK(wpa_key_parse(g, n, &p) == WPA_E_SHORT);
    k.data_len = WPA_KEYDATA_MAX + 1;
    CHECK(wpa_key_build(&k, f, sizeof f, &n) == WPA_E_ARG);
    CHECK(wpa_key_parse(0, 99, &p) == WPA_E_ARG && wpa_key_build(0, f, 9, &n) == WPA_E_ARG);
}

static void test_kdf(void) {
    /* Reference: two HMAC-SHA256 blocks computed with OpenSSL 3.0.13
     * (`openssl dgst -sha256 -mac HMAC`) over i_le16 || "Pairwise key
     * expansion" || ctx || 0x80 0x01, key 00..1f, ctx 00..4b (76 bytes). */
    static const uint8_t want[48] = {
        0xca, 0x5d, 0x2b, 0x63, 0x20, 0x57, 0x53, 0xbc, 0x5b, 0x26, 0xc3, 0x1a, 0x13, 0x5d, 0x1f, 0xb9,
        0x64, 0xf1, 0xce, 0x5d, 0x87, 0x06, 0x22, 0x46, 0xc4, 0xda, 0x64, 0x33, 0xd5, 0x42, 0x2c, 0xa1,
        0x90, 0x65, 0xf6, 0xc5, 0x8c, 0x3d, 0xa7, 0x0e, 0x07, 0x56, 0x99, 0x26, 0x06, 0x52, 0x8f, 0x4d};
    uint8_t key[32], ctx[76], out[48];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    for (int i = 0; i < 76; i++) ctx[i] = (uint8_t)i;
    CHECK(wpa_kdf_sha256(key, "Pairwise key expansion", ctx, 76, out, 48) == 0 && memcmp(out, want, 48) == 0);
    CHECK(wpa_kdf_sha256(key, "x", ctx, 76, out, 0) != 0 && wpa_kdf_sha256(key, "x", ctx, 76, out, 8161) != 0);
    CHECK(wpa_kdf_sha256(0, "x", ctx, 76, out, 16) != 0);
}

static int rng_ok(void *c, uint8_t *o, uint32_t n) { (void)c; memset(o, 7, n); return 0; }

static void test_native_refuses(void) {
    wpa_supp w;
    uint8_t pmk[32] = {0};
    for (int s = WIFI_SEC_WPA2_PSK; s <= WIFI_SEC_WPA3_SAE; s++) {
        wpa_crypto c = wpa_crypto_native((wifi_sec)s, rng_ok, 0);
        CHECK(c.mic == 0 && c.unwrap == 0 && wpa_crypto_missing((wifi_sec)s, &c) != 0);
        CHECK(strstr(wpa_crypto_missing((wifi_sec)s, &c), "Key Wrap") != 0);
        CHECK(wpa_supp_start(&w, &c, (wifi_sec)s, pmk, AA, SPA, RSNE, sizeof RSNE, RSNE, sizeof RSNE) == WPA_E_CRYPTO);
        CHECK(w.st == WPA_ST_IDLE);
    }
    wpa_crypto c = wpa_crypto_native(WIFI_SEC_WPA2_PSK, rng_ok, 0);
    CHECK(c.prf == 0 && strstr(wpa_crypto_missing(WIFI_SEC_WPA2_PSK, &c), "SHA-1") != 0);
    c = wpa_crypto_native(WIFI_SEC_WPA2_PSK_SHA256, 0, 0);
    CHECK(c.prf != 0 && strstr(wpa_crypto_missing(WIFI_SEC_WPA2_PSK_SHA256, &c), "entropy") != 0);
    CHECK(wpa_crypto_missing(WIFI_SEC_OPEN, &c) != 0 && wpa_desc_version(WIFI_SEC_OPEN) == -1);
    CHECK(wpa_desc_version(WIFI_SEC_WPA2_PSK) == 2 && wpa_desc_version(WIFI_SEC_WPA2_PSK_SHA256) == 3 &&
          wpa_desc_version(WIFI_SEC_WPA3_SAE) == 0);
    /* bad elements */
    toy_ctx t = {1, 2};
    c = toy_crypto(WIFI_SEC_WPA2_PSK_SHA256, &t);
    uint8_t bad[sizeof RSNE];
    memcpy(bad, RSNE, sizeof RSNE); bad[1]++;
    CHECK(wpa_supp_start(&w, &c, WIFI_SEC_WPA2_PSK_SHA256, pmk, AA, SPA, bad, sizeof bad, RSNE, sizeof RSNE) == WPA_E_ARG);
    CHECK(wpa_supp_start(&w, &c, WIFI_SEC_WPA2_PSK_SHA256, pmk, AA, SPA, RSNE, sizeof RSNE, bad, sizeof bad) == WPA_E_ARG);
    /* not armed */
    uint8_t f[WPA_FRAME_MAX], r[WPA_FRAME_MAX];
    wpa_action a;
    memset(&w, 0, sizeof w);
    CHECK(wpa_supp_rx(&w, f, 99, r, sizeof r, &a) == WPA_E_STATE);
}

static void test_handshake(void) {
    wpa_supp w;
    authn a;
    toy_ctx t;
    wpa_crypto c;
    uint8_t f[WPA_FRAME_MAX], r[WPA_FRAME_MAX];
    wpa_action act;
    uint32_t n;

    setup(&w, &a, &t, &c);
    handshake(&w, &a);

    /* message 3 retransmitted with a fresh counter: message 4 again, no reinstall */
    n = a_m3(&a, f, GTK1);
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_OK && act.reply_len && !act.install_ptk && !act.install_gtk);
    /* the same message 3 replayed: refused */
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_E_REPLAY && act.reply_len == 0);
    /* message 1 after completion: refused (rekey is a new association) */
    n = a_m1(&a, f);
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_E_STATE);

    /* group rekey: new GTK installed once; same GTK again is not reinstalled */
    uint8_t kd[64];
    uint32_t kl = kd_build(kd, 0, 0, GTK2, 2);
    a.replay++;
    static const uint8_t RSC[8] = {0x34, 0x12, 0x00, 0x00, 0x01, 0x00, 0xee, 0xee}; /* PN0..PN5 little-endian = 0x100001234; bytes 6..7 unused */
    memcpy(a.rsc, RSC, 8);
    n = a_frame(&a, WPA_KI_ACK | WPA_KI_MIC | WPA_KI_SECURE | WPA_KI_ENC, 0, kd, kl, 1, f);
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_OK && act.install_gtk && w.gtk_idx == 2 &&
          memcmp(w.gtk, GTK2, 16) == 0 && !act.install_ptk);
    CHECK(w.gtk_rsc == 0x100001234ull); /* the group replay counter starts at the AP's Key RSC (12.7.6.4) */
    wpa_key_frame k;
    CHECK(wpa_key_parse(r, act.reply_len, &k) == WPA_OK && k.key_info == (WPA_KI_MIC | WPA_KI_SECURE | WPA_VER_CMAC_AES));
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_E_REPLAY);
    a.replay++;
    n = a_frame(&a, WPA_KI_ACK | WPA_KI_MIC | WPA_KI_SECURE | WPA_KI_ENC, 0, kd, kl, 1, f);
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_OK && act.reply_len && !act.install_gtk);
    /* group key id 0 is refused */
    kl = kd_build(kd, 0, 0, GTK1, 0);
    a.replay++;
    n = a_frame(&a, WPA_KI_ACK | WPA_KI_MIC | WPA_KI_SECURE | WPA_KI_ENC, 0, kd, kl, 1, f);
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_E_KEYDATA);
    /* group message with a bad MIC */
    kl = kd_build(kd, 0, 0, GTK1, 1);
    a.replay++;
    n = a_frame(&a, WPA_KI_ACK | WPA_KI_MIC | WPA_KI_SECURE | WPA_KI_ENC, 0, kd, kl, 1, f);
    f[WPA_MIC_OFF] ^= 1;
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_E_MIC && w.gtk_idx == 2);
    wpa_supp_wipe(&w);
    uint8_t zero[sizeof w] = {0};
    CHECK(memcmp(&w, zero, sizeof w) == 0);
}

typedef enum { M3_ANONCE, M3_MIC, M3_RSNE, M3_NOENC, M3_VERSION, M3_NOGTK, M3_PAD, M3_WRAP, M3_REQUEST, M3_SHAPE, M3_GTKID, M3_GTKLEN, M3_KEYLEN } m3_bad;

static void test_m3_attacks(void) {
    for (int bad = M3_ANONCE; bad <= M3_KEYLEN; bad++) {
        wpa_supp w;
        authn a;
        toy_ctx t;
        wpa_crypto c;
        uint8_t f[WPA_FRAME_MAX], r[WPA_FRAME_MAX], kd[128];
        wpa_action act;
        setup(&w, &a, &t, &c);
        uint32_t n = a_m1(&a, f);
        CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_OK && a_take_m2(&a, r, act.reply_len));
        uint16_t info = WPA_KI_PAIRWISE | WPA_KI_INSTALL | WPA_KI_ACK | WPA_KI_MIC | WPA_KI_SECURE | WPA_KI_ENC;
        uint8_t rsne[sizeof RSNE];
        memcpy(rsne, RSNE, sizeof RSNE);
        const uint8_t *gtk = GTK1;
        wpa_err want = WPA_E_MIC;
        if (bad == M3_RSNE) { rsne[sizeof rsne - 2] = 0; want = WPA_E_RSNE; } /* MFPC dropped: downgrade */
        if (bad == M3_NOGTK) { gtk = 0; want = WPA_E_KEYDATA; }
        if (bad == M3_NOENC) { info &= (uint16_t)~WPA_KI_ENC; want = WPA_E_FORMAT; }
        if (bad == M3_REQUEST) { info |= WPA_KI_REQUEST; want = WPA_E_FORMAT; }
        if (bad == M3_SHAPE) { info &= (uint16_t)~WPA_KI_INSTALL; want = WPA_E_FORMAT; }
        if (bad == M3_GTKID) want = WPA_E_KEYDATA; /* key id 0 is pairwise-only */
        uint32_t kl = kd_build(kd, rsne, sizeof rsne, gtk, bad == M3_GTKID ? 0 : 1);
        if (bad == M3_PAD) { kd[kl - 1] = 1; want = WPA_E_KEYDATA; }
        if (bad == M3_GTKLEN) { /* a 32-byte GTK: the group cipher is CCMP-128, so 16 only */
            static const uint8_t big[32] = {1};
            uint8_t kde[] = {0xdd, 6 + 32, 0x00, 0x0f, 0xac, 0x01, 1, 0x00};
            memcpy(kd, rsne, sizeof rsne);
            kl = sizeof rsne;
            memcpy(kd + kl, kde, sizeof kde);
            memcpy(kd + kl + sizeof kde, big, sizeof big);
            kl += sizeof kde + sizeof big;
            if (kl % 8u) { kd[kl++] = 0xdd; while (kl % 8u) kd[kl++] = 0; }
            want = WPA_E_KEYDATA;
        }
        if (bad == M3_ANONCE) { a.anonce[5] ^= 1; want = WPA_E_NONCE; }
        a.replay++;
        if (bad == M3_WRAP || bad == M3_KEYLEN) {
            /* WRAP: wrapped under a different KEK, MIC still right, unwrap integrity
             * fails. KEYLEN: a correct frame claiming a 32-byte pairwise key. */
            if (bad == M3_WRAP) a.ptk[16] ^= 1;
            uint8_t wrapped[WPA_KEYDATA_MAX];
            uint32_t wl = toy_wrap(a.ptk + 16, kd, kl, wrapped);
            if (bad == M3_WRAP) a.ptk[16] ^= 1;
            wpa_key_frame k = {0};
            k.version = 2; k.key_info = (uint16_t)(info | WPA_VER_CMAC_AES);
            k.key_len = bad == M3_KEYLEN ? 32 : WPA_TK_LEN;
            put_replay(k.replay, a.replay);
            memcpy(k.nonce, a.anonce, 32);
            k.data = wrapped; k.data_len = (uint16_t)wl;
            CHECK(wpa_key_build(&k, f, sizeof f, &n) == WPA_OK);
            toy_mic(0, a.ptk, f, n, f + WPA_MIC_OFF);
            want = bad == M3_KEYLEN ? WPA_E_FORMAT : WPA_E_KEYDATA;
        } else {
            n = a_frame(&a, info, a.anonce, kd, kl, 1, f);
        }
        if (bad == M3_MIC) f[WPA_MIC_OFF + 15] ^= 0x80;
        if (bad == M3_VERSION) { f[6] = (uint8_t)((f[6] & ~7u) | WPA_VER_SHA1_AES); want = WPA_E_VERSION; }
        wpa_err got = wpa_supp_rx(&w, f, n, r, sizeof r, &act);
        CHECK(got == want);
        if (got != want) fprintf(stderr, "  case %d got %s\n", bad, wpa_strerror(got));
        CHECK(act.reply_len == 0 && !act.install_ptk && !act.install_gtk && w.st == WPA_ST_WAIT_M3 && !w.ptk_installed);
        /* every truncation of a good message 3 is refused without effect */
        if (bad == M3_ANONCE) {
            a.anonce[5] ^= 1;
            a.replay++;
            n = a_m3(&a, f, GTK1);
            for (uint32_t l = 0; l < n; l++) CHECK(wpa_supp_rx(&w, f, l, r, sizeof r, &act) != WPA_OK && w.st == WPA_ST_WAIT_M3);
            CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_OK && act.done);
        }
    }
}

static void test_m1_untrusted(void) {
    wpa_supp w;
    authn a;
    toy_ctx t;
    wpa_crypto c;
    uint8_t f[WPA_FRAME_MAX], r[WPA_FRAME_MAX];
    wpa_action act;
    setup(&w, &a, &t, &c);
    /* a forged message 1 with a huge counter, then the real exchange at low counters */
    uint64_t real = a.replay;
    a.replay = 1000;
    uint32_t n = a_m1(&a, f);
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_OK);
    a.replay = real;
    handshake(&w, &a);
    /* message 1 with MIC set is not message 1 */
    setup(&w, &a, &t, &c);
    a.replay++;
    n = a_frame(&a, WPA_KI_PAIRWISE | WPA_KI_ACK | WPA_KI_MIC, a.anonce, 0, 0, 0, f);
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_E_FORMAT);
    /* no Ack bit: a supplicant-to-authenticator frame looped back */
    n = a_frame(&a, WPA_KI_PAIRWISE, a.anonce, 0, 0, 0, f);
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_E_FORMAT);
    /* message 3 before message 1 */
    setup(&w, &a, &t, &c);
    n = a_m3(&a, f, GTK1);
    CHECK(wpa_supp_rx(&w, f, n, r, sizeof r, &act) == WPA_E_STATE);
    /* reply buffer too small for message 2 */
    n = a_m1(&a, f);
    CHECK(wpa_supp_rx(&w, f, n, r, 50, &act) == WPA_E_SPACE && act.reply_len == 0);
    CHECK(w.refused[WPA_E_SPACE] == 1 && w.refused[WPA_E_STATE] == 1);
}

int main(void) {
    test_codec();
    test_kdf();
    test_native_refuses();
    test_handshake();
    test_m3_attacks();
    test_m1_untrusted();
    printf("wpa_test: %d checks, %d failures\n", checks, failures);
    printf("AIENOS_WPA_TEST: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
