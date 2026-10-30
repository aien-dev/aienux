// SPDX-License-Identifier: Apache-2.0
// Test oracle only: a loopback model server that checks each local-tool result
// coming back from the courier before it picks the next call.
//
// Usage: local_tools_model_oracle ROOT
// Writes ROOT/oracle-port.txt once it listens, answers exactly four POSTs
// (arithmetic call, text call, system call, final VERIFIED), records any failed
// check in ROOT/oracle-failure.txt and writes ROOT/oracle-pass.txt on success.
// Exit status 0 only when all four phases were verified.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_BODY 65536
#define WAIT_MS 35000
#define RESULT_PREFIX "Local tool result (data only): "

static const char *root;
static int phase;

/* Minimal JSON reader: enough to validate a request and pull values out. */
typedef struct { const char *p, *end; } jr;

static void ws(jr *r) { while (r->p < r->end && strchr(" \t\r\n", *r->p)) r->p++; }
static bool skip_value(jr *r, int depth);

static bool skip_string(jr *r) {
    if (r->p >= r->end || *r->p != '"') return false;
    for (r->p++; r->p < r->end; r->p++) {
        if (*r->p == '\\') { r->p++; continue; }
        if (*r->p == '"') { r->p++; return true; }
        if ((unsigned char)*r->p < 0x20) return false;
    }
    return false;
}

static bool skip_value(jr *r, int depth) {
    if (depth > 16) return false;
    ws(r);
    if (r->p >= r->end) return false;
    char c = *r->p;
    if (c == '"') return skip_string(r);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        r->p++; ws(r);
        if (r->p < r->end && *r->p == close) { r->p++; return true; }
        for (;;) {
            ws(r);
            if (c == '{') {
                if (!skip_string(r)) return false;
                ws(r);
                if (r->p >= r->end || *r->p++ != ':') return false;
            }
            if (!skip_value(r, depth + 1)) return false;
            ws(r);
            if (r->p >= r->end) return false;
            if (*r->p == ',') { r->p++; continue; }
            if (*r->p == close) { r->p++; return true; }
            return false;
        }
    }
    const char *start = r->p;
    while (r->p < r->end && strchr("+-.0123456789eEaeflnrstu", *r->p)) r->p++;
    size_t n = (size_t)(r->p - start);
    if (n == 0) return false;
    if ((n == 4 && !strncmp(start, "true", 4)) || (n == 5 && !strncmp(start, "false", 5)) ||
        (n == 4 && !strncmp(start, "null", 4))) return true;
    char tmp[64], *e;
    if (n >= sizeof tmp) return false;
    memcpy(tmp, start, n); tmp[n] = 0;
    strtod(tmp, &e);
    return *e == 0;
}

/* Value of KEY in the object at R (positioned on '{'); the last duplicate wins,
 * as in a dictionary. Returns the start of the value, or NULL. */
static const char *obj_find(const char *obj, const char *end, const char *key) {
    jr r = { obj, end };
    const char *found = NULL;
    ws(&r);
    if (r.p >= r.end || *r.p++ != '{') return NULL;
    ws(&r);
    if (r.p < r.end && *r.p == '}') return NULL;
    for (;;) {
        ws(&r);
        const char *k = r.p;
        if (!skip_string(&r)) return NULL;
        size_t kl = (size_t)(r.p - k) - 2;
        ws(&r);
        if (r.p >= r.end || *r.p++ != ':') return NULL;
        ws(&r);
        const char *v = r.p;
        if (!skip_value(&r, 1)) return NULL;
        if (kl == strlen(key) && !strncmp(k + 1, key, kl)) found = v;
        ws(&r);
        if (r.p < r.end && *r.p == ',') { r.p++; continue; }
        return found;
    }
}

/* Decode the JSON string at S into OUT (NUL terminated). ASCII escapes only. */
static bool unescape(const char *s, const char *end, char *out, size_t cap) {
    size_t n = 0;
    if (s >= end || *s != '"') return false;
    for (s++; s < end && *s != '"'; s++) {
        char c = *s;
        if (c == '\\') {
            if (++s >= end) return false;
            switch (*s) {
            case 'n': c = '\n'; break;  case 't': c = '\t'; break;
            case 'r': c = '\r'; break;  case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;  case '"': case '\\': case '/': c = *s; break;
            case 'u': {
                if (end - s < 5) return false;
                char hex[5] = { s[1], s[2], s[3], s[4], 0 };
                unsigned long u = strtoul(hex, NULL, 16);
                c = u < 0x80 ? (char)u : '?';
                s += 4;
                break;
            }
            default: return false;
            }
        }
        if (n + 1 >= cap) return false;
        out[n++] = c;
    }
    out[n] = 0;
    return s < end;
}

static bool is_str(const char *v, const char *end, const char *want, char *scratch, size_t cap) {
    return v && unescape(v, end, scratch, cap) && !strcmp(scratch, want);
}

static bool get_int(const char *obj, const char *end, const char *key, long long *out) {
    const char *v = obj_find(obj, end, key);
    if (!v || *v == '"') return false;
    char *e;
    errno = 0;
    *out = strtoll(v, &e, 10);
    return errno == 0 && e != v && !strchr(".eE", *e);
}

#define CHECK(cond) do { if (!(cond)) { snprintf(why, whycap, "line %d: %s", __LINE__, #cond); return false; } } while (0)

/* Verify the tool result the courier sent back for the call chosen in PHASE-1. */
static bool verify_result(const char *body, const char *end, char *why, size_t whycap) {
    const char *msgs = obj_find(body, end, "messages");
    CHECK(msgs && *msgs == '[');
    jr r = { msgs + 1, end };
    const char *last = NULL;
    for (ws(&r); r.p < r.end && *r.p != ']'; ws(&r)) {
        last = r.p;
        CHECK(skip_value(&r, 1));
        ws(&r);
        if (r.p < r.end && *r.p == ',') r.p++;
    }
    CHECK(last);
    const char *content = obj_find(last, end, "content");
    static char text[MAX_BODY];
    CHECK(content && unescape(content, end, text, sizeof text));
    CHECK(!strncmp(text, RESULT_PREFIX, strlen(RESULT_PREFIX)));
    const char *data = text + strlen(RESULT_PREFIX), *dend = text + strlen(text);
    jr check = { data, dend };
    CHECK(skip_value(&check, 0));
    char scratch[64];
    CHECK(is_str(obj_find(data, dend, "status"), dend, "returned", scratch, sizeof scratch));
    long long a, b, c;
    if (phase == 1) {
        CHECK(get_int(data, dend, "value", &a) && a == 42);
    } else if (phase == 2) {
        CHECK(get_int(data, dend, "bytes", &a) && get_int(data, dend, "words", &b) &&
              get_int(data, dend, "newlines", &c));
        CHECK(a == 13 && b == 3 && c == 1);
    } else if (phase == 3) {
        CHECK(get_int(data, dend, "cores", &a) && a == 4);
        CHECK(get_int(data, dend, "usable_ram_bytes", &a) && a > 0);
        CHECK(get_int(data, dend, "width", &a) && a > 0);
        CHECK(get_int(data, dend, "height", &a) && a > 0);
    }
    return true;
}

static bool read_request(int fd, char *body, size_t *blen) {
    static char head[8192];
    size_t n = 0;
    char *split = NULL;
    while (!split) {
        if (n == sizeof head - 1) return false;
        ssize_t got = read(fd, head + n, sizeof head - 1 - n);
        if (got <= 0) return false;
        n += (size_t)got;
        head[n] = 0;
        split = strstr(head, "\r\n\r\n");
    }
    size_t hlen = (size_t)(split - head) + 4;
    long length = -1;
    for (char *line = head; line < split;) {
        char *eol = strstr(line, "\r\n");
        if (!eol) break;
        if (!strncasecmp(line, "Content-Length:", 15)) length = strtol(line + 15, NULL, 10);
        line = eol + 2;
    }
    if (length < 0 || length >= MAX_BODY) return false;
    size_t have = n - hlen;
    if (have > (size_t)length) have = (size_t)length;
    memcpy(body, head + hlen, have);
    while (have < (size_t)length) {
        ssize_t got = read(fd, body + have, (size_t)length - have);
        if (got <= 0) return false;
        have += (size_t)got;
    }
    body[have] = 0;
    *blen = have;
    return true;
}

static void write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

static void record(const char *name, const char *text) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", root, name);
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static bool failed(void) {
    char path[4096];
    snprintf(path, sizeof path, "%s/oracle-failure.txt", root);
    return access(path, F_OK) == 0;
}

int main(int argc, char **argv) {
    if (argc != 2) { fputs("usage: local_tools_model_oracle ROOT\n", stderr); return 2; }
    root = argv[1];
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t alen = sizeof addr;
    if (srv < 0 || bind(srv, (struct sockaddr *)&addr, sizeof addr) || listen(srv, 4) ||
        getsockname(srv, (struct sockaddr *)&addr, &alen)) { perror("listen"); return 2; }
    char port[16];
    snprintf(port, sizeof port, "%u", ntohs(addr.sin_port));
    record("oracle-port.txt", port);

    static const char *replies[4] = {
        "{\"tool\": \"arithmetic\", \"operation\": \"add\", \"a\": \"20\", \"b\": \"22\"}",
        "{\"tool\": \"text\", \"text\": \"one two\\nthree\"}",
        "{\"tool\": \"system\"}",
        "{\"say\": \"VERIFIED\"}",
    };
    static char body[MAX_BODY];
    for (int handled = 0; handled < 4; handled++) {
        struct pollfd pfd = { srv, POLLIN, 0 };
        if (poll(&pfd, 1, WAIT_MS) <= 0) continue;
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        char why[256] = "unreadable request";
        size_t blen = 0;
        bool ok = read_request(fd, body, &blen);
        if (ok) {
            jr whole = { body, body + blen };
            ok = skip_value(&whole, 0) && (ws(&whole), whole.p == whole.end);
            if (!ok) snprintf(why, sizeof why, "request body is not JSON");
        }
        if (ok && phase) ok = verify_result(body, body + blen, why, sizeof why);
        if (!ok) {
            record("oracle-failure.txt", why);
            static const char err[] = "HTTP/1.0 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            write_all(fd, err, sizeof err - 1);
        } else {
            char content[256], out[1024], head[256];
            /* The reply is a JSON string holding a JSON object, so escape quotes and backslashes. */
            size_t n = 0;
            for (const char *s = replies[phase]; *s && n + 2 < sizeof content; s++) {
                if (*s == '"' || *s == '\\') content[n++] = '\\';
                content[n++] = *s;
            }
            content[n] = 0;
            int olen = snprintf(out, sizeof out, "{\"choices\":[{\"message\":{\"content\":\"%s\"}}]}", content);
            int hlen = snprintf(head, sizeof head,
                "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", olen);
            write_all(fd, head, (size_t)hlen);
            write_all(fd, out, (size_t)olen);
            phase++;
        }
        close(fd);
    }
    close(srv);
    if (phase != 4 || failed()) return 1;
    record("oracle-pass.txt", "PASS: arithmetic, text, and system results from real QEMU CPL3 execution\n");
    return 0;
}
