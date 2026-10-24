/* WPA2/WPA3 supplicant key handshakes. See aienos_wpa.h. */
#include "aienos_wpa.h"

#include <string.h>

#include "../crypto/aienos_crypto.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* ---- EAPOL-Key codec (Figure 12-32) ---- */

wpa_err wpa_key_parse(const uint8_t *p, uint32_t len, wpa_key_frame *k) {
    if (!p || !k) return WPA_E_ARG;
    if (len < WPA_FRAME_MIN) return WPA_E_SHORT;
    if (p[1] != WPA_EAPOL_TYPE_KEY || p[4] != WPA_DESC_RSN) return WPA_E_FORMAT;
    uint32_t body = be16(p + 2), dl = be16(p + 97);
    /* The body length and the key data length must agree with each other
     * and with the bytes received (trailing padding is allowed, short is not). */
    if (body < WPA_KEY_FIXED || WPA_EAPOL_HDR + body > len) return WPA_E_SHORT;
    if (WPA_KEY_FIXED + dl != body) return WPA_E_SHORT;
    if (dl > WPA_KEYDATA_MAX) return WPA_E_SHORT;
    memset(k, 0, sizeof *k);
    k->version = p[0];
    k->desc_type = p[4];
    k->key_info = be16(p + 5);
    k->key_len = be16(p + 7);
    memcpy(k->replay, p + 9, WPA_REPLAY_LEN);
    memcpy(k->nonce, p + 17, WPA_NONCE_LEN);
    memcpy(k->iv, p + 49, 16);
    memcpy(k->rsc, p + 65, 8);
    memcpy(k->mic, p + WPA_MIC_OFF, WPA_MIC_LEN);
    k->data_len = (uint16_t)dl;
    k->data = p + WPA_FRAME_MIN;
    k->total = WPA_EAPOL_HDR + body;
    return WPA_OK;
}

wpa_err wpa_key_build(const wpa_key_frame *k, uint8_t *out, uint32_t cap, uint32_t *out_len) {
    if (!k || !out || !out_len || (k->data_len && !k->data)) return WPA_E_ARG;
    if (k->data_len > WPA_KEYDATA_MAX) return WPA_E_ARG;
    uint32_t n = WPA_FRAME_MIN + k->data_len;
    if (n > cap) return WPA_E_SPACE;
    memset(out, 0, WPA_FRAME_MIN);
    out[0] = k->version;
    out[1] = WPA_EAPOL_TYPE_KEY;
    put_be16(out + 2, (uint16_t)(n - WPA_EAPOL_HDR));
    out[4] = WPA_DESC_RSN;
    put_be16(out + 5, k->key_info);
    put_be16(out + 7, k->key_len);
    memcpy(out + 9, k->replay, WPA_REPLAY_LEN);
    memcpy(out + 17, k->nonce, WPA_NONCE_LEN);
    memcpy(out + 49, k->iv, 16);
    memcpy(out + 65, k->rsc, 8);
    put_be16(out + 97, k->data_len);
    if (k->data_len) memcpy(out + WPA_FRAME_MIN, k->data, k->data_len);
    *out_len = n;
    return WPA_OK;
}

/* ---- KDF-SHA256 (12.7.1.6.2) ---- */

int wpa_kdf_sha256(const uint8_t key[32], const char *label, const uint8_t *ctx, uint32_t ctx_len, uint8_t *out,
                   uint32_t out_len) {
    uint8_t h[32], le[2], bits[2];
    if (!key || !label || (ctx_len && !ctx) || !out || out_len == 0 || out_len > 8160u) return -1;
    uint32_t lbits = out_len * 8u;
    if (lbits > 0xffffu) return -1;
    bits[0] = (uint8_t)lbits;
    bits[1] = (uint8_t)(lbits >> 8);
    size_t ll = strlen(label);
    for (uint32_t i = 1, pos = 0; pos < out_len; i++) {
        aienos_hmac_sha256_ctx c;
        le[0] = (uint8_t)i;
        le[1] = (uint8_t)(i >> 8);
        aienos_hmac_sha256_init(&c, key);
        aienos_hmac_sha256_update(&c, le, 2);
        aienos_hmac_sha256_update(&c, (const uint8_t *)label, ll);
        if (ctx_len) aienos_hmac_sha256_update(&c, ctx, ctx_len);
        aienos_hmac_sha256_update(&c, bits, 2);
        aienos_hmac_sha256_final(&c, h);
        uint32_t take = out_len - pos < 32u ? out_len - pos : 32u;
        memcpy(out + pos, h, take);
        pos += take;
    }
    aienos_wipe(h, sizeof h);
    return 0;
}

static int native_prf_sha256(void *ctx, const uint8_t pmk[WPA_PMK_LEN], const uint8_t *data, uint32_t data_len,
                             uint8_t *out, uint32_t out_len) {
    (void)ctx;
    return wpa_kdf_sha256(pmk, "Pairwise key expansion", data, data_len, out, out_len);
}

int wpa_desc_version(wifi_sec sec) {
    switch (sec) {
    case WIFI_SEC_WPA2_PSK: return WPA_VER_SHA1_AES;
    case WIFI_SEC_WPA2_PSK_SHA256: return WPA_VER_CMAC_AES;
    case WIFI_SEC_WPA3_SAE: return WPA_VER_AKM;
    default: return -1;
    }
}

wpa_crypto wpa_crypto_native(wifi_sec sec, int (*random)(void *, uint8_t *, uint32_t), void *rng_ctx) {
    wpa_crypto c = {0};
    c.ctx = rng_ctx;
    c.random = random;
    /* AKM 2 derives the PTK with PRF-SHA1: no SHA-1 in native/. AKM 6 and 8
     * use KDF-SHA256 over HMAC-SHA256, which exists. Neither the EAPOL MIC
     * (HMAC-SHA1-128 for AKM 2, AES-128-CMAC for AKM 6 and 8) nor AES-128 Key
     * Wrap exists: both stay NULL. */
    if (sec == WIFI_SEC_WPA2_PSK_SHA256 || sec == WIFI_SEC_WPA3_SAE) c.prf = native_prf_sha256;
    return c;
}

const char *wpa_crypto_missing(wifi_sec sec, const wpa_crypto *c) {
    if (wpa_desc_version(sec) < 0) return "no 4-way handshake for this security";
    if (!c) return "no crypto table";
    if (c->random && c->prf && c->mic && c->unwrap) return 0;
    if (!c->random) return "platform entropy source (SNonce)";
    switch (sec) {
    case WIFI_SEC_WPA2_PSK:
        return "SHA-1 (PRF-SHA1 for the PTK, HMAC-SHA1-128 for the EAPOL MIC), AES-128 block and "
               "AES-128 Key Wrap RFC 3394 (GTK)";
    case WIFI_SEC_WPA2_PSK_SHA256:
        return "AES-128 block, AES-128-CMAC (EAPOL MIC), AES-128 Key Wrap RFC 3394 (GTK)";
    default:
        return "AES-128 block, AES-128-CMAC (EAPOL MIC), AES-128 Key Wrap RFC 3394 (GTK)";
    }
}

/* ---- Supplicant ---- */

void wpa_supp_wipe(wpa_supp *w) {
    if (w) aienos_wipe(w, sizeof *w);
}

const uint8_t *wpa_supp_tk(const wpa_supp *w) { return w ? w->ptk + WPA_KCK_LEN + WPA_KEK_LEN : 0; }

wpa_err wpa_supp_start(wpa_supp *w, const wpa_crypto *c, wifi_sec sec, const uint8_t pmk[WPA_PMK_LEN],
                       const uint8_t aa[6], const uint8_t spa[6], const uint8_t *own_rsne, uint8_t own_len,
                       const uint8_t *ap_rsne, uint8_t ap_len) {
    if (!w || !pmk || !aa || !spa || !own_rsne || !ap_rsne) return WPA_E_ARG;
    if (own_len < 2 || own_len > 64 || ap_len < 2 || ap_len > 64) return WPA_E_ARG;
    if (own_rsne[0] != WIFI_EID_RSN || own_rsne[1] + 2u != own_len) return WPA_E_ARG;
    if (ap_rsne[0] != WIFI_EID_RSN || ap_rsne[1] + 2u != ap_len) return WPA_E_ARG;
    wpa_supp_wipe(w);
    if (wpa_crypto_missing(sec, c)) return WPA_E_CRYPTO;
    w->c = *c;
    w->sec = sec;
    w->ver = wpa_desc_version(sec);
    memcpy(w->pmk, pmk, WPA_PMK_LEN);
    memcpy(w->aa, aa, 6);
    memcpy(w->spa, spa, 6);
    memcpy(w->own_rsne, own_rsne, own_len);
    w->own_rsne_len = own_len;
    memcpy(w->ap_rsne, ap_rsne, ap_len);
    w->ap_rsne_len = ap_len;
    w->st = WPA_ST_WAIT_M1;
    return WPA_OK;
}

/* Replay counters are 64-bit big endian; strictly greater than the last one
 * accepted (12.7.2 "Key Replay Counter"). */
static int replay_fresh(const wpa_supp *w, const uint8_t r[8]) {
    if (!w->replay_set) return 1;
    for (int i = 0; i < 8; i++)
        if (r[i] != w->replay[i]) return r[i] > w->replay[i];
    return 0;
}

static int lt(const uint8_t *a, const uint8_t *b, uint32_t n) { return memcmp(a, b, n) < 0; }

static wpa_err derive_tptk(wpa_supp *w) {
    uint8_t data[6 + 6 + WPA_NONCE_LEN + WPA_NONCE_LEN];
    int a_first = lt(w->aa, w->spa, 6), an_first = lt(w->anonce, w->snonce, WPA_NONCE_LEN);
    memcpy(data, a_first ? w->aa : w->spa, 6);
    memcpy(data + 6, a_first ? w->spa : w->aa, 6);
    memcpy(data + 12, an_first ? w->anonce : w->snonce, WPA_NONCE_LEN);
    memcpy(data + 12 + WPA_NONCE_LEN, an_first ? w->snonce : w->anonce, WPA_NONCE_LEN);
    int r = w->c.prf(w->c.ctx, w->pmk, data, sizeof data, w->tptk, WPA_PTK_LEN);
    aienos_wipe(data, sizeof data);
    return r ? WPA_E_PRIMITIVE : WPA_OK;
}

/* MIC over the received PDU with its MIC field zeroed, compared in constant time. */
static wpa_err verify_mic(wpa_supp *w, const uint8_t *kck, const uint8_t *p, uint32_t total) {
    uint8_t copy[WPA_FRAME_MAX], mic[WPA_MIC_LEN];
    if (total > sizeof copy) return WPA_E_SHORT;
    memcpy(copy, p, total);
    memset(copy + WPA_MIC_OFF, 0, WPA_MIC_LEN);
    int r = w->c.mic(w->c.ctx, kck, copy, total, mic);
    int ok = r == 0 && aienos_ct_equal(mic, p + WPA_MIC_OFF, WPA_MIC_LEN);
    aienos_wipe(copy, sizeof copy);
    aienos_wipe(mic, sizeof mic);
    return r ? WPA_E_PRIMITIVE : ok ? WPA_OK : WPA_E_MIC;
}

static wpa_err send_key(wpa_supp *w, uint16_t info, const uint8_t replay[8], const uint8_t *nonce,
                        const uint8_t *data, uint16_t dl, uint8_t version, uint8_t *out, uint32_t cap,
                        uint32_t *n) {
    wpa_key_frame k = {0};
    k.version = version;
    k.key_info = (uint16_t)(info | (uint16_t)w->ver);
    memcpy(k.replay, replay, 8);
    if (nonce) memcpy(k.nonce, nonce, WPA_NONCE_LEN);
    k.data = data;
    k.data_len = dl;
    wpa_err e = wpa_key_build(&k, out, cap, n);
    if (e != WPA_OK) return e;
    const uint8_t *kck = w->st == WPA_ST_WAIT_M1 ? w->tptk : w->ptk;
    return w->c.mic(w->c.ctx, kck, out, *n, out + WPA_MIC_OFF) ? WPA_E_PRIMITIVE : WPA_OK;
}

/* Walk decrypted key data: elements and KDEs (0xdd, len, OUI, type, data).
 * A single 0xdd 0x00.. run is padding (12.7.2 "Key Data"). */
static wpa_err parse_keydata(const uint8_t *kd, uint32_t n, int need_rsne, const uint8_t **rsne, const uint8_t **gtk,
                             uint32_t *gtk_len) {
    *rsne = 0;
    *gtk = 0;
    *gtk_len = 0;
    for (uint32_t i = 0; i < n;) {
        if (kd[i] == 0xdd && (i + 1 >= n || kd[i + 1] == 0)) {
            for (uint32_t j = i + 1; j < n; j++)
                if (kd[j]) return WPA_E_KEYDATA;
            break;
        }
        if (i + 2 > n || i + 2u + kd[i + 1] > n) return WPA_E_KEYDATA;
        const uint8_t *e = kd + i;
        uint32_t el = e[1];
        if (e[0] == WIFI_EID_RSN) {
            if (*rsne) return WPA_E_KEYDATA;
            *rsne = e;
        } else if (e[0] == 0xdd && el >= 4 && e[2] == 0x00 && e[3] == 0x0f && e[4] == 0xac && e[5] == 0x01) {
            /* GTK KDE: Key ID/Tx byte, reserved byte, GTK (Figure 12-35). The
             * group cipher is always CCMP-128 here (wifi_choose_security), so
             * the GTK is exactly 16 bytes; group key ids are 1..3. Checked
             * before any reply goes out. */
            if (*gtk || el != 6u + WPA_TK_LEN || (e[6] & 3u) == 0) return WPA_E_KEYDATA;
            *gtk = e + 6;
            *gtk_len = el - 4u;
        }
        i += 2u + el;
    }
    if (!*gtk || (need_rsne && !*rsne)) return WPA_E_KEYDATA;
    return WPA_OK;
}

static wpa_err take_gtk(wpa_supp *w, const uint8_t *g, uint32_t glen, const uint8_t rsc[8], wpa_action *a) {
    uint8_t idx = g[0] & 3u, len = (uint8_t)(glen - 2u);
    if (idx == 0) return WPA_E_KEYDATA; /* group keys use ids 1..3 */
    memcpy(w->gtk, g + 2, len);
    w->gtk_len = len;
    w->gtk_idx = idx;
    /* Never reinstall the key the chip already has (it would reset its RSC). */
    if (w->gtk_installed_len == len && w->gtk_installed_idx == idx &&
        aienos_ct_equal(w->gtk_installed, w->gtk, len))
        return WPA_OK;
    memcpy(w->gtk_installed, w->gtk, len);
    w->gtk_installed_len = len;
    w->gtk_installed_idx = idx;
    /* Key RSC (12.7.6.4): the receive counter for this key starts at the AP's packet number, bytes 0..5 = PN0..PN5,
     * so frames it sent under this key before we joined are replays. */
    w->gtk_rsc = 0;
    for (int b = 5; b >= 0; b--) w->gtk_rsc = (w->gtk_rsc << 8) | rsc[b];
    a->install_gtk = 1;
    return WPA_OK;
}

static wpa_err unwrap_keydata(wpa_supp *w, const uint8_t *kek, const wpa_key_frame *k, uint8_t *plain, uint32_t *pl) {
    if (k->data_len < 24 || k->data_len % 8u) return WPA_E_KEYDATA;
    return w->c.unwrap(w->c.ctx, kek, k->data, k->data_len, plain, pl) || *pl != k->data_len - 8u ? WPA_E_KEYDATA
                                                                                                  : WPA_OK;
}

static wpa_err rx_m1(wpa_supp *w, const wpa_key_frame *k, uint8_t *out, uint32_t cap, wpa_action *a) {
    if (w->st == WPA_ST_DONE) return WPA_E_STATE; /* rekey via a new association only */
    if (w->c.random(w->c.ctx, w->snonce, WPA_NONCE_LEN)) return WPA_E_PRIMITIVE;
    memcpy(w->anonce, k->nonce, WPA_NONCE_LEN);
    wpa_err e = derive_tptk(w);
    if (e != WPA_OK) return e;
    /* Message 1 carries no MIC, so its replay counter is not trusted state
     * (12.7.6.2): message 2 echoes it; only MIC-verified messages move
     * w->replay, so a forged message 1 cannot lock out the real message 3. */
    w->st = WPA_ST_WAIT_M1;
    e = send_key(w, WPA_KI_PAIRWISE | WPA_KI_MIC, k->replay, w->snonce, w->own_rsne, w->own_rsne_len, k->version,
                 out, cap, &a->reply_len);
    if (e != WPA_OK) return e;
    w->st = WPA_ST_WAIT_M3;
    return WPA_OK;
}

static wpa_err rx_m3(wpa_supp *w, const uint8_t *p, const wpa_key_frame *k, uint8_t *out, uint32_t cap, wpa_action *a) {
    uint8_t plain[WPA_KEYDATA_MAX];
    uint32_t pl = 0, glen = 0;
    const uint8_t *rsne, *gtk;
    int retransmit = w->st == WPA_ST_DONE;
    if (w->st != WPA_ST_WAIT_M3 && !retransmit) return WPA_E_STATE;
    if (!aienos_ct_equal(k->nonce, w->anonce, WPA_NONCE_LEN)) return WPA_E_NONCE;
    const uint8_t *ptk = retransmit ? w->ptk : w->tptk;
    wpa_err e = verify_mic(w, ptk, p, k->total);
    if (e != WPA_OK) return e;
    if (!(k->key_info & WPA_KI_ENC) || k->key_len != WPA_TK_LEN) return WPA_E_FORMAT;
    if ((e = unwrap_keydata(w, ptk + WPA_KCK_LEN, k, plain, &pl)) != WPA_OK) goto out;
    if ((e = parse_keydata(plain, pl, 1, &rsne, &gtk, &glen)) != WPA_OK) goto out;
    if (rsne[1] + 2u != w->ap_rsne_len || memcmp(rsne, w->ap_rsne, w->ap_rsne_len)) {
        e = WPA_E_RSNE;
        goto out;
    }
    if (!retransmit) memcpy(w->ptk, w->tptk, WPA_PTK_LEN);
    e = send_key(w, WPA_KI_PAIRWISE | WPA_KI_MIC | WPA_KI_SECURE, k->replay, 0, 0, 0, k->version, out, cap,
                 &a->reply_len);
    if (e != WPA_OK) goto out;
    memcpy(w->replay, k->replay, 8);
    w->replay_set = 1;
    if ((e = take_gtk(w, gtk, glen, k->rsc, a)) != WPA_OK) goto out;
    if (!w->ptk_installed) {
        w->ptk_installed = 1;
        a->install_ptk = 1;
    }
    aienos_wipe(w->tptk, sizeof w->tptk);
    w->st = WPA_ST_DONE;
    a->done = 1;
out:
    aienos_wipe(plain, sizeof plain);
    if (e != WPA_OK) a->reply_len = 0;
    return e;
}

static wpa_err rx_group(wpa_supp *w, const uint8_t *p, const wpa_key_frame *k, uint8_t *out, uint32_t cap,
                        wpa_action *a) {
    uint8_t plain[WPA_KEYDATA_MAX];
    uint32_t pl = 0, glen = 0;
    const uint8_t *rsne, *gtk;
    if (w->st != WPA_ST_DONE) return WPA_E_STATE;
    wpa_err e = verify_mic(w, w->ptk, p, k->total);
    if (e != WPA_OK) return e;
    if (!(k->key_info & WPA_KI_ENC)) return WPA_E_FORMAT;
    if ((e = unwrap_keydata(w, w->ptk + WPA_KCK_LEN, k, plain, &pl)) != WPA_OK) goto out;
    if ((e = parse_keydata(plain, pl, 0, &rsne, &gtk, &glen)) != WPA_OK) goto out;
    e = send_key(w, WPA_KI_MIC | WPA_KI_SECURE, k->replay, 0, 0, 0, k->version, out, cap, &a->reply_len);
    if (e != WPA_OK) goto out;
    memcpy(w->replay, k->replay, 8);
    e = take_gtk(w, gtk, glen, k->rsc, a);
out:
    aienos_wipe(plain, sizeof plain);
    if (e != WPA_OK) a->reply_len = 0;
    return e;
}

wpa_err wpa_supp_rx(wpa_supp *w, const uint8_t *p, uint32_t len, uint8_t *reply, uint32_t cap, wpa_action *a) {
    wpa_key_frame k;
    wpa_err e;
    if (!w || !p || !reply || !a) return WPA_E_ARG;
    memset(a, 0, sizeof *a);
    if (w->st == WPA_ST_IDLE) {
        e = WPA_E_STATE;
        goto done;
    }
    if ((e = wpa_key_parse(p, len, &k)) != WPA_OK) goto done;
    uint16_t ki = k.key_info;
    if ((int)(ki & WPA_KI_VER_MASK) != w->ver) {
        e = WPA_E_VERSION;
        goto done;
    }
    /* Authenticator-to-supplicant messages always set Ack and never Request,
     * Error or SMK (12.7.6.2, 12.7.6.4, 12.7.7.2). */
    if (!(ki & WPA_KI_ACK) || (ki & (WPA_KI_REQUEST | WPA_KI_ERROR | WPA_KI_SMK))) {
        e = WPA_E_FORMAT;
        goto done;
    }
    uint16_t shape = ki & (WPA_KI_PAIRWISE | WPA_KI_INSTALL | WPA_KI_MIC | WPA_KI_SECURE);
    /* Message 1 is the only one without a MIC; every other message is
     * checked for freshness here, before its MIC is computed. */
    if (shape != WPA_KI_PAIRWISE && !replay_fresh(w, k.replay)) {
        e = WPA_E_REPLAY;
        goto done;
    }
    if (shape == WPA_KI_PAIRWISE)
        e = rx_m1(w, &k, reply, cap, a);
    else if (shape == (WPA_KI_PAIRWISE | WPA_KI_INSTALL | WPA_KI_MIC | WPA_KI_SECURE))
        e = rx_m3(w, p, &k, reply, cap, a);
    else if (shape == (WPA_KI_MIC | WPA_KI_SECURE))
        e = rx_group(w, p, &k, reply, cap, a);
    else
        e = WPA_E_FORMAT;
done:
    if (e != WPA_OK) {
        memset(a, 0, sizeof *a);
        if ((unsigned)e < sizeof w->refused / sizeof w->refused[0]) w->refused[e]++;
    }
    return e;
}

const char *wpa_strerror(wpa_err e) {
    switch (e) {
    case WPA_OK: return "ok";
    case WPA_E_ARG: return "bad argument";
    case WPA_E_SHORT: return "EAPOL-Key frame too short or lengths disagree";
    case WPA_E_FORMAT: return "not a known EAPOL-Key message";
    case WPA_E_VERSION: return "key descriptor version does not match the AKM";
    case WPA_E_REPLAY: return "replay counter not fresh";
    case WPA_E_NONCE: return "message 3 ANonce differs from message 1";
    case WPA_E_MIC: return "MIC did not verify";
    case WPA_E_KEYDATA: return "key data unwrap or KDE check failed";
    case WPA_E_RSNE: return "RSNE in message 3 differs from the beacon";
    case WPA_E_STATE: return "message out of order";
    case WPA_E_CRYPTO: return "a primitive this AKM needs is missing";
    case WPA_E_SPACE: return "output buffer too small";
    case WPA_E_PRIMITIVE: return "a crypto primitive failed";
    }
    return "unknown";
}
