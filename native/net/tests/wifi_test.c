/* 802.11 frame layer tests: header, elements, RSN, beacon, security choice,
 * BSS table, builders round-tripped through the parsers, 802.3 <-> 802.11,
 * every truncation and a seeded hostile-frame fuzz. */
#include <stdio.h>
#include <string.h>

#include "../aienos_wifi.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t AP[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t ME[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
static const uint8_t PEER[6] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};

/* RSNE bodies (without id/len). */
static const uint8_t RSN_PSK[] = {1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0};
static const uint8_t RSN_SAE[] = {1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 8, 0xc0, 0};
/* Transition mode: PSK + SAE, MFP capable. */
static const uint8_t RSN_MIXED[] = {1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 2, 0, 0, 0x0f, 0xac, 2,
                                    0, 0x0f, 0xac, 8, 0x80, 0};
static const uint8_t RSN_TKIP[] = {1, 0, 0, 0x0f, 0xac, 2, 1, 0, 0, 0x0f, 0xac, 2, 1, 0, 0, 0x0f, 0xac, 2, 0, 0};
static const uint8_t RSN_PSK_SHA256[] = {1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 6, 0x80, 0};
static const uint8_t RSN_FULL[] = {1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 8, 0xc0, 0,
                                   1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0, 0x0f, 0xac, 6};

static uint32_t beacon(uint8_t *f, const char *ssid, uint8_t chan, int privacy, const uint8_t *rsn, uint8_t rsn_len) {
    uint32_t n = 0;
    f[n++] = (uint8_t)(WIFI_ST_BEACON << 4);
    f[n++] = 0;
    f[n++] = 0; f[n++] = 0;
    memset(f + n, 0xff, 6); n += 6;
    memcpy(f + n, AP, 6); n += 6;
    memcpy(f + n, AP, 6); n += 6;
    f[n++] = 0x10; f[n++] = 0;
    memset(f + n, 0, 8); n += 8; /* timestamp */
    f[n++] = 100; f[n++] = 0;
    f[n++] = (uint8_t)(WIFI_CAP_ESS | (privacy ? WIFI_CAP_PRIVACY : 0)); f[n++] = 0;
    uint8_t sl = (uint8_t)strlen(ssid);
    f[n++] = WIFI_EID_SSID; f[n++] = sl; memcpy(f + n, ssid, sl); n += sl;
    f[n++] = WIFI_EID_RATES; f[n++] = 4; f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8b; f[n++] = 0x96;
    f[n++] = WIFI_EID_DS; f[n++] = 1; f[n++] = chan;
    if (rsn) { f[n++] = WIFI_EID_RSN; f[n++] = rsn_len; memcpy(f + n, rsn, rsn_len); n += rsn_len; }
    return n;
}

static void test_hdr(void) {
    uint8_t f[64] = {0};
    wifi_hdr h;
    CHECK(wifi_parse_hdr(0, 24, &h) == WIFI_E_ARG);
    CHECK(wifi_parse_hdr(f, 23, &h) == WIFI_E_SHORT);
    CHECK(wifi_parse_hdr(f, 24, &h) == WIFI_OK && h.type == WIFI_TYPE_MGMT && h.hdr_len == 24 && h.body_len == 0);
    f[0] = 0x01; /* protocol version 1 */
    CHECK(wifi_parse_hdr(f, 24, &h) == WIFI_E_UNSUPPORTED);
    f[0] = (uint8_t)(WIFI_TYPE_CTRL << 2);
    CHECK(wifi_parse_hdr(f, 24, &h) == WIFI_E_UNSUPPORTED);
    f[0] = (uint8_t)((WIFI_TYPE_DATA << 2) | (WIFI_ST_QOS_DATA << 4));
    CHECK(wifi_parse_hdr(f, 25, &h) == WIFI_E_SHORT);
    f[24] = 0x05; f[25] = 0x00;
    CHECK(wifi_parse_hdr(f, 30, &h) == WIFI_OK && h.hdr_len == 26 && h.qos == 5 && h.body_len == 4);
    f[1] = WIFI_FL_TODS | WIFI_FL_FROMDS;
    CHECK(wifi_parse_hdr(f, 30, &h) == WIFI_E_UNSUPPORTED);
}

static void test_elements(void) {
    const uint8_t ok[] = {0, 3, 'a', 'b', 'c', 1, 1, 0x82, 221, 0};
    const uint8_t *v;
    uint8_t vl;
    CHECK(wifi_check_elements(ok, sizeof ok) == WIFI_OK);
    CHECK(wifi_check_elements(0, 0) == WIFI_OK);
    CHECK(wifi_check_elements(0, 1) == WIFI_E_ARG);
    for (uint32_t n = 1; n < sizeof ok; n++)
        if (n != 5 && n != 8) CHECK(wifi_check_elements(ok, n) == WIFI_E_ELEMENT);
    CHECK(wifi_find_element(ok, sizeof ok, 1, &v, &vl) == WIFI_OK && vl == 1 && v[0] == 0x82);
    CHECK(wifi_find_element(ok, sizeof ok, 221, &v, &vl) == WIFI_OK && vl == 0);
    CHECK(wifi_find_element(ok, sizeof ok, 48, &v, &vl) == WIFI_E_TYPE);
    const uint8_t over[] = {0, 200, 'a'};
    CHECK(wifi_find_element(over, sizeof over, 0, &v, &vl) == WIFI_E_ELEMENT);
}

static void test_rsn(void) {
    wifi_rsn r;
    CHECK(wifi_parse_rsn(RSN_PSK, sizeof RSN_PSK, &r) == WIFI_OK && r.present && r.group == WIFI_CIPHER_CCMP128 &&
          r.pairwise == WIFI_CIPHER_CCMP128 && r.akm == WIFI_AKM_PSK && r.caps == 0);
    CHECK(wifi_parse_rsn(RSN_MIXED, sizeof RSN_MIXED, &r) == WIFI_OK && r.akm == (WIFI_AKM_PSK | WIFI_AKM_SAE) &&
          r.caps == WIFI_RSN_CAP_MFPC);
    CHECK(wifi_parse_rsn(RSN_FULL, sizeof RSN_FULL, &r) == WIFI_OK && r.group_mgmt == WIFI_CIPHER_BIP_CMAC128);
    /* Version-only RSNE takes the defaults. */
    CHECK(wifi_parse_rsn(RSN_PSK, 2, &r) == WIFI_OK && r.akm == WIFI_AKM_8021X && r.pairwise == WIFI_CIPHER_CCMP128);
    /* Every cut that does not land on a field boundary is malformed. */
    for (uint32_t n = 0; n <= sizeof RSN_FULL; n++) {
        int boundary = n == 2 || n == 6 || n == 12 || n == 18 || n == 20 || n == 38 || n == 42;
        wifi_err e = wifi_parse_rsn(RSN_FULL, n, &r);
        CHECK(boundary ? e == WIFI_OK : e == WIFI_E_RSN);
    }
    uint8_t bad[sizeof RSN_PSK];
    memcpy(bad, RSN_PSK, sizeof bad);
    bad[0] = 2;
    CHECK(wifi_parse_rsn(bad, sizeof bad, &r) == WIFI_E_RSN);
    memcpy(bad, RSN_PSK, sizeof bad);
    bad[6] = 0; /* zero pairwise suites */
    CHECK(wifi_parse_rsn(bad, sizeof bad, &r) == WIFI_E_RSN);
    memcpy(bad, RSN_PSK, sizeof bad);
    bad[6] = 0xff; /* count runs past the end */
    CHECK(wifi_parse_rsn(bad, sizeof bad, &r) == WIFI_E_RSN);
    memcpy(bad, RSN_PSK, sizeof bad);
    bad[15] = 0x50; /* AKM suite at 14..17: OUI 00-50-xx is not IEEE */
    CHECK(wifi_parse_rsn(bad, sizeof bad, &r) == WIFI_OK && r.akm == WIFI_AKM_OTHER);

    /* build -> parse round trip for every security we can request. */
    wifi_sec secs[] = {WIFI_SEC_WPA2_PSK, WIFI_SEC_WPA2_PSK_SHA256, WIFI_SEC_WPA3_SAE};
    uint32_t akms[] = {WIFI_AKM_PSK, WIFI_AKM_PSK_SHA256, WIFI_AKM_SAE};
    for (int i = 0; i < 3; i++) {
        uint8_t out[32];
        uint32_t ol = 0;
        CHECK(wifi_build_rsn(secs[i], out, sizeof out, &ol) == WIFI_OK && out[0] == WIFI_EID_RSN && out[1] == ol - 2);
        CHECK(wifi_parse_rsn(out + 2, out[1], &r) == WIFI_OK && r.akm == akms[i] && r.pairwise == WIFI_CIPHER_CCMP128);
        CHECK(wifi_build_rsn(secs[i], out, ol - 1, &ol) == WIFI_E_SPACE);
    }
    CHECK((r.caps & (WIFI_RSN_CAP_MFPC | WIFI_RSN_CAP_MFPR)) == (WIFI_RSN_CAP_MFPC | WIFI_RSN_CAP_MFPR));
    uint8_t out[32];
    uint32_t ol;
    CHECK(wifi_build_rsn(WIFI_SEC_OPEN, out, sizeof out, &ol) == WIFI_E_ARG);
    CHECK(wifi_build_rsn(WIFI_SEC_UNSUPPORTED, out, sizeof out, &ol) == WIFI_E_ARG);
}

static void test_beacon_and_security(void) {
    uint8_t f[256];
    wifi_bss b;
    uint32_t n = beacon(f, "spark-lab", 36, 1, RSN_SAE, sizeof RSN_SAE);
    CHECK(wifi_parse_beacon(f, n, &b) == WIFI_OK);
    CHECK(memcmp(b.bssid, AP, 6) == 0 && b.ssid_len == 9 && memcmp(b.ssid, "spark-lab", 9) == 0);
    CHECK(b.channel == 36 && b.beacon_int == 100 && b.n_rates == 4 && b.rsn.present);
    CHECK(wifi_choose_security(&b) == WIFI_SEC_WPA3_SAE);
    /* Every truncation fails cleanly. */
    for (uint32_t k = 0; k < n; k++) {
        wifi_err e = wifi_parse_beacon(f, k, &b);
        CHECK(e != WIFI_OK || k >= 36 + 2 + 9);
    }

    struct { const uint8_t *rsn; uint8_t len; int privacy; wifi_sec want; } cases[] = {
        {0, 0, 0, WIFI_SEC_OPEN},
        {0, 0, 1, WIFI_SEC_UNSUPPORTED}, /* WEP / WPA1 */
        {RSN_PSK, sizeof RSN_PSK, 1, WIFI_SEC_WPA2_PSK},
        {RSN_PSK_SHA256, sizeof RSN_PSK_SHA256, 1, WIFI_SEC_WPA2_PSK_SHA256},
        {RSN_MIXED, sizeof RSN_MIXED, 1, WIFI_SEC_WPA2_PSK}, /* transition network: PSK, no PMF to promise */
        {RSN_SAE, sizeof RSN_SAE, 1, WIFI_SEC_WPA3_SAE},
        {RSN_TKIP, sizeof RSN_TKIP, 1, WIFI_SEC_UNSUPPORTED},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        n = beacon(f, "net", 6, cases[i].privacy, cases[i].rsn, cases[i].len);
        CHECK(wifi_parse_beacon(f, n, &b) == WIFI_OK);
        CHECK(wifi_choose_security(&b) == cases[i].want);
    }
    /* Malformed RSN inside a beacon refuses the beacon. */
    n = beacon(f, "net", 6, 1, RSN_PSK, 5);
    CHECK(wifi_parse_beacon(f, n, &b) == WIFI_E_RSN);
    /* SSID longer than 32. */
    char longssid[40];
    memset(longssid, 'x', 33);
    longssid[33] = 0;
    n = beacon(f, longssid, 6, 0, 0, 0);
    CHECK(wifi_parse_beacon(f, n, &b) == WIFI_E_ELEMENT);
    /* Not a beacon. */
    n = beacon(f, "net", 6, 0, 0, 0);
    f[0] = (uint8_t)(WIFI_ST_AUTH << 4);
    CHECK(wifi_parse_beacon(f, n, &b) == WIFI_E_TYPE);
    CHECK(wifi_choose_security(0) == WIFI_SEC_UNSUPPORTED);
}

static void test_bss_table(void) {
    wifi_bss_table t;
    wifi_bss b = {0};
    wifi_bss_table_init(&t);
    CHECK(t.n == 0);
    memcpy(b.ssid, "home", 4);
    b.ssid_len = 4;
    for (int i = 0; i < 20; i++) {
        b.bssid[5] = (uint8_t)i;
        b.rssi = (int8_t)(-90 + i);
        wifi_bss_table_update(&t, &b);
    }
    CHECK(t.n == WIFI_BSS_MAX);
    int have_weakest = 0;
    for (uint32_t i = 0; i < t.n; i++) have_weakest |= t.e[i].rssi == -90;
    CHECK(!have_weakest); /* the four weakest were replaced */
    b.bssid[5] = 19;
    b.rssi = -10;
    wifi_bss_table_update(&t, &b);
    const wifi_bss *s = wifi_bss_select(&t, (const uint8_t *)"home", 4);
    CHECK(s && s->bssid[5] == 19 && s->rssi == -10 && s->seen == 2);
    CHECK(wifi_bss_select(&t, (const uint8_t *)"other", 5) == 0);
    /* An unsupported-security BSS is never chosen even if strongest. */
    b.bssid[5] = 99;
    b.rssi = 0;
    b.cap = WIFI_CAP_PRIVACY;
    wifi_bss_table_update(&t, &b);
    s = wifi_bss_select(&t, (const uint8_t *)"home", 4);
    CHECK(s && s->bssid[5] == 19);
}

static void test_mgmt_builders(void) {
    uint8_t f[512];
    uint32_t n;
    wifi_hdr h;
    const uint8_t *v;
    uint8_t vl;
    CHECK(wifi_build_probe_req(f, sizeof f, ME, (const uint8_t *)"lab", 3, &n) == WIFI_OK);
    CHECK(wifi_parse_hdr(f, n, &h) == WIFI_OK && h.subtype == WIFI_ST_PROBE_REQ && memcmp(h.a2, ME, 6) == 0);
    CHECK(wifi_find_element(h.body, h.body_len, WIFI_EID_SSID, &v, &vl) == WIFI_OK && vl == 3 && !memcmp(v, "lab", 3));
    CHECK(wifi_build_probe_req(f, n - 1, ME, (const uint8_t *)"lab", 3, &n) == WIFI_E_SPACE);
    CHECK(wifi_build_probe_req(f, sizeof f, ME, 0, 0, &n) == WIFI_OK); /* wildcard SSID */

    uint16_t alg, seq, st;
    const uint8_t *ex;
    uint32_t exl;
    const uint8_t commit[] = {19, 0, 1, 2, 3};
    CHECK(wifi_build_auth(f, sizeof f, AP, ME, WIFI_AUTH_SAE, 1, 0, commit, sizeof commit, &n) == WIFI_OK);
    CHECK(wifi_parse_auth(f, n, &alg, &seq, &st, &ex, &exl) == WIFI_OK && alg == WIFI_AUTH_SAE && seq == 1 && st == 0 &&
          exl == sizeof commit && !memcmp(ex, commit, exl));
    CHECK(wifi_parse_auth(f, 29, &alg, &seq, &st, &ex, &exl) == WIFI_E_SHORT);
    CHECK(wifi_build_auth(f, 29, AP, ME, 0, 1, 0, 0, 0, &n) == WIFI_E_SPACE);

    wifi_bss b = {0};
    memcpy(b.bssid, AP, 6);
    memcpy(b.ssid, "lab", 3);
    b.ssid_len = 3;
    CHECK(wifi_build_assoc_req(f, sizeof f, &b, ME, WIFI_SEC_WPA3_SAE, 10, &n) == WIFI_OK);
    CHECK(wifi_parse_hdr(f, n, &h) == WIFI_OK && h.subtype == WIFI_ST_ASSOC_REQ && !memcmp(h.a1, AP, 6));
    CHECK(wifi_check_elements(h.body + 4, h.body_len - 4) == WIFI_OK);
    wifi_rsn r;
    CHECK(wifi_find_element(h.body + 4, h.body_len - 4, WIFI_EID_RSN, &v, &vl) == WIFI_OK &&
          wifi_parse_rsn(v, vl, &r) == WIFI_OK && r.akm == WIFI_AKM_SAE);
    CHECK((h.body[0] | (h.body[1] << 8)) & WIFI_CAP_PRIVACY);
    CHECK(wifi_build_assoc_req(f, sizeof f, &b, ME, WIFI_SEC_OPEN, 10, &n) == WIFI_OK);
    CHECK(wifi_parse_hdr(f, n, &h) == WIFI_OK &&
          wifi_find_element(h.body + 4, h.body_len - 4, WIFI_EID_RSN, &v, &vl) == WIFI_E_TYPE);
    CHECK(wifi_build_assoc_req(f, sizeof f, &b, ME, WIFI_SEC_UNSUPPORTED, 10, &n) == WIFI_E_ARG);

    /* Association response. */
    uint8_t ar[40] = {0};
    ar[0] = (uint8_t)(WIFI_ST_ASSOC_RESP << 4);
    memcpy(ar + 4, ME, 6); memcpy(ar + 10, AP, 6); memcpy(ar + 16, AP, 6);
    ar[24] = 0x11; ar[26] = 0; ar[27] = 0; ar[28] = 0x05; ar[29] = 0xc0;
    ar[30] = 1; ar[31] = 1; ar[32] = 0x82;
    uint16_t cap, aid;
    CHECK(wifi_parse_assoc_resp(ar, 33, &cap, &st, &aid) == WIFI_OK && cap == 0x11 && st == 0 && aid == 5);
    CHECK(wifi_parse_assoc_resp(ar, 32, &cap, &st, &aid) == WIFI_E_ELEMENT);
    CHECK(wifi_parse_assoc_resp(ar, 29, &cap, &st, &aid) == WIFI_E_SHORT);

    CHECK(wifi_build_deauth(f, sizeof f, AP, ME, AP, WIFI_REASON_LEAVING, &n) == WIFI_OK && n == 26 && f[24] == 3);
    CHECK(wifi_build_deauth(f, 25, AP, ME, AP, 3, &n) == WIFI_E_SPACE);
}

static void test_data_conv(void) {
    uint8_t eth[64], w[128], back[128];
    uint32_t wl, bl;
    memcpy(eth, PEER, 6);
    memcpy(eth + 6, ME, 6);
    eth[12] = 0x08; eth[13] = 0x00;
    for (int i = 14; i < 60; i++) eth[i] = (uint8_t)i;
    CHECK(wifi_eth_to_80211(eth, 60, AP, 0x120, w, sizeof w, &wl) == WIFI_OK && wl == 24 + 8 + 46);
    wifi_hdr h;
    CHECK(wifi_parse_hdr(w, wl, &h) == WIFI_OK && h.type == WIFI_TYPE_DATA && h.flags == WIFI_FL_TODS);
    CHECK(!memcmp(h.a1, AP, 6) && !memcmp(h.a2, ME, 6) && !memcmp(h.a3, PEER, 6) && h.seq_ctrl == 0x120);
    CHECK(wifi_eth_to_80211(eth, 13, AP, 0, w, sizeof w, &wl) == WIFI_E_SHORT);
    CHECK(wifi_eth_to_80211(eth, 60, AP, 0, w, 77, &wl) == WIFI_E_SPACE);
    eth[12] = 0x00; eth[13] = 0x40; /* 802.3 length */
    CHECK(wifi_eth_to_80211(eth, 60, AP, 0, w, sizeof w, &wl) == WIFI_E_UNSUPPORTED);
    eth[12] = 0x08; eth[13] = 0x00;

    /* AP -> us: FromDS, a1 = us, a2 = BSSID, a3 = original source. */
    CHECK(wifi_eth_to_80211(eth, 60, AP, 0, w, sizeof w, &wl) == WIFI_OK);
    w[1] = WIFI_FL_FROMDS;
    memcpy(w + 4, ME, 6); memcpy(w + 10, AP, 6); memcpy(w + 16, PEER, 6);
    CHECK(wifi_80211_to_eth(w, wl, AP, 0, back, sizeof back, &bl) == WIFI_OK && bl == 60);
    CHECK(!memcmp(back, ME, 6) && !memcmp(back + 6, PEER, 6) && !memcmp(back + 12, eth + 12, 48));
    CHECK(wifi_80211_to_eth(w, wl, PEER, 0, back, sizeof back, &bl) == WIFI_E_TYPE); /* not our AP */
    CHECK(wifi_80211_to_eth(w, wl, AP, 0, back, 59, &bl) == WIFI_E_SPACE);
    for (uint32_t k = 0; k < 32; k++) CHECK(wifi_80211_to_eth(w, k, AP, 0, back, sizeof back, &bl) != WIFI_OK);
    w[1] = WIFI_FL_FROMDS | WIFI_FL_PROTECTED;
    CHECK(wifi_80211_to_eth(w, wl, AP, 0, back, sizeof back, &bl) == WIFI_E_PROTECTED);
    CHECK(wifi_80211_to_eth(w, wl, AP, 1, back, sizeof back, &bl) == WIFI_OK && bl == 60); /* chip decrypted */
    w[1] = WIFI_FL_FROMDS | WIFI_FL_MOREFRAG;
    CHECK(wifi_80211_to_eth(w, wl, AP, 0, back, sizeof back, &bl) == WIFI_E_UNSUPPORTED);
    w[1] = WIFI_FL_TODS;
    CHECK(wifi_80211_to_eth(w, wl, AP, 0, back, sizeof back, &bl) == WIFI_E_UNSUPPORTED);
    w[1] = WIFI_FL_FROMDS;
    w[24] = 0xab; /* not SNAP */
    CHECK(wifi_80211_to_eth(w, wl, AP, 0, back, sizeof back, &bl) == WIFI_E_UNSUPPORTED);
    w[24] = 0xaa;

    /* QoS data: two extra header bytes; A-MSDU refused. */
    uint8_t q[130];
    memcpy(q, w, 24);
    q[0] = (uint8_t)((WIFI_TYPE_DATA << 2) | (WIFI_ST_QOS_DATA << 4));
    q[24] = 0; q[25] = 0;
    memcpy(q + 26, w + 24, wl - 24);
    CHECK(wifi_80211_to_eth(q, wl + 2, AP, 0, back, sizeof back, &bl) == WIFI_OK && bl == 60);
    q[24] = 0x80;
    CHECK(wifi_80211_to_eth(q, wl + 2, AP, 0, back, sizeof back, &bl) == WIFI_E_UNSUPPORTED);
}

/* Hostile bytes: no parser may read out of bounds (run under ASan) or claim
 * success with views that fall outside the input. */
static void test_fuzz(void) {
    uint32_t s = 0x6a09e667u;
    uint8_t f[300], out[400];
    for (int it = 0; it < 20000; it++) {
        s = s * 1664525u + 1013904223u;
        uint32_t n = (s >> 8) % sizeof f;
        for (uint32_t i = 0; i < n; i++) {
            s = s * 1664525u + 1013904223u;
            f[i] = (uint8_t)(s >> 24);
        }
        if (it & 1) { f[0] = (uint8_t)(WIFI_ST_BEACON << 4); if (n > 1) f[1] = 0; }
        wifi_hdr h;
        wifi_bss b;
        wifi_rsn r;
        uint32_t ol;
        uint16_t a, c, d;
        const uint8_t *ex;
        if (wifi_parse_hdr(f, n, &h) == WIFI_OK) CHECK(h.body == f + h.hdr_len && h.hdr_len + h.body_len == n);
        if (wifi_parse_beacon(f, n, &b) == WIFI_OK) CHECK(b.ssid_len <= WIFI_SSID_MAX && b.n_rates <= WIFI_RATES_MAX);
        (void)wifi_parse_rsn(f, n, &r);
        (void)wifi_check_elements(f, n);
        (void)wifi_parse_auth(f, n, &a, &c, &d, &ex, &ol);
        (void)wifi_parse_assoc_resp(f, n, &a, &c, &d);
        if (wifi_80211_to_eth(f, n, f + 10, 0, out, sizeof out, &ol) == WIFI_OK) CHECK(ol + 10 + 8 <= n + 14);
    }
}

int main(void) {
    test_hdr();
    test_elements();
    test_rsn();
    test_beacon_and_security();
    test_bss_table();
    test_mgmt_builders();
    test_data_conv();
    test_fuzz();
    CHECK(wifi_strerror(WIFI_E_PROTECTED)[0] != '?');
    printf("wifi_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
