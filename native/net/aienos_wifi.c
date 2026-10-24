/* IEEE 802.11 station-side frame layer. See aienos_wifi.h. */
#include "aienos_wifi.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void cpy(uint8_t *d, const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
static int eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static const uint8_t OUI_IEEE[3] = {0x00, 0x0f, 0xac};
static const uint8_t SNAP[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
/* 1, 2, 5.5, 11 Mb/s basic; 6, 9, 12, 18 Mb/s; extended 24, 36, 48, 54 Mb/s (units of 500 kb/s). */
static const uint8_t OUR_RATES[8] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
static const uint8_t OUR_EXT_RATES[4] = {0x30, 0x48, 0x60, 0x6c};

wifi_err wifi_parse_hdr(const uint8_t *f, uint32_t len, wifi_hdr *h) {
    if (!f || !h) return WIFI_E_ARG;
    if (len < WIFI_HDR_MGMT) return WIFI_E_SHORT;
    h->type = (uint8_t)((f[0] >> 2) & 3u);
    h->subtype = (uint8_t)(f[0] >> 4);
    h->flags = f[1];
    if ((f[0] & 3u) != 0) return WIFI_E_UNSUPPORTED; /* protocol version must be 0 */
    if (h->type == WIFI_TYPE_CTRL) return WIFI_E_UNSUPPORTED;
    h->duration = le16(f + 2);
    cpy(h->a1, f + 4, 6);
    cpy(h->a2, f + 10, 6);
    cpy(h->a3, f + 16, 6);
    h->seq_ctrl = le16(f + 22);
    h->qos = 0;
    h->hdr_len = WIFI_HDR_MGMT;
    if (h->type == WIFI_TYPE_DATA) {
        if ((h->flags & (WIFI_FL_TODS | WIFI_FL_FROMDS)) == (WIFI_FL_TODS | WIFI_FL_FROMDS))
            return WIFI_E_UNSUPPORTED; /* 4-address (mesh/WDS) */
        if (h->subtype & 0x8u) {
            if (len < WIFI_HDR_MGMT + 2u) return WIFI_E_SHORT;
            h->qos = le16(f + 24);
            h->hdr_len += 2;
        }
    }
    h->body = f + h->hdr_len;
    h->body_len = len - h->hdr_len;
    return WIFI_OK;
}

wifi_err wifi_check_elements(const uint8_t *ies, uint32_t len) {
    uint32_t i = 0;
    if (!ies && len) return WIFI_E_ARG;
    while (i < len) {
        if (len - i < 2u || ies[i + 1] > len - i - 2u) return WIFI_E_ELEMENT;
        i += 2u + ies[i + 1];
    }
    return WIFI_OK;
}

wifi_err wifi_find_element(const uint8_t *ies, uint32_t len, uint8_t eid, const uint8_t **body, uint8_t *blen) {
    wifi_err e = wifi_check_elements(ies, len);
    if (e != WIFI_OK) return e;
    for (uint32_t i = 0; i < len; i += 2u + ies[i + 1]) {
        if (ies[i] == eid) {
            if (body) *body = ies + i + 2;
            if (blen) *blen = ies[i + 1];
            return WIFI_OK;
        }
    }
    return WIFI_E_TYPE;
}

static uint32_t suite_bit(const uint8_t *s) {
    if (!eq(s, OUI_IEEE, 3) || s[3] >= 31u) return 1u << 31;
    return 1u << s[3];
}

wifi_err wifi_parse_rsn(const uint8_t *b, uint32_t len, wifi_rsn *r) {
    if (!b || !r) return WIFI_E_ARG;
    *r = (wifi_rsn){0};
    if (len < 2u) return WIFI_E_RSN;
    r->version = le16(b);
    if (r->version != 1u) return WIFI_E_RSN;
    r->present = 1;
    /* Defaults when fields are omitted (9.4.2.24.1): CCMP-128 group and pairwise, 802.1X AKM. */
    r->group = WIFI_CIPHER_CCMP128;
    r->pairwise = WIFI_CIPHER_CCMP128;
    r->akm = WIFI_AKM_8021X;
    uint32_t i = 2;
    if (len - i == 0) return WIFI_OK;
    if (len - i < 4u) return WIFI_E_RSN;
    r->group = suite_bit(b + i);
    i += 4;
    if (len - i == 0) return WIFI_OK;
    if (len - i < 2u) return WIFI_E_RSN;
    uint32_t n = le16(b + i);
    i += 2;
    if (n == 0 || n > (len - i) / 4u) return WIFI_E_RSN;
    r->pairwise = 0;
    for (uint32_t k = 0; k < n; k++, i += 4) r->pairwise |= suite_bit(b + i);
    if (len - i == 0) return WIFI_OK;
    if (len - i < 2u) return WIFI_E_RSN;
    n = le16(b + i);
    i += 2;
    if (n == 0 || n > (len - i) / 4u) return WIFI_E_RSN;
    r->akm = 0;
    for (uint32_t k = 0; k < n; k++, i += 4) r->akm |= suite_bit(b + i);
    if (len - i == 0) return WIFI_OK;
    if (len - i < 2u) return WIFI_E_RSN;
    r->caps = le16(b + i);
    i += 2;
    if (len - i == 0) return WIFI_OK;
    if (len - i < 2u) return WIFI_E_RSN;
    n = le16(b + i); /* PMKID count */
    i += 2;
    if (n > (len - i) / 16u) return WIFI_E_RSN;
    i += 16u * n;
    if (len - i == 0) return WIFI_OK;
    if (len - i < 4u) return WIFI_E_RSN;
    r->group_mgmt = suite_bit(b + i);
    i += 4;
    return len == i ? WIFI_OK : WIFI_E_RSN;
}

wifi_err wifi_build_rsn(wifi_sec sec, uint8_t *out, uint32_t cap, uint32_t *out_len) {
    if (!out || !out_len) return WIFI_E_ARG;
    uint8_t akm;
    uint16_t caps;
    switch (sec) {
    case WIFI_SEC_WPA2_PSK: akm = 2; caps = 0; break;
    case WIFI_SEC_WPA2_PSK_SHA256: akm = 6; caps = WIFI_RSN_CAP_MFPC; break;
    case WIFI_SEC_WPA3_SAE: akm = 8; caps = WIFI_RSN_CAP_MFPC | WIFI_RSN_CAP_MFPR; break;
    default: return WIFI_E_ARG;
    }
    uint32_t n = 2 + 2 + 4 + 2 + 4 + 2 + 4 + 2;
    if (cap < n) return WIFI_E_SPACE;
    uint8_t *p = out;
    *p++ = WIFI_EID_RSN;
    *p++ = (uint8_t)(n - 2);
    put16(p, 1); p += 2;
    cpy(p, OUI_IEEE, 3); p[3] = 4; p += 4;        /* group CCMP-128 */
    put16(p, 1); p += 2;
    cpy(p, OUI_IEEE, 3); p[3] = 4; p += 4;        /* pairwise CCMP-128 */
    put16(p, 1); p += 2;
    cpy(p, OUI_IEEE, 3); p[3] = akm; p += 4;
    put16(p, caps);
    *out_len = n;
    return WIFI_OK;
}

static void take_rates(wifi_bss *b, const uint8_t *r, uint8_t n) {
    for (uint8_t i = 0; i < n && b->n_rates < WIFI_RATES_MAX; i++) b->rates[b->n_rates++] = r[i];
}

wifi_err wifi_parse_beacon(const uint8_t *f, uint32_t len, wifi_bss *b) {
    wifi_hdr h;
    if (!b) return WIFI_E_ARG;
    wifi_err e = wifi_parse_hdr(f, len, &h);
    if (e != WIFI_OK) return e;
    if (h.type != WIFI_TYPE_MGMT || (h.subtype != WIFI_ST_BEACON && h.subtype != WIFI_ST_PROBE_RESP))
        return WIFI_E_TYPE;
    if (h.body_len < 12u) return WIFI_E_SHORT;
    *b = (wifi_bss){0};
    cpy(b->bssid, h.a3, 6);
    b->beacon_int = le16(h.body + 8);
    b->cap = le16(h.body + 10);
    const uint8_t *ies = h.body + 12, *v;
    uint32_t il = h.body_len - 12u;
    uint8_t vl;
    if ((e = wifi_check_elements(ies, il)) != WIFI_OK) return e;
    if (wifi_find_element(ies, il, WIFI_EID_SSID, &v, &vl) != WIFI_OK || vl > WIFI_SSID_MAX) return WIFI_E_ELEMENT;
    b->ssid_len = vl;
    cpy(b->ssid, v, vl);
    if (wifi_find_element(ies, il, WIFI_EID_RATES, &v, &vl) == WIFI_OK) take_rates(b, v, vl);
    if (wifi_find_element(ies, il, WIFI_EID_EXT_RATES, &v, &vl) == WIFI_OK) take_rates(b, v, vl);
    if (wifi_find_element(ies, il, WIFI_EID_DS, &v, &vl) == WIFI_OK) {
        if (vl != 1) return WIFI_E_ELEMENT;
        b->channel = v[0];
    }
    if (wifi_find_element(ies, il, WIFI_EID_HT_CAP, &v, &vl) == WIFI_OK) b->has_ht = 1;
    if (wifi_find_element(ies, il, WIFI_EID_HT_OP, &v, &vl) == WIFI_OK && vl >= 1 && !b->channel) b->channel = v[0];
    if (wifi_find_element(ies, il, WIFI_EID_RSN, &v, &vl) == WIFI_OK) {
        if (wifi_parse_rsn(v, vl, &b->rsn) != WIFI_OK) return WIFI_E_RSN;
        if (vl + 2u <= WIFI_RSNE_MAX) {
            b->rsne[0] = WIFI_EID_RSN;
            b->rsne[1] = vl;
            cpy(b->rsne + 2, v, vl);
            b->rsne_len = (uint8_t)(vl + 2u);
        }
    }
    return WIFI_OK;
}

wifi_sec wifi_choose_security(const wifi_bss *b) {
    if (!b) return WIFI_SEC_UNSUPPORTED;
    if (!b->rsn.present) return (b->cap & WIFI_CAP_PRIVACY) ? WIFI_SEC_UNSUPPORTED /* WEP or WPA1 */ : WIFI_SEC_OPEN;
    if (!(b->rsn.pairwise & WIFI_CIPHER_CCMP128)) return WIFI_SEC_UNSUPPORTED;
    if (b->rsn.group != WIFI_CIPHER_CCMP128 && b->rsn.group != WIFI_CIPHER_TKIP) return WIFI_SEC_UNSUPPORTED;
    int mfpc = (b->rsn.caps & WIFI_RSN_CAP_MFPC) != 0, mfpr = (b->rsn.caps & WIFI_RSN_CAP_MFPR) != 0;
    /* Protected management frames (IGTK, BIP, SA Query) do not exist here yet, and AKM 6 and SAE both need
     * them, so plain PSK wins whenever it is offered: a WPA2/WPA3 transition network (PSK + SAE, MFPC=1,
     * MFPR=0) is joined as WPA2-PSK without MFPC instead of being refused. */
    if (b->rsn.group == WIFI_CIPHER_CCMP128 && (b->rsn.akm & WIFI_AKM_PSK) && !mfpr) return WIFI_SEC_WPA2_PSK;
    if (b->rsn.akm & WIFI_AKM_SAE) return WIFI_SEC_WPA3_SAE;
    if (b->rsn.group != WIFI_CIPHER_CCMP128) return WIFI_SEC_UNSUPPORTED; /* TKIP group with CCMP pairwise: refuse */
    if ((b->rsn.akm & WIFI_AKM_PSK_SHA256) && mfpc) return WIFI_SEC_WPA2_PSK_SHA256;
    return WIFI_SEC_UNSUPPORTED;
}

void wifi_bss_table_init(wifi_bss_table *t) {
    if (t) t->n = 0;
}

void wifi_bss_table_update(wifi_bss_table *t, const wifi_bss *b) {
    if (!t || !b) return;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < t->n; i++)
        if (eq(t->e[i].bssid, b->bssid, 6)) {
            seen = t->e[i].seen;
            t->e[i] = *b;
            t->e[i].seen = seen + 1;
            return;
        }
    if (t->n < WIFI_BSS_MAX) {
        t->e[t->n] = *b;
        t->e[t->n++].seen = 1;
        return;
    }
    uint32_t w = 0;
    for (uint32_t i = 1; i < t->n; i++)
        if (t->e[i].rssi < t->e[w].rssi) w = i;
    if (b->rssi > t->e[w].rssi) {
        t->e[w] = *b;
        t->e[w].seen = 1;
    }
}

const wifi_bss *wifi_bss_select(const wifi_bss_table *t, const uint8_t *ssid, uint8_t ssid_len) {
    const wifi_bss *best = 0;
    if (!t || !ssid || ssid_len > WIFI_SSID_MAX) return 0;
    for (uint32_t i = 0; i < t->n; i++) {
        const wifi_bss *b = &t->e[i];
        if (b->ssid_len != ssid_len || !eq(b->ssid, ssid, ssid_len)) continue;
        if (wifi_choose_security(b) == WIFI_SEC_UNSUPPORTED) continue;
        if (!best || b->rssi > best->rssi) best = b;
    }
    return best;
}

static uint8_t *mgmt_hdr(uint8_t *out, uint8_t subtype, const uint8_t da[6], const uint8_t sa[6], const uint8_t bssid[6]) {
    out[0] = (uint8_t)(subtype << 4);
    out[1] = 0;
    put16(out + 2, 0);
    cpy(out + 4, da, 6);
    cpy(out + 10, sa, 6);
    cpy(out + 16, bssid, 6);
    put16(out + 22, 0); /* sequence numbers are assigned by the chip */
    return out + WIFI_HDR_MGMT;
}

static uint8_t *put_ie(uint8_t *p, uint8_t id, const uint8_t *body, uint8_t len) {
    p[0] = id;
    p[1] = len;
    cpy(p + 2, body, len);
    return p + 2 + len;
}

wifi_err wifi_build_probe_req(uint8_t *out, uint32_t cap, const uint8_t sa[6], const uint8_t *ssid, uint8_t ssid_len,
                              uint32_t *out_len) {
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (!out || !sa || !out_len || (ssid_len && !ssid) || ssid_len > WIFI_SSID_MAX) return WIFI_E_ARG;
    uint32_t n = WIFI_HDR_MGMT + 2u + ssid_len + 2u + sizeof(OUR_RATES) + 2u + sizeof(OUR_EXT_RATES);
    if (cap < n) return WIFI_E_SPACE;
    uint8_t *p = mgmt_hdr(out, WIFI_ST_PROBE_REQ, bcast, sa, bcast);
    p = put_ie(p, WIFI_EID_SSID, ssid, ssid_len);
    p = put_ie(p, WIFI_EID_RATES, OUR_RATES, sizeof(OUR_RATES));
    p = put_ie(p, WIFI_EID_EXT_RATES, OUR_EXT_RATES, sizeof(OUR_EXT_RATES));
    *out_len = (uint32_t)(p - out);
    return WIFI_OK;
}

wifi_err wifi_build_auth(uint8_t *out, uint32_t cap, const uint8_t bssid[6], const uint8_t sa[6], uint16_t alg,
                         uint16_t seq, uint16_t status, const uint8_t *extra, uint32_t extra_len, uint32_t *out_len) {
    if (!out || !bssid || !sa || !out_len || (extra_len && !extra)) return WIFI_E_ARG;
    if ((uint64_t)WIFI_HDR_MGMT + 6u + extra_len > cap) return WIFI_E_SPACE;
    uint8_t *p = mgmt_hdr(out, WIFI_ST_AUTH, bssid, sa, bssid);
    put16(p, alg);
    put16(p + 2, seq);
    put16(p + 4, status);
    cpy(p + 6, extra, extra_len);
    *out_len = WIFI_HDR_MGMT + 6u + extra_len;
    return WIFI_OK;
}

wifi_err wifi_parse_auth(const uint8_t *f, uint32_t len, uint16_t *alg, uint16_t *seq, uint16_t *status,
                         const uint8_t **extra, uint32_t *extra_len) {
    wifi_hdr h;
    wifi_err e = wifi_parse_hdr(f, len, &h);
    if (e != WIFI_OK) return e;
    if (h.type != WIFI_TYPE_MGMT || h.subtype != WIFI_ST_AUTH) return WIFI_E_TYPE;
    if (h.body_len < 6u) return WIFI_E_SHORT;
    if (alg) *alg = le16(h.body);
    if (seq) *seq = le16(h.body + 2);
    if (status) *status = le16(h.body + 4);
    if (extra) *extra = h.body + 6;
    if (extra_len) *extra_len = h.body_len - 6u;
    return WIFI_OK;
}

wifi_err wifi_build_assoc_req(uint8_t *out, uint32_t cap, const wifi_bss *b, const uint8_t sa[6], wifi_sec sec,
                              uint16_t listen_interval, uint32_t *out_len) {
    if (!out || !b || !sa || !out_len || b->ssid_len > WIFI_SSID_MAX) return WIFI_E_ARG;
    uint8_t rsn[32];
    uint32_t rl = 0;
    if (sec != WIFI_SEC_OPEN && wifi_build_rsn(sec, rsn, sizeof(rsn), &rl) != WIFI_OK) return WIFI_E_ARG;
    uint32_t n = WIFI_HDR_MGMT + 4u + 2u + b->ssid_len + 2u + sizeof(OUR_RATES) + 2u + sizeof(OUR_EXT_RATES) + rl;
    if (cap < n) return WIFI_E_SPACE;
    uint8_t *p = mgmt_hdr(out, WIFI_ST_ASSOC_REQ, b->bssid, sa, b->bssid);
    put16(p, (uint16_t)(WIFI_CAP_ESS | WIFI_CAP_SHORT_SLOT | (sec != WIFI_SEC_OPEN ? WIFI_CAP_PRIVACY : 0)));
    put16(p + 2, listen_interval);
    p += 4;
    p = put_ie(p, WIFI_EID_SSID, b->ssid, b->ssid_len);
    p = put_ie(p, WIFI_EID_RATES, OUR_RATES, sizeof(OUR_RATES));
    p = put_ie(p, WIFI_EID_EXT_RATES, OUR_EXT_RATES, sizeof(OUR_EXT_RATES));
    cpy(p, rsn, rl);
    p += rl;
    *out_len = (uint32_t)(p - out);
    return WIFI_OK;
}

wifi_err wifi_parse_assoc_resp(const uint8_t *f, uint32_t len, uint16_t *cap_info, uint16_t *status, uint16_t *aid) {
    wifi_hdr h;
    wifi_err e = wifi_parse_hdr(f, len, &h);
    if (e != WIFI_OK) return e;
    if (h.type != WIFI_TYPE_MGMT || h.subtype != WIFI_ST_ASSOC_RESP) return WIFI_E_TYPE;
    if (h.body_len < 6u) return WIFI_E_SHORT;
    if ((e = wifi_check_elements(h.body + 6, h.body_len - 6u)) != WIFI_OK) return e;
    if (cap_info) *cap_info = le16(h.body);
    if (status) *status = le16(h.body + 2);
    if (aid) *aid = (uint16_t)(le16(h.body + 4) & 0x3fffu); /* two top bits are set by convention */
    return WIFI_OK;
}

wifi_err wifi_build_deauth(uint8_t *out, uint32_t cap, const uint8_t da[6], const uint8_t sa[6], const uint8_t bssid[6],
                           uint16_t reason, uint32_t *out_len) {
    if (!out || !da || !sa || !bssid || !out_len) return WIFI_E_ARG;
    if (cap < WIFI_HDR_MGMT + 2u) return WIFI_E_SPACE;
    put16(mgmt_hdr(out, WIFI_ST_DEAUTH, da, sa, bssid), reason);
    *out_len = WIFI_HDR_MGMT + 2u;
    return WIFI_OK;
}

wifi_err wifi_eth_to_80211(const uint8_t *eth, uint32_t eth_len, const uint8_t bssid[6], uint16_t seq_ctrl, uint8_t *out,
                           uint32_t cap, uint32_t *out_len) {
    if (!eth || !bssid || !out || !out_len) return WIFI_E_ARG;
    if (eth_len < 14u) return WIFI_E_SHORT;
    uint16_t type = (uint16_t)((eth[12] << 8) | eth[13]);
    if (type < 0x0600u) return WIFI_E_UNSUPPORTED; /* 802.3 length field, not Ethernet II */
    uint32_t n = WIFI_HDR_MGMT + 8u + (eth_len - 14u);
    if (n > cap || n > WIFI_HDR_MGMT + WIFI_FRAME_MAX) return WIFI_E_SPACE;
    out[0] = (uint8_t)(WIFI_TYPE_DATA << 2);
    out[1] = WIFI_FL_TODS;
    put16(out + 2, 0);
    cpy(out + 4, bssid, 6);   /* RA = BSSID */
    cpy(out + 10, eth + 6, 6); /* TA = SA */
    cpy(out + 16, eth, 6);    /* DA */
    put16(out + 22, seq_ctrl);
    cpy(out + 24, SNAP, 6);
    out[30] = eth[12];
    out[31] = eth[13];
    cpy(out + 32, eth + 14, eth_len - 14u);
    *out_len = n;
    return WIFI_OK;
}

wifi_err wifi_80211_to_eth(const uint8_t *f, uint32_t len, const uint8_t bssid[6], int hw_decrypted, uint8_t *out,
                           uint32_t cap, uint32_t *out_len) {
    wifi_hdr h;
    if (!bssid || !out || !out_len) return WIFI_E_ARG;
    wifi_err e = wifi_parse_hdr(f, len, &h);
    if (e != WIFI_OK) return e;
    if (h.type != WIFI_TYPE_DATA || (h.subtype != WIFI_ST_DATA && h.subtype != WIFI_ST_QOS_DATA)) return WIFI_E_TYPE;
    if ((h.flags & (WIFI_FL_TODS | WIFI_FL_FROMDS)) != WIFI_FL_FROMDS) return WIFI_E_UNSUPPORTED;
    if (!eq(h.a2, bssid, 6)) return WIFI_E_TYPE; /* not from our AP */
    if ((h.flags & WIFI_FL_PROTECTED) && !hw_decrypted) return WIFI_E_PROTECTED;
    if (h.flags & WIFI_FL_MOREFRAG || (h.seq_ctrl & 0xfu)) return WIFI_E_UNSUPPORTED; /* fragments */
    if (h.subtype == WIFI_ST_QOS_DATA && (h.qos & 0x80u)) return WIFI_E_UNSUPPORTED; /* A-MSDU */
    if (h.body_len < 8u) return WIFI_E_SHORT;
    if (!eq(h.body, SNAP, 6)) return WIFI_E_UNSUPPORTED;
    uint32_t pl = h.body_len - 8u, n = 14u + pl;
    if (n > cap) return WIFI_E_SPACE;
    cpy(out, h.a1, 6);
    cpy(out + 6, h.a3, 6);
    out[12] = h.body[6];
    out[13] = h.body[7];
    cpy(out + 14, h.body + 8, pl);
    *out_len = n;
    return WIFI_OK;
}

const char *wifi_strerror(wifi_err e) {
    switch (e) {
    case WIFI_OK: return "ok";
    case WIFI_E_ARG: return "bad argument";
    case WIFI_E_SHORT: return "frame too short";
    case WIFI_E_ELEMENT: return "malformed element";
    case WIFI_E_TYPE: return "wrong frame type";
    case WIFI_E_RSN: return "malformed RSN element";
    case WIFI_E_SPACE: return "output too small";
    case WIFI_E_UNSUPPORTED: return "unsupported frame";
    case WIFI_E_PROTECTED: return "frame still encrypted";
    case WIFI_E_CRYPTO: return "security primitive missing";
    case WIFI_E_TIMEOUT: return "no answer from the access point";
    case WIFI_E_REFUSED: return "access point refused";
    case WIFI_E_RADIO: return "radio driver refused";
    case WIFI_E_STATE: return "not associated";
    case WIFI_E_NOT_FOUND: return "network not found";
    }
    return "?";
}
