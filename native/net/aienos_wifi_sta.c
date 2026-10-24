/* Wi-Fi station. See aienos_wifi_sta.h. */
#include "aienos_wifi_sta.h"

#include "../crypto/aienos_crypto.h"

#define STEP_US 1000u

static int eq6(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}
static void cpy(uint8_t *d, const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

void wifi_sta_init(wifi_sta *s, const wifi_radio *r, const uint8_t mac[6]) {
    if (!s || !r || !mac) return;
    *s = (wifi_sta){0};
    s->r = *r;
    cpy(s->mac, mac, 6);
    wifi_bss_table_init(&s->scan);
}

void wifi_sta_set_psk(wifi_sta *s, const uint8_t pmk[WPA_PMK_LEN], const wpa_crypto *c) {
    if (!s) return;
    if (!pmk) {
        aienos_wipe(s->pmk, sizeof s->pmk);
        s->has_pmk = 0;
        return;
    }
    cpy(s->pmk, pmk, WPA_PMK_LEN);
    s->has_pmk = 1;
    s->crypto = c;
}

const char *wifi_sta_missing_crypto(wifi_sec sec) {
    /* What native/ has: AES-256 block, AES-GCM-SIV, POLYVAL, HMAC-SHA256,
     * SHA-256, SHA-512, Ed25519, X25519. CCMP-128 itself runs in the chip. */
    switch (sec) {
    case WIFI_SEC_OPEN:
        return 0;
    case WIFI_SEC_WPA2_PSK:
        return "SHA-1, PBKDF2-HMAC-SHA1 (passphrase to PMK), PRF-SHA1 (PTK), HMAC-SHA1-128 (EAPOL MIC), "
               "AES-128 block and AES-128 Key Wrap RFC 3394 (GTK)";
    case WIFI_SEC_WPA2_PSK_SHA256:
        return "SHA-1 and PBKDF2-HMAC-SHA1 (passphrase to PMK; a raw 32-byte PSK skips it), AES-128 block, "
               "AES-128-CMAC (EAPOL MIC), AES-128 Key Wrap (GTK). KDF-SHA256 exists (wpa_kdf_sha256)";
    case WIFI_SEC_WPA3_SAE:
        return "NIST P-256 group arithmetic and SAE hash-to-element (Dragonfly commit/confirm), "
               "AES-128 block, AES-128-CMAC (EAPOL MIC), AES-128 Key Wrap (GTK/IGTK)";
    default:
        return "unsupported security (WEP, TKIP, 802.1X/EAP)";
    }
}

/* Drop the association: forget the keys, then tell the AP (deauth with
 * `reason`) so it does not keep a half-built security association. The
 * chip-side key entries are not removed (no key-delete command is wired;
 * docs/SPARK_WIFI_MT7925.md row R25); the next join_prepare rewrites them. */
static wifi_err leave(wifi_sta *s, uint16_t reason) {
    uint8_t f[32];
    uint32_t n = 0;
    s->st = WIFI_STA_IDLE;
    wpa_supp_wipe(&s->wpa);
    s->ptk_set = 0;
    s->epoch++;
    wifi_err e = wifi_build_deauth(f, sizeof f, s->cur.bssid, s->mac, s->cur.bssid, reason, &n);
    if (e != WIFI_OK) return e;
    return s->r.tx(s->r.ctx, f, n, 0) == 0 ? WIFI_OK : WIFI_E_RADIO;
}

/* One EAPOL PDU from the AP: run the supplicant, send its reply (under the
 * keys the chip has now), then install what it derived (12.7.6.4: message
 * 4 goes out before the new pairwise key is used). */
static void eapol_rx(wifi_sta *s, const uint8_t *p, uint32_t len) {
    uint8_t reply[WPA_FRAME_MAX], eth[14 + WPA_FRAME_MAX], f[WIFI_HDR_MGMT + 8u + 14u + WPA_FRAME_MAX];
    uint32_t n = 0;
    wpa_action a;
    wpa_err we = wpa_supp_rx(&s->wpa, p, len, reply, sizeof reply, &a);
    if (we != WPA_OK) {
        s->eapol_refused++;
        /* A protected message 1 after the handshake is a PTK rekey, which the
         * supplicant does not do (row R24): leave cleanly so the caller
         * reconnects, rather than keep a key the AP is about to retire. */
        if (s->st == WIFI_STA_RUN && we == WPA_E_STATE) (void)leave(s, WIFI_REASON_UNSPECIFIED);
        return;
    }
    /* The TX below may poll the chip (ring full) and deliver frames here,
     * among them a deauthentication that wipes the supplicant: then nothing
     * it derived may be installed. */
    uint32_t epoch = s->epoch;
    if (a.reply_len) {
        cpy(eth, s->cur.bssid, 6);
        cpy(eth + 6, s->mac, 6);
        eth[12] = (uint8_t)(WIFI_ETHERTYPE_EAPOL >> 8);
        eth[13] = (uint8_t)WIFI_ETHERTYPE_EAPOL;
        cpy(eth + 14, reply, a.reply_len);
        if (wifi_eth_to_80211(eth, 14u + a.reply_len, s->cur.bssid, (uint16_t)(s->seq << 4), f, sizeof f, &n) ==
            WIFI_OK) {
            if (s->ptk_set) f[1] |= WIFI_FL_PROTECTED;
            if (s->r.tx(s->r.ctx, f, n, s->ptk_set) == 0) s->seq = (uint16_t)((s->seq + 1u) & 0xfffu);
        }
        aienos_wipe(reply, sizeof reply);
        aienos_wipe(eth, sizeof eth);
        aienos_wipe(f, sizeof f);
    }
    if (s->epoch != epoch) {
        s->eapol_refused++;
        return;
    }
    if ((a.install_ptk && s->r.set_key(s->r.ctx, 1, 0, wpa_supp_tk(&s->wpa), WPA_TK_LEN) != 0) ||
        (a.install_gtk && s->r.set_key(s->r.ctx, 0, s->wpa.gtk_idx, s->wpa.gtk, s->wpa.gtk_len) != 0)) {
        s->key_install_failed++;
        s->keys_done = 2;
        /* In RUN nobody is waiting on keys_done (a group rekey): the chip
         * holds a stale group key, so leave. In KEYS connect fails and leaves. */
        if (s->st == WIFI_STA_RUN) (void)leave(s, WIFI_REASON_UNSPECIFIED);
        return;
    }
    /* A new key starts its packet numbers again (12.5.3.4.4). */
    if (a.install_ptk) {
        s->ptk_set = 1;
        for (int t = 0; t < 17; t++) s->pn_ptk[t] = 0;
        s->pn_mgmt = 0;
    }
    if (a.install_gtk) /* the group counter starts at the AP's Key RSC, not 0 (12.7.6.4) */
        for (int t = 0; t < 17; t++) s->pn_gtk[t] = s->wpa.gtk_rsc;
    if (a.done && !s->keys_done) s->keys_done = 1;
}

static void queue_eth(wifi_sta *s, const uint8_t *f, uint32_t len, int decrypted) {
    uint8_t *slot;
    uint32_t n = 0;
    if (s->rxq_n == WIFI_STA_RXQ) {
        s->rx_dropped++;
        return;
    }
    slot = s->rxq[(s->rxq_head + s->rxq_n) % WIFI_STA_RXQ];
    if (wifi_80211_to_eth(f, len, s->cur.bssid, decrypted, slot, WIFI_STA_ETH_MAX, &n) != WIFI_OK) {
        s->rx_dropped++;
        return;
    }
    uint16_t et = (uint16_t)((slot[12] << 8) | slot[13]);
    /* On a protected association only EAPOL may arrive in the clear. */
    if (s->sec != WIFI_SEC_OPEN && !decrypted && et != WIFI_ETHERTYPE_EAPOL) {
        s->rx_unprotected_dropped++;
        return;
    }
    /* Key messages go to the supplicant, never up the stack; until the
     * handshake is done nothing else is accepted. */
    if (s->sec != WIFI_SEC_OPEN && et == WIFI_ETHERTYPE_EAPOL) {
        /* The authenticator is the AP itself (source = BSSID), and once the
         * pairwise key is in, its key messages come protected. */
        if (!eq6(slot + 6, s->cur.bssid) || (s->ptk_set && !decrypted)) {
            s->rx_unprotected_dropped += s->ptk_set && !decrypted;
            s->eapol_refused++;
            return;
        }
        if (s->st == WIFI_STA_ASSOC) {
            if (n - 14u <= sizeof s->early_eapol) {
                cpy(s->early_eapol, slot + 14, n - 14u);
                s->early_len = (uint16_t)(n - 14u);
            } else {
                s->eapol_refused++; /* larger than any key message 1 */
            }
        } else {
            eapol_rx(s, slot + 14, n - 14u);
        }
        return;
    }
    if (s->st != WIFI_STA_RUN) {
        s->rx_dropped++;
        return;
    }
    s->rxq_len[(s->rxq_head + s->rxq_n) % WIFI_STA_RXQ] = (uint16_t)n;
    s->rxq_n++;
}

void wifi_sta_rx(wifi_sta *s, const uint8_t *f, uint32_t len, int decrypted, int8_t rssi) {
    wifi_sta_rx_pn(s, f, len, decrypted, 0, 0, rssi);
}

/* CCMP replay check for a decrypted data frame from the AP: 1 = fresh. */
static int pn_fresh(wifi_sta *s, const uint8_t *f, uint32_t len, const uint8_t *a1, int has_pn, uint64_t pn) {
    /* QoS data (subtype bit 3) carries the TID in QoS Control right after
     * the 3-address header; non-QoS frames share counter 16. */
    uint32_t tid = (f[0] & 0x80u) && len >= 26u ? (f[24] & 0x0fu) : 16u;
    uint64_t *last = (a1[0] & 1u) ? &s->pn_gtk[tid] : &s->pn_ptk[tid];
    if (!has_pn || pn <= *last) {
        s->rx_replay_dropped++;
        return 0;
    }
    *last = pn;
    return 1;
}

void wifi_sta_rx_pn(wifi_sta *s, const uint8_t *f, uint32_t len, int decrypted, int has_pn, uint64_t pn,
                    int8_t rssi) {
    wifi_hdr h;
    if (!s || wifi_parse_hdr(f, len, &h) != WIFI_OK) {
        if (s) s->rx_dropped++;
        return;
    }
    s->rx_frames++;
    if (h.type == WIFI_TYPE_MGMT && (h.subtype == WIFI_ST_BEACON || h.subtype == WIFI_ST_PROBE_RESP)) {
        wifi_bss b;
        if (wifi_parse_beacon(f, len, &b) == WIFI_OK) {
            b.rssi = rssi;
            wifi_bss_table_update(&s->scan, &b);
        }
        return;
    }
    int from_ap = s->st != WIFI_STA_IDLE && eq6(h.a2, s->cur.bssid) && eq6(h.a1, s->mac);
    if (h.type == WIFI_TYPE_MGMT) {
        /* An AP that restarts deauthenticates everyone at once (Address 1
         * broadcast); like a unicast one it is accepted from our BSSID only. */
        int bcast = h.a1[0] == 0xff && h.a1[1] == 0xff && h.a1[2] == 0xff && h.a1[3] == 0xff && h.a1[4] == 0xff &&
                    h.a1[5] == 0xff;
        int leave_frame = h.subtype == WIFI_ST_DEAUTH || h.subtype == WIFI_ST_DISASSOC;
        if (!from_ap && !(leave_frame && bcast && s->st != WIFI_STA_IDLE && eq6(h.a2, s->cur.bssid))) return;
        uint16_t alg = 0, seq = 0, st = 0, cap = 0, aid = 0;
        if (h.subtype == WIFI_ST_AUTH && s->st == WIFI_STA_AUTH &&
            wifi_parse_auth(f, len, &alg, &seq, &st, 0, 0) == WIFI_OK && alg == WIFI_AUTH_OPEN && seq == 2) {
            s->last_status = st;
            s->auth_done = 1;
        } else if (h.subtype == WIFI_ST_ASSOC_RESP && s->st == WIFI_STA_ASSOC &&
                   wifi_parse_assoc_resp(f, len, &cap, &st, &aid) == WIFI_OK) {
            s->last_status = st;
            s->aid = aid;
            s->assoc_done = 1;
        } else if ((h.subtype == WIFI_ST_DEAUTH || h.subtype == WIFI_ST_DISASSOC) && h.body_len >= 2) {
            /* Protected management frames negotiated (AKM 6 sends MFPC; SAE needs MFPR) and the PTK in place:
             * deauth/disassoc are robust frames (802.11-2020 12.6.19), so only a unicast one the chip
             * decrypted with our key, with a fresh packet number, ends the link. Broadcast ones need BIP
             * under the IGTK, which is not implemented, so they are refused on such a link. */
            int pmf = s->sec == WIFI_SEC_WPA2_PSK_SHA256 || s->sec == WIFI_SEC_WPA3_SAE;
            if (pmf && s->ptk_set && (bcast || !decrypted || !has_pn || pn <= s->pn_mgmt)) {
                s->mgmt_unprotected_dropped++;
                return;
            }
            if (pmf && s->ptk_set) s->pn_mgmt = pn;
            s->deauths++;
            s->last_status = (uint16_t)(h.body[0] | (h.body[1] << 8));
            s->st = WIFI_STA_IDLE;
            wpa_supp_wipe(&s->wpa);
            s->ptk_set = 0;
            s->epoch++;
        }
        return;
    }
    if (h.type == WIFI_TYPE_DATA && (s->st == WIFI_STA_RUN || s->st == WIFI_STA_KEYS || s->st == WIFI_STA_ASSOC) && eq6(h.a2, s->cur.bssid)) {
        if (decrypted && s->sec != WIFI_SEC_OPEN && !pn_fresh(s, f, len, h.a1, has_pn, pn)) return;
        queue_eth(s, f, len, decrypted);
        return;
    }
    s->rx_dropped++;
}

/* Poll the radio until `flag` is set or the budget is spent. */
static wifi_err wait_flag(wifi_sta *s, const uint8_t *flag, uint32_t budget_us) {
    for (uint32_t spent = 0; !*flag; spent += STEP_US) {
        if (s->r.poll(s->r.ctx) < 0) return WIFI_E_RADIO;
        if (s->st == WIFI_STA_IDLE) return WIFI_E_REFUSED; /* deauthenticated meanwhile */
        if (*flag) break;
        if (spent >= budget_us) return WIFI_E_TIMEOUT;
        s->r.udelay(s->r.ctx, STEP_US);
    }
    return WIFI_OK;
}

wifi_err wifi_sta_connect(wifi_sta *s, const uint8_t *ssid, uint8_t ssid_len, uint32_t budget_us) {
    uint8_t f[512];
    uint32_t n = 0;
    if (!s || !ssid || ssid_len > WIFI_SSID_MAX) return WIFI_E_ARG;
    const wifi_bss *b = wifi_bss_select(&s->scan, ssid, ssid_len);
    if (!b) {
        /* An RSN network we cannot join is not "usable": say why. */
        for (uint32_t i = 0; i < s->scan.n; i++) {
            const wifi_bss *e = &s->scan.e[i];
            if (e->ssid_len == ssid_len) {
                int same = 1;
                for (uint8_t k = 0; k < ssid_len; k++) same &= e->ssid[k] == ssid[k];
                if (same) {
                    s->sec = wifi_choose_security(e);
                    return WIFI_E_CRYPTO;
                }
            }
        }
        return WIFI_E_NOT_FOUND;
    }
    s->cur = *b;
    s->sec = wifi_choose_security(b);
    if (s->sec != WIFI_SEC_OPEN) {
        /* SAE needs its own exchange before the 4-way; nothing provides it. */
        if (s->sec == WIFI_SEC_WPA3_SAE || !s->has_pmk || !s->cur.rsne_len) return WIFI_E_CRYPTO;
        s->tbl = s->crypto ? *s->crypto : wpa_crypto_native(s->sec, s->r.random, s->r.rng_ctx);
        if (wpa_crypto_missing(s->sec, &s->tbl)) return WIFI_E_CRYPTO;
    }
    if (s->r.join_prepare(s->r.ctx, &s->cur) != 0) return WIFI_E_RADIO;

    s->auth_done = s->assoc_done = 0;
    s->early_len = 0;
    s->st = WIFI_STA_AUTH;
    wifi_err e = wifi_build_auth(f, sizeof f, s->cur.bssid, s->mac, WIFI_AUTH_OPEN, 1, 0, 0, 0, &n);
    if (e != WIFI_OK) goto fail;
    if (s->r.tx(s->r.ctx, f, n, 0) != 0) { e = WIFI_E_RADIO; goto fail; }
    if ((e = wait_flag(s, &s->auth_done, budget_us)) != WIFI_OK) goto fail;
    if (s->last_status != WIFI_STATUS_SUCCESS) { e = WIFI_E_REFUSED; goto fail; }

    s->st = WIFI_STA_ASSOC;
    if ((e = wifi_build_assoc_req(f, sizeof f, &s->cur, s->mac, s->sec, 10, &n)) != WIFI_OK) goto fail;
    if (s->r.tx(s->r.ctx, f, n, 0) != 0) { e = WIFI_E_RADIO; goto fail; }
    if ((e = wait_flag(s, &s->assoc_done, budget_us)) != WIFI_OK) goto fail;
    if (s->last_status != WIFI_STATUS_SUCCESS) { e = WIFI_E_REFUSED; goto fail; }
    if (s->r.assoc_done(s->r.ctx, &s->cur, s->aid) != 0) { e = WIFI_E_RADIO; goto fail; }
    if (s->sec != WIFI_SEC_OPEN) {
        uint8_t own[32];
        uint32_t ol = 0;
        s->keys_done = s->ptk_set = 0;
        if (wifi_build_rsn(s->sec, own, sizeof own, &ol) != WIFI_OK ||
            wpa_supp_start(&s->wpa, &s->tbl, s->sec, s->pmk, s->cur.bssid, s->mac, own, (uint8_t)ol, s->cur.rsne,
                           s->cur.rsne_len) != WPA_OK) {
            e = WIFI_E_CRYPTO;
            goto fail;
        }
        s->st = WIFI_STA_KEYS;
        if (s->early_len) {
            eapol_rx(s, s->early_eapol, s->early_len);
            aienos_wipe(s->early_eapol, sizeof s->early_eapol);
            s->early_len = 0;
        }
        if ((e = wait_flag(s, &s->keys_done, budget_us)) != WIFI_OK) goto fail;
        if (s->keys_done != 1) { e = WIFI_E_RADIO; goto fail; }
    }
    s->st = WIFI_STA_RUN;
    return WIFI_OK;
fail:
    /* After a successful association the AP holds state for us: tell it.
     * A handshake that ran out of time is reason 15 (12.7.6.1). */
    if (s->st == WIFI_STA_KEYS || (s->st == WIFI_STA_ASSOC && s->assoc_done && s->last_status == WIFI_STATUS_SUCCESS)) {
        (void)leave(s, s->st == WIFI_STA_KEYS && e == WIFI_E_TIMEOUT ? WIFI_REASON_4WAY_TIMEOUT : WIFI_REASON_UNSPECIFIED);
        return e;
    }
    s->st = WIFI_STA_IDLE;
    wpa_supp_wipe(&s->wpa);
    s->ptk_set = 0;
    return e;
}

wifi_err wifi_sta_send_eth(wifi_sta *s, const uint8_t *eth, uint32_t len) {
    uint8_t f[WIFI_HDR_MGMT + 8u + WIFI_STA_ETH_MAX];
    uint32_t n = 0;
    if (!s || !eth) return WIFI_E_ARG;
    if (s->st != WIFI_STA_RUN) return WIFI_E_STATE;
    if (len > WIFI_STA_ETH_MAX) return WIFI_E_SPACE;
    wifi_err e = wifi_eth_to_80211(eth, len, s->cur.bssid, (uint16_t)(s->seq << 4), f, sizeof f, &n);
    if (e != WIFI_OK) return e;
    int protect = s->sec != WIFI_SEC_OPEN;
    if (protect) f[1] |= WIFI_FL_PROTECTED;
    if (s->r.tx(s->r.ctx, f, n, protect) != 0) return WIFI_E_RADIO;
    s->seq = (uint16_t)((s->seq + 1u) & 0xfffu);
    return WIFI_OK;
}

int wifi_sta_recv_eth(wifi_sta *s, uint8_t *out, uint32_t cap, uint32_t *len) {
    if (!s || !out || !len || !s->rxq_n) return 0;
    uint32_t i = s->rxq_head, n = s->rxq_len[i];
    s->rxq_head = (s->rxq_head + 1u) % WIFI_STA_RXQ;
    s->rxq_n--;
    if (n > cap) {
        s->rx_dropped++;
        return 0;
    }
    cpy(out, s->rxq[i], n);
    *len = n;
    return 1;
}

wifi_err wifi_sta_disconnect(wifi_sta *s) {
    if (!s) return WIFI_E_ARG;
    if (s->st == WIFI_STA_IDLE) return WIFI_OK;
    s->keys_done = 0;
    return leave(s, WIFI_REASON_LEAVING);
}
