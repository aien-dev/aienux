/* Wi-Fi station: scan table, open-system authentication, association and
 * the Ethernet bridge, over a small radio interface any chip driver fills
 * (MT7925 on the Spark today; the iMac, Raspberry Pi and MacBook radios
 * later). Polled, no heap, no clock: time comes from the radio's udelay.
 *
 * Security: an RSN network is joined only when every primitive its AKM
 * needs exists in native/ (wifi_sta_missing_crypto). Today none of WPA2-PSK,
 * WPA2-PSK-SHA256 or WPA3-SAE is complete, so wifi_sta_connect refuses them
 * with WIFI_E_CRYPTO before sending anything; open networks work. The frame
 * layer (RSN element, protected-frame flag, chip key install) is in place
 * for when the primitives land. */
#ifndef AIENOS_WIFI_STA_H
#define AIENOS_WIFI_STA_H

#include "aienos_wifi.h"
#include "aienos_wpa.h"

#define WIFI_STA_RXQ 8u
#define WIFI_STA_ETH_MAX 1536u
#define WIFI_ETHERTYPE_EAPOL 0x888eu

typedef struct {
    void *ctx;
    /* Before authentication: chip-side records for `b` and on its channel. 0 = ok. */
    int (*join_prepare)(void *ctx, const wifi_bss *b);
    /* After a successful association response. */
    int (*assoc_done)(void *ctx, const wifi_bss *b, uint16_t aid);
    /* Send one 802.11 frame (no FCS). `protect`: the chip encrypts. 0 = ok. */
    int (*tx)(void *ctx, const uint8_t *frame, uint32_t len, int protect);
    /* Drain the chip; received frames come back through wifi_sta_rx. <0 = broken. */
    int (*poll)(void *ctx);
    void (*udelay)(void *ctx, uint32_t us);
    /* Hand a key to the chip: pairwise (key_id 0) or group (1..3). 0 = ok. */
    int (*set_key)(void *ctx, int pairwise, uint8_t key_id, const uint8_t *key, uint32_t len);
    /* Platform entropy for the SNonce (the kernel: ck_entropy_fill), with its
     * own context. NULL = none: RSN joins are refused. 0 = ok. */
    int (*random)(void *rng_ctx, uint8_t *out, uint32_t n);
    void *rng_ctx;
} wifi_radio;

typedef enum {
    WIFI_STA_IDLE = 0,
    WIFI_STA_AUTH = 1,      /* authentication request sent */
    WIFI_STA_ASSOC = 2,     /* association request sent */
    WIFI_STA_RUN = 3,       /* associated, data flows */
    WIFI_STA_KEYS = 4,      /* associated, 4-way handshake running: only EAPOL flows */
} wifi_sta_state;

typedef struct {
    wifi_radio r;
    uint8_t mac[6];
    wifi_bss_table scan;
    wifi_bss cur;
    wifi_sec sec;
    wifi_sta_state st;
    uint16_t aid;
    uint16_t last_status;   /* status code of the last refused auth/assoc */
    uint16_t seq;           /* 802.11 sequence number for data frames */
    uint8_t auth_done, assoc_done;
    uint8_t rxq[WIFI_STA_RXQ][WIFI_STA_ETH_MAX];
    uint16_t rxq_len[WIFI_STA_RXQ];
    uint32_t rxq_head, rxq_n;
    uint32_t rx_frames, rx_dropped, rx_unprotected_dropped, deauths;
    const wpa_crypto *crypto;   /* caller's table, or NULL = native */
    wpa_crypto tbl;             /* the table this association uses */
    uint8_t pmk[WPA_PMK_LEN], has_pmk;
    wpa_supp wpa;
    uint8_t keys_done;      /* 1 = handshake complete and keys installed, 2 = key install failed */
    uint8_t ptk_set;        /* pairwise key is in the chip: EAPOL replies go out protected */
    uint32_t eapol_refused, key_install_failed;
    /* CCMP replay check (12.5.3.4.4): last packet number accepted per TID
     * (index 16 = non-QoS), pairwise and group, reset when that key is
     * installed. The chip decrypts but does not check it. */
    uint64_t pn_ptk[17], pn_gtk[17];
    uint64_t pn_mgmt;          /* robust management frames under the PTK (deauth, disassoc), own counter */
    uint32_t mgmt_unprotected_dropped; /* deauth/disassoc refused on a link with protected management frames */
    uint32_t rx_replay_dropped;
    /* Bumped whenever the association ends (leave, deauth from the AP): a
     * callback that ran nested inside a TX can tell its state is gone. */
    uint32_t epoch;
    /* EAPOL that arrived between the association response and the start of
     * the supplicant (the AP may send message 1 at once), held for it. */
    uint8_t early_eapol[WPA_FRAME_MAX];
    uint16_t early_len;
} wifi_sta;

void wifi_sta_init(wifi_sta *s, const wifi_radio *r, const uint8_t mac[6]);
/* Every received 802.11 frame. `decrypted`: the chip opened it (CCMP). */
void wifi_sta_rx(wifi_sta *s, const uint8_t *frame, uint32_t len, int decrypted, int8_t rssi);
/* The same with the CCMP packet number the chip reported for a decrypted
 * frame (has_pn = 0: none). On a protected association a decrypted data
 * frame without a packet number, or with one not above the last, is dropped.
 * wifi_sta_rx is this with has_pn = 0. */
void wifi_sta_rx_pn(wifi_sta *s, const uint8_t *frame, uint32_t len, int decrypted, int has_pn, uint64_t pn,
                    int8_t rssi);
/* Strongest scanned BSS for `ssid`; open system auth + association, bounded
 * by budget_us of radio time per step. */
wifi_err wifi_sta_connect(wifi_sta *s, const uint8_t *ssid, uint8_t ssid_len, uint32_t budget_us);
/* PMK for PSK networks (the 32-byte PSK; a passphrase needs PBKDF2-SHA1,
 * which native/ lacks) and the crypto table; NULL table = native. */
void wifi_sta_set_psk(wifi_sta *s, const uint8_t pmk[WPA_PMK_LEN], const wpa_crypto *c);
/* Ethernet II frame from the stack (aienos_net) to the AP. */
wifi_err wifi_sta_send_eth(wifi_sta *s, const uint8_t *eth, uint32_t len);
/* Next received Ethernet II frame: 1 = one copied, 0 = none. */
int wifi_sta_recv_eth(wifi_sta *s, uint8_t *out, uint32_t cap, uint32_t *len);
wifi_err wifi_sta_disconnect(wifi_sta *s);
/* NULL when native/ has every primitive `sec` needs, else a fixed list of
 * the missing ones. */
const char *wifi_sta_missing_crypto(wifi_sec sec);

#endif
