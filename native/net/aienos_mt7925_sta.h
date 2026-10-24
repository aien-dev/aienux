/* MT7925 station-join MCU commands: NIC capability (efuse MAC), DEV_INFO,
 * BSS_INFO, remain-on-channel (ROC) and STA_REC, plus the pairwise/group key
 * record. Read from Linux mt76 (mt7925/mcu.c, mt76_connac_mcu.c, main.c at
 * _ext/mt76 467ae2e3, BSD-3-Clause-Clear) and written here; nothing copied.
 *
 * Every layout below is INFERRED from that reading and UNKNOWN on hardware
 * until the attended boot (docs/SPARK_WIFI_MT7925.md, rows J1..J9).
 *
 * The order the reference drives for a non-MLD station, which
 * mt7925_join_* follows:
 *   interface up : DEV_INFO(active, our MAC), BSS_INFO{BASIC} (conn_state 1)
 *   before auth  : BSS_INFO{BASIC,SEC,RATE,QBSS,MLD,IFS} for the AP,
 *                  STA_REC{BASIC,PHY,RA,STATE=NONE,HDR_TRANS} for the AP,
 *                  ROC acquire (JOIN) on the AP channel, wait for the grant
 *   after assoc  : BSS_INFO{... ,RLM} (enable), STA_REC{... STATE=ASSOC},
 *                  STA_REC for our own BSS record (state ASSOC)
 *   keys         : STA_REC{KEY_V3} pairwise on the AP record, group on ours
 *   authorized   : ROC abort
 *
 * Builders are pure (fill `out`, return the length). Header translation is
 * left disabled (dis_rx_hdr_tran = 1): frames stay 802.11 both ways. */
#ifndef AIENOS_MT7925_STA_H
#define AIENOS_MT7925_STA_H

#include "aienos_mt7925.h"
#include "aienos_wifi_sta.h"

#define MT7925_UNI_CMD_DEV_INFO 0x01u
#define MT7925_UNI_CMD_BSS_INFO 0x02u
#define MT7925_UNI_CMD_STA_REC 0x03u
#define MT7925_UNI_CMD_ROC 0x27u
#define MT7925_UNI_EVENT_ROC 0x27u
#define MT7925_UNI_EVENT_CMD_RESULT 0x01u

#define MT7925_NIC_CAP_TAG 0x3u       /* UNI_CHIP_CONFIG_NIC_CAPA */
#define MT7925_NIC_CAP_MAC_ADDR 7u

/* Index choices that match what the reference computes for one interface. */
#define MT7925_BSS_IDX 0u
#define MT7925_OMAC_IDX 0u
#define MT7925_BAND_AUTO 0xffu
#define MT7925_WCID_OWN 19u           /* MT792x_WTBL_RESERVED - bss idx 0 */
#define MT7925_WCID_AP 1u             /* first station slot (UNKNOWN: J8) */
#define MT7925_ROC_REQ_JOIN 0u
#define MT7925_ROC_MS 1000u           /* HZ in ms when mac80211 gives no duration */

#define MT7925_CONN_INFRA_STA 0x10001u /* STA_TYPE_STA | NETWORK_INFRA */
#define MT7925_CONN_INFRA_AP 0x10002u
#define MT7925_STA_STATE_NONE 0u
#define MT7925_STA_STATE_AUTH 1u
#define MT7925_STA_STATE_ASSOC 2u
#define MT7925_CIPHER_NONE 0u

#define MT7925_JOIN_CMD_MAX 256u

typedef struct {
    uint8_t own_mac[6];
    uint8_t bssid[6];
    uint8_t band;           /* 1 = 2.4 GHz, 2 = 5 GHz, 3 = 6 GHz */
    uint8_t channel;
    uint16_t beacon_int;
    uint8_t dtim;
    uint8_t qos;            /* AP advertised WMM */
    uint8_t short_slot;
    uint16_t aid;
    uint8_t cipher;         /* MT7925_CIPHER_CCMP once RSN, else MT7925_CIPHER_NONE */
    int8_t rssi;
} mt7925_join_cfg;

/* Pure builders: return bytes written, 0 if `cap` is too small or an
 * argument is out of range. */
uint32_t mt7925_build_nic_cap_query(uint8_t *out, uint32_t cap);
uint32_t mt7925_build_dev_info(uint8_t *out, uint32_t cap, const uint8_t mac[6], int active);
/* BSS_INFO with only the BASIC TLV for our own interface (reference
 * mt76_connac_mcu_uni_add_dev second half). */
uint32_t mt7925_build_bss_own(uint8_t *out, uint32_t cap, int active);
/* Full BSS_INFO for the AP. `enable` adds RLM (channel) and sets
 * conn_state 0 (connected), as after association. */
uint32_t mt7925_build_bss_info(uint8_t *out, uint32_t cap, const mt7925_join_cfg *c, int enable);
uint32_t mt7925_build_roc(uint8_t *out, uint32_t cap, uint8_t token, uint8_t channel, uint8_t band,
                          uint32_t duration_ms, int abort);
/* STA_REC for the AP (`wcid` MT7925_WCID_AP) or our own BSS record
 * (MT7925_WCID_OWN, conn type broadcast). */
uint32_t mt7925_build_sta_rec(uint8_t *out, uint32_t cap, const mt7925_join_cfg *c, uint16_t wcid, uint8_t state);
/* STA_REC{KEY_V3}. CCMP-128 only (16-byte key). `pairwise` selects the AP
 * record and tx_key; the group key goes on our own record. */
uint32_t mt7925_build_key(uint8_t *out, uint32_t cap, const mt7925_join_cfg *c, int pairwise, uint8_t key_id,
                          const uint8_t key[16]);

/* Parse a NIC capability reply (whole event, header included) for the
 * efuse MAC. Returns MT7925_OK and fills mac, MT7925_E_DEVICE on a malformed
 * reply, MT7925_E_STATE if the MAC element is absent. */
mt7925_err mt7925_parse_nic_cap(const uint8_t *ev, uint32_t len, uint8_t mac[6]);
/* ROC grant event (whole event): returns 1 and the token if it is a grant. */
int mt7925_parse_roc_grant(const uint8_t *ev, uint32_t len, uint8_t *token, uint8_t *status);

/* Driving the chip (each waits for the MCU reply; DEV/BSS/STA commands also
 * check the UNI result status). */
mt7925_err mt7925_read_mac(mt7925_dev *d, uint8_t mac[6]);
mt7925_err mt7925_join_iface_up(mt7925_dev *d, const uint8_t mac[6]);
/* Before authentication: BSS + STA records, then ROC on the AP channel and
 * wait (bounded) for the grant. */
mt7925_err mt7925_join_prepare(mt7925_dev *d, const mt7925_join_cfg *c);
mt7925_err mt7925_join_assoc_done(mt7925_dev *d, const mt7925_join_cfg *c);
/* Key material is copied into a command buffer and the MCU ring slot; both
 * are zeroed before return, whatever the outcome. */
mt7925_err mt7925_join_install_key(mt7925_dev *d, const mt7925_join_cfg *c, int pairwise, uint8_t key_id,
                                   const uint8_t key[16]);
mt7925_err mt7925_join_authorized(mt7925_dev *d);

/* Station adapter: binds a wifi_sta (aienos_wifi_sta.h) to this driver.
 * Frames from the data ring go through mt7925_rx_parse into wifi_sta_rx;
 * the station's radio calls become the join commands above and
 * mt7925_tx_80211. Takes over d->on_rx and d->cb_ctx. */
#define MT7925_LINK_RX_ERRS 9u
typedef struct {
    mt7925_dev *d;
    wifi_sta *sta;
    mt7925_join_cfg cfg;
    uint32_t rx_refused[MT7925_LINK_RX_ERRS]; /* by mt7925_rx_err */
} mt7925_link;
void mt7925_link_attach(mt7925_link *l, mt7925_dev *d, wifi_sta *s, const uint8_t mac[6]);

#endif
