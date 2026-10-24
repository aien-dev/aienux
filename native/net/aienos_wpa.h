/* WPA2/WPA3 supplicant key handshakes, chip independent.
 *
 * Written from IEEE Std 802.11-2020: 12.7.2 (EAPOL-Key frames, Figure 12-32,
 * key information bits), 12.7.1.6.2 (KDF), 12.7.1.3 (PTK = KCK || KEK || TK),
 * 12.7.6 (4-way handshake, messages 1 to 4 and the supplicant's checks),
 * 12.7.7 (group key handshake) and 12.7.2 Table 12-8 (KDE format, GTK KDE).
 * Nothing here is MediaTek specific; any chip that installs keys through the
 * radio interface can use it.
 *
 * The cryptography sits behind wpa_crypto, one function per primitive. Only
 * what native/ already has is wired natively (wpa_crypto_native): random
 * from the platform, and the 802.11 KDF-SHA256 composed over the existing
 * HMAC-SHA256 (AKM 6 and 8 with a 32-byte PMK). The EAPOL MIC (HMAC-SHA1-128
 * or AES-128-CMAC) and AES-128 Key Wrap do not exist in native/ yet, so the
 * native table leaves them NULL and wpa_supp_start refuses with
 * WPA_E_CRYPTO and wpa_crypto_missing names them. Tests drive the state
 * machine with a non-cryptographic test double (tests/wpa_toy.h); that
 * double is never linked into the kernel.
 *
 * Checks the supplicant makes (each is a test): message shape per key-info
 * bits, descriptor version per AKM, strictly increasing replay counter,
 * ANonce of message 3 equals message 1, constant-time MIC verify before any
 * key data is used, unwrap integrity, the RSNE in message 3 equals the AP's
 * beacon RSNE byte for byte (downgrade), GTK KDE bounds, and no
 * reinstallation of a key already installed (key reinstallation attacks).
 * Secrets are wiped on every exit path. */
#ifndef AIENOS_WPA_H
#define AIENOS_WPA_H

#include <stddef.h>
#include <stdint.h>

#include "aienos_wifi.h"

/* EAPOL (IEEE 802.1X-2010 11.3) and EAPOL-Key fixed part (Figure 12-32, 16-byte MIC). */
#define WPA_EAPOL_HDR 4u
#define WPA_EAPOL_TYPE_KEY 3u
#define WPA_DESC_RSN 2u
#define WPA_KEY_FIXED 95u                       /* descriptor type .. key data length */
#define WPA_FRAME_MIN (WPA_EAPOL_HDR + WPA_KEY_FIXED) /* 99 */
#define WPA_MIC_OFF 81u                         /* MIC offset from the EAPOL version byte */
#define WPA_MIC_LEN 16u
#define WPA_NONCE_LEN 32u
#define WPA_REPLAY_LEN 8u
#define WPA_KEYDATA_MAX 256u
#define WPA_FRAME_MAX (WPA_FRAME_MIN + WPA_KEYDATA_MAX)

/* Key information (12.7.2 b). */
#define WPA_KI_VER_MASK 0x0007u
#define WPA_KI_PAIRWISE 0x0008u
#define WPA_KI_INSTALL 0x0040u
#define WPA_KI_ACK 0x0080u
#define WPA_KI_MIC 0x0100u
#define WPA_KI_SECURE 0x0200u
#define WPA_KI_ERROR 0x0400u
#define WPA_KI_REQUEST 0x0800u
#define WPA_KI_ENC 0x1000u
#define WPA_KI_SMK 0x2000u
/* Key descriptor version: 2 = HMAC-SHA1-128 + AES Key Wrap (AKM 2),
 * 3 = AES-128-CMAC + AES Key Wrap (AKM 6), 0 = defined by the AKM (AKM 8). */
#define WPA_VER_AKM 0u
#define WPA_VER_SHA1_AES 2u
#define WPA_VER_CMAC_AES 3u

/* CCMP-128 PTK: KCK 16 || KEK 16 || TK 16 (12.7.1.3, Table 12-11). */
#define WPA_KCK_LEN 16u
#define WPA_KEK_LEN 16u
#define WPA_TK_LEN 16u
#define WPA_PTK_LEN 48u
#define WPA_PMK_LEN 32u
#define WPA_GTK_MAX 32u

typedef enum {
    WPA_OK = 0,
    WPA_E_ARG = 1,
    WPA_E_SHORT = 2,     /* shorter than the EAPOL-Key fixed part or its own lengths */
    WPA_E_FORMAT = 3,    /* not an RSN EAPOL-Key, or key-info bits do not form a known message */
    WPA_E_VERSION = 4,   /* descriptor version is not the one this AKM uses */
    WPA_E_REPLAY = 5,    /* replay counter not above the last accepted one */
    WPA_E_NONCE = 6,     /* message 3 ANonce differs from message 1 */
    WPA_E_MIC = 7,       /* MIC did not verify */
    WPA_E_KEYDATA = 8,   /* unwrap failed, or a KDE overruns, or the GTK KDE is missing / bad */
    WPA_E_RSNE = 9,      /* RSNE in message 3 differs from the beacon (downgrade) */
    WPA_E_STATE = 10,    /* message out of order */
    WPA_E_CRYPTO = 11,   /* a primitive this AKM needs is missing (wpa_crypto_missing) */
    WPA_E_SPACE = 12,
    WPA_E_PRIMITIVE = 13, /* a primitive returned failure */
} wpa_err;

typedef struct {
    uint8_t version;     /* EAPOL protocol version */
    uint8_t desc_type;
    uint16_t key_info, key_len;
    uint8_t replay[WPA_REPLAY_LEN];
    uint8_t nonce[WPA_NONCE_LEN];
    uint8_t iv[16];
    uint8_t rsc[8];
    uint8_t mic[WPA_MIC_LEN];
    uint16_t data_len;
    const uint8_t *data;
    uint32_t total;      /* EAPOL header + body bytes the frame claims */
} wpa_key_frame;

/* Parse one EAPOL PDU (from the version byte). Refuses anything that is not
 * an RSN EAPOL-Key whose lengths agree with `len`. */
wpa_err wpa_key_parse(const uint8_t *p, uint32_t len, wpa_key_frame *k);
/* Build one EAPOL-Key PDU with a zero MIC field (the MIC goes in afterwards
 * at WPA_MIC_OFF). */
wpa_err wpa_key_build(const wpa_key_frame *k, uint8_t *out, uint32_t cap, uint32_t *out_len);

/* One function per primitive; NULL = absent. Return 0 on success. */
typedef struct {
    void *ctx;
    int (*random)(void *ctx, uint8_t *out, uint32_t n);
    /* PTK from PMK, addresses and nonces (12.7.1.3: Min/Max ordering is done by the caller). */
    int (*prf)(void *ctx, const uint8_t pmk[WPA_PMK_LEN], const uint8_t *data, uint32_t data_len, uint8_t *out,
               uint32_t out_len);
    int (*mic)(void *ctx, const uint8_t kck[WPA_KCK_LEN], const uint8_t *frame, uint32_t len, uint8_t mic[WPA_MIC_LEN]);
    /* Key unwrap; *out_len = in_len - 8 on success, nonzero on integrity failure. */
    int (*unwrap)(void *ctx, const uint8_t kek[WPA_KEK_LEN], const uint8_t *in, uint32_t in_len, uint8_t *out,
                  uint32_t *out_len);
} wpa_crypto;

/* 802.11 KDF-SHA256 (12.7.1.6.2) over the in-house HMAC-SHA256: 32-byte key,
 * label without its NUL, context, out_len bytes (<= 8160). */
int wpa_kdf_sha256(const uint8_t key[32], const char *label, const uint8_t *ctx, uint32_t ctx_len, uint8_t *out,
                   uint32_t out_len);
/* The table native/ can fill for `sec` today. `random` is the platform's
 * entropy source (NULL if it has none). */
wpa_crypto wpa_crypto_native(wifi_sec sec, int (*random)(void *, uint8_t *, uint32_t), void *rng_ctx);
/* NULL when `c` has every primitive `sec` needs, else a fixed sentence
 * naming the absent ones. */
const char *wpa_crypto_missing(wifi_sec sec, const wpa_crypto *c);
/* Descriptor version this AKM uses (WPA_VER_*), or -1 for no handshake. */
int wpa_desc_version(wifi_sec sec);

typedef enum {
    WPA_ST_IDLE = 0,
    WPA_ST_WAIT_M1 = 1,  /* associated, waiting for message 1 */
    WPA_ST_WAIT_M3 = 2,  /* message 2 sent */
    WPA_ST_DONE = 3,     /* message 4 sent, PTK and GTK installable */
} wpa_state;

/* What the caller must do after wpa_supp_rx returns WPA_OK. Order matters:
 * send `reply` first (message 4 goes out under the old keys), then install. */
typedef struct {
    uint32_t reply_len;          /* 0 = nothing to send */
    uint8_t install_ptk;         /* install tk as the pairwise key */
    uint8_t install_gtk;         /* install gtk[gtk_len] with gtk_idx */
    uint8_t done;                /* 4-way handshake complete (port may open) */
} wpa_action;

typedef struct {
    wpa_crypto c;
    wifi_sec sec;
    int ver;
    wpa_state st;
    uint8_t pmk[WPA_PMK_LEN];
    uint8_t aa[6], spa[6];
    uint8_t own_rsne[64], own_rsne_len;    /* full element we sent in association */
    uint8_t ap_rsne[64], ap_rsne_len;      /* full element from the AP's beacon */
    uint8_t anonce[WPA_NONCE_LEN], snonce[WPA_NONCE_LEN];
    uint8_t ptk[WPA_PTK_LEN];              /* KCK || KEK || TK once message 3 verified */
    uint8_t tptk[WPA_PTK_LEN];             /* candidate from message 1 */
    uint8_t replay[WPA_REPLAY_LEN];
    uint8_t replay_set;
    uint8_t ptk_installed;                 /* TK already handed to the chip */
    uint8_t gtk[WPA_GTK_MAX], gtk_len, gtk_idx;
    uint8_t gtk_installed[WPA_GTK_MAX], gtk_installed_len, gtk_installed_idx;
    uint64_t gtk_rsc;                      /* PN the AP gave for the installed GTK (Key RSC bytes 0..5), 12.7.6.4 */
    uint32_t refused[14];                  /* by wpa_err */
} wpa_supp;

/* Arm for one association. `own_rsne`/`ap_rsne` are full elements (id, len,
 * body). Refuses with WPA_E_CRYPTO when a primitive is absent. */
wpa_err wpa_supp_start(wpa_supp *w, const wpa_crypto *c, wifi_sec sec, const uint8_t pmk[WPA_PMK_LEN],
                       const uint8_t aa[6], const uint8_t spa[6], const uint8_t *own_rsne, uint8_t own_len,
                       const uint8_t *ap_rsne, uint8_t ap_len);
/* One received EAPOL PDU. On WPA_OK `a` says what to send and install
 * (`reply` holds the frame). A refused message never installs a key, never
 * moves the trusted replay counter and never reaches WPA_ST_DONE. */
wpa_err wpa_supp_rx(wpa_supp *w, const uint8_t *p, uint32_t len, uint8_t *reply, uint32_t cap, wpa_action *a);
/* Pairwise TK (valid after install_ptk). */
const uint8_t *wpa_supp_tk(const wpa_supp *w);
void wpa_supp_wipe(wpa_supp *w);
const char *wpa_strerror(wpa_err e);

#endif
