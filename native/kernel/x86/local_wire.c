/* A closed, bounded data protocol over the existing untrusted courier.
 * CALL request invocation tool total offset hex (chunks <=96 bytes, '-' empty).
 * POLL request invocation and CANCEL request invocation require a live request.
 * RESULT request invocation state reason hex. Only the three build-owned tools
 * are reachable. No address, path, executable or resource limit comes off-wire. */
#include "local_wire.h"
#include "local_tools.h"
#include "../core/ck_internal.h"
static uint32_t current, assembling, tool, total, received;
static uint8_t bytes[CK_LOCAL_INPUT];
static const struct ck_local_result *pending;

static int number(const char **p, uint32_t *out)
{
    const char *s = *p;
    if (*s < '0' || *s > '9') return 0;
    uint32_t v = 0;
    do {
        unsigned d = (unsigned)(*s++ - '0');
        if (v > (UINT32_MAX - d) / 10) return 0;
        v = v * 10 + d;
    } while (*s >= '0' && *s <= '9');
    if (*s && *s != ' ') return 0;
    *p = *s ? s + 1 : s; *out = v;
    return 1;
}
static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
void ck_local_wire_begin(uint32_t request)
{
    ck_local_begin(request);
    current = request; assembling = received = total = tool = 0; pending = 0;
    memset(bytes, 0, sizeof bytes);
}
int ck_local_wire_line(const char *line)
{
    int call = strlen(line) >= 5 && !memcmp(line, "CALL ", 5);
    int poll = strlen(line) >= 5 && !memcmp(line, "POLL ", 5);
    int cancel = strlen(line) >= 7 && !memcmp(line, "CANCEL ", 7);
    if (!call && !poll && !cancel) return 0;
    const char *p = line + (cancel ? 7 : 5);
    uint32_t req, inv, id, n, off;
    if (!number(&p, &req) || !number(&p, &inv) || !current || req != current || !inv) return 1;
    if (!call) {
        if (*p) return 1;
        const struct ck_local_result *r = ck_local_status(req, inv);
        if (r) {
            if (cancel && r->state == CK_LOCAL_RUNNING) ck_local_cancel();
            pending = r;
        }
        return 1;
    }
    if (!number(&p, &id) || !number(&p, &n) || !number(&p, &off) || id < 1 || id > 3 ||
        n > sizeof bytes || off > n) return 1;
    if (pending && pending->state == CK_LOCAL_RUNNING && pending->invocation != inv) return 1;
    uint8_t chunk[96]; unsigned count = 0;
    if (*p == '-' && !p[1]) p++;
    while (*p) {
        if (!p[1] || count == sizeof chunk) return 1;
        int a = nibble(p[0]), b = nibble(p[1]);
        if (a < 0 || b < 0) return 1;
        chunk[count++] = (uint8_t)((a << 4) | b); p += 2;
    }
    if (count > n - off || (!count && n)) return 1;
    if (!assembling) {
        if (off) return 1;
        assembling = inv; tool = id; total = n; received = 0;
    }
    if (inv != assembling || tool != id || total != n || off > received) return 1;
    if (off < received) { /* Retransmitted chunks must be byte-for-byte identical. */
        if (count > received - off || memcmp(bytes + off, chunk, count)) return 1;
    } else {
        memcpy(bytes + off, chunk, count); received += count;
    }
    if (received == total) {
        pending = ck_local_start(req, inv, id, bytes, total);
        memset(bytes, 0, sizeof bytes); assembling = received = total = tool = 0;
    }
    return 1;
}
void ck_local_wire_step(const ck_courier *courier)
{
    if (!courier || !pending || pending->state == CK_LOCAL_RUNNING) return;
    /* Current tool schemas return 8, 24 or 40 bytes. Do not silently truncate
     * an unexpected output or turn it into a success. */
    unsigned state = pending->state, reason = pending->reason, len = pending->length;
    if (len > 40) { state = CK_LOCAL_ABNORMAL; reason = CK_LOCAL_BAD_RETURN; len = 0; }
    char line[192];
    int at = ck_snprintf(line, sizeof line, "RESULT %u %u %u %u ",
        (unsigned)pending->request, (unsigned)pending->invocation, state, reason);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < len; i++) {
        line[at++] = hex[pending->output[i] >> 4]; line[at++] = hex[pending->output[i] & 15];
    }
    if (!len) line[at++] = '-';
    line[at] = 0;
    if (!courier->send(line, (unsigned)at)) pending = 0;
}
