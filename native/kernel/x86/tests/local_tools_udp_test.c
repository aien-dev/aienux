// SPDX-License-Identifier: Apache-2.0
// UDP adapter oracle for the local-tools courier path, synthetic result bytes.
// Real CPL3 execution is covered separately in QEMU (scripts/qemu_ck_local_tools_test.sh).
//
// Usage: local_tools_udp_test ORACLE RELAY
//   ORACLE  the local_tools_model_oracle binary (loopback model server)
//   RELAY   the allen-courier relay binary
// Plays the iMac's side of the wire: sends HELLO and JOB frames, answers every
// CALL with a hand-made little-endian result, then runs three direct arithmetic
// jobs with the model server already gone.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t oracle_pid, relay_pid;
static char root[256];
static int sock, relay_port;
static const char run_id[] = "abc0000000000421";
static int under = 1;

typedef struct { int seq, part; } seen_key;
static seen_key seen[64];
static int nseen;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *w) {
    (void)st; (void)flag; (void)w;
    return remove(path);
}

static void cleanup(void) {
    if (oracle_pid > 0) { kill(oracle_pid, SIGTERM); waitpid(oracle_pid, NULL, 0); oracle_pid = 0; }
    if (relay_pid > 0) { kill(relay_pid, SIGTERM); waitpid(relay_pid, NULL, 0); relay_pid = 0; }
    if (root[0]) { nftw(root, rm_entry, 16, FTW_DEPTH | FTW_PHYS); root[0] = 0; }
}

static void die(const char *fmt, ...) {
    va_list ap;
    fputs("LOCAL_TOOLS_UDP: FAIL ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    cleanup();
    exit(1);
}

#define ASSERT(cond, ...) do { if (!(cond)) die(__VA_ARGS__); } while (0)

static void put_le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_le64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

static size_t from_hex(const char *s, uint8_t *out, size_t cap) {
    size_t n = strlen(s);
    ASSERT(n % 2 == 0 && n / 2 <= cap, "bad hex %s", s);
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        ASSERT(sscanf(s + 2 * i, "%2x", &v) == 1, "bad hex %s", s);
        out[i] = (uint8_t)v;
    }
    return n / 2;
}

static void to_hex(const uint8_t *p, size_t n, char *out) {
    if (!n) { strcpy(out, "-"); return; }
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]);
}

static void send_frame(const char *kind, int seq, int part, int total, const char *body) {
    char data[4096];
    int n = snprintf(data, sizeof data, "ALLEN-COURIER v=1 run=%s kind=%s seq=%d part=%d/%d bytes=%zu\n%s",
                     run_id, kind, seq, part, total, strlen(body), body);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)relay_port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    ASSERT(n > 0 && (size_t)n < sizeof data, "frame too large");
    ASSERT(sendto(sock, data, (size_t)n, 0, (struct sockaddr *)&to, sizeof to) == n, "sendto: %s", strerror(errno));
}

/* One LINE frame from the relay: acknowledges it and reports whether it is new.
 * Returns 0 on timeout, 1 for a fresh LINE, -1 for anything to skip. */
static int next_line(int *seq, char *body, size_t cap) {
    char packet[4097];
    ssize_t n = recv(sock, packet, sizeof packet - 1, 0);
    if (n < 0) { ASSERT(errno == EAGAIN || errno == EWOULDBLOCK, "recv: %s", strerror(errno)); return 0; }
    packet[n] = 0;
    char *nl = strchr(packet, '\n');
    ASSERT(nl, "frame has no header line: %s", packet);
    *nl = 0;
    char kind[32] = "";
    int s = -1, part = -1;
    char *save = NULL;
    for (char *tok = strtok_r(packet, " ", &save), *first = tok; tok; tok = strtok_r(NULL, " ", &save)) {
        if (tok == first) continue;
        char *eq = strchr(tok, '=');
        ASSERT(eq, "bad header field %s", tok);
        *eq = 0;
        if (!strcmp(tok, "kind")) snprintf(kind, sizeof kind, "%s", eq + 1);
        else if (!strcmp(tok, "seq")) s = atoi(eq + 1);
        else if (!strcmp(tok, "part")) part = atoi(eq + 1);
    }
    if (strcmp(kind, "LINE")) return -1;
    send_frame("GOT", s, part, 0, "");
    for (int i = 0; i < nseen; i++) if (seen[i].seq == s && seen[i].part == part) return -1;
    ASSERT(nseen < 64, "too many frames");
    seen[nseen++] = (seen_key){ s, part };
    snprintf(body, cap, "%s", nl + 1);
    *seq = s;
    return 1;
}

static pid_t spawn(char *const argv[], bool quiet) {
    pid_t pid = fork();
    ASSERT(pid >= 0, "fork: %s", strerror(errno));
    if (pid == 0) {
        if (quiet) {
            int null = open("/dev/null", O_WRONLY);
            dup2(null, 1); dup2(null, 2);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static int read_port(const char *name) {
    char path[320], text[16] = "";
    snprintf(path, sizeof path, "%s/%s", root, name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(text, sizeof text, f)) text[0] = 0;
    fclose(f);
    return atoi(text);
}

static void expect_calc(int job_id, const char *ask, const int64_t operands[3], int state, int reason,
                        const uint8_t *raw, size_t raw_len, const char *said) {
    char body[4096];
    under++;
    double start = now();
    int calls = 0;
    snprintf(body, sizeof body, "JOB %d do Calculate locally.\t%s\tlocal-tools=v1", job_id, ask);
    send_frame("JOB", under, 1, 1, body);
    while (now() - start < 5) {
        int seq;
        if (next_line(&seq, body, sizeof body) != 1) continue;
        ASSERT(seq == under, "seq %d != %d", seq, under);
        if (!strncmp(body, "WAIT ", 5)) continue;
        if (!strncmp(body, "SAY ", 4)) {
            char want[512];
            snprintf(want, sizeof want, "SAY %d 1/1 %s", job_id, said);
            ASSERT(!strcmp(body, want), "unexpected answer %s (wanted %s)", body, want);
            ASSERT(calls == 1, "calls=%d", calls);
            printf("LOCAL_DIRECT_UDP: PASS job=%d elapsed_ms=%.2f model=offline\n", job_id, (now() - start) * 1000);
            return;
        }
        char head[64], hex[256];
        snprintf(head, sizeof head, "CALL %d 1 1 24 0 ", job_id);
        ASSERT(!strncmp(body, head, strlen(head)), "unexpected call %s", body);
        snprintf(hex, sizeof hex, "%s", body + strlen(head));
        uint8_t data[64];
        size_t dn = from_hex(hex, data, sizeof data);
        ASSERT(dn == 24, "operand bytes %zu", dn);
        for (int i = 0; i < 3; i++) ASSERT((int64_t)get_le64(data + 8 * i) == operands[i], "operand %d in %s", i, body);
        calls++;
        under++;
        char rhex[64];
        to_hex(raw, raw_len, rhex);
        snprintf(body, sizeof body, "RESULT %d 1 %d %d %s", job_id, state, reason, rhex);
        send_frame("JOB", under, 1, 1, body);
    }
    die("direct arithmetic stalled with model offline");
}

int main(int argc, char **argv) {
    if (argc != 3) { fputs("usage: local_tools_udp_test ORACLE RELAY\n", stderr); return 2; }
    const char *tmpdir = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/local-tools-udp-XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }

    char *oracle_argv[] = { argv[1], root, NULL };
    oracle_pid = spawn(oracle_argv, false);
    double until = now() + 5;
    int model_port;
    while (!(model_port = read_port("oracle-port.txt"))) {
        ASSERT(now() < until, "oracle startup");
        usleep(20000);
    }
    int reserve = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t alen = sizeof a;
    ASSERT(reserve >= 0 && !bind(reserve, (struct sockaddr *)&a, sizeof a) &&
           !getsockname(reserve, (struct sockaddr *)&a, &alen), "reserve port");
    relay_port = ntohs(a.sin_port);
    close(reserve);

    char port_s[16], model_s[32], work[300], logs[300];
    snprintf(port_s, sizeof port_s, "%d", relay_port);
    snprintf(model_s, sizeof model_s, "127.0.0.1:%d", model_port);
    snprintf(work, sizeof work, "%s/work", root);
    snprintf(logs, sizeof logs, "%s/logs", root);
    char *relay_argv[] = { argv[2], "--udp", port_s, "--model", model_s, "--work", work, "--log-dir", logs, NULL };
    relay_pid = spawn(relay_argv, true);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    struct timeval tv = { 0, 200000 };
    ASSERT(sock >= 0 && !bind(sock, (struct sockaddr *)&me, sizeof me) &&
           !setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv), "client socket");
    usleep(200000);
    send_frame("HELLO", 0, 0, 0, "");
    send_frame("JOB", under, 1, 1,
               "JOB 4242 do Check tools.\tCheck arithmetic, text and system.\tlocal-tools=v1");

    int tools[8], ntools = 0;
    bool done = false;
    for (double deadline = now() + 20; now() < deadline && !done;) {
        char body[4096];
        int seq;
        if (next_line(&seq, body, sizeof body) != 1) continue;
        ASSERT(seq == under, "transport sequence is not semantic job ID: %d vs %d", seq, under);
        if (!strncmp(body, "WAIT ", 5)) continue;
        if (!strcmp(body, "SAY 4242 1/1 VERIFIED")) { done = true; break; }
        int inv, tool, total, off, used = 0;
        char hex[1024];
        ASSERT(sscanf(body, "CALL 4242 %d %d %d %d %1023s%n", &inv, &tool, &total, &off, hex, &used) == 5,
               "unexpected line %s", body);
        uint8_t data[256];
        size_t dn = strcmp(hex, "-") ? from_hex(hex, data, sizeof data) : 0;
        ASSERT(off == 0 && (int)dn == total, "bad call %s", body);
        ASSERT(ntools < 8, "too many calls");
        tools[ntools++] = tool;
        uint8_t result[40];
        size_t rn;
        if (tool == 1) {
            ASSERT(dn == 24 && get_le64(data) == 1 && get_le64(data + 8) == 20 && get_le64(data + 16) == 22, "arithmetic operands: %s", body);
            put_le64(result, 42); rn = 8;
        } else if (tool == 2) {
            ASSERT(dn == 13 && !memcmp(data, "one two\nthree", 13), "text operand: %s", body);
            put_le64(result, 13); put_le64(result + 8, 3); put_le64(result + 16, 1); rn = 24;
        } else {
            ASSERT(tool == 3 && dn == 0, "system call: %s", body);
            put_le64(result, 4); put_le64(result + 8, 1000000); put_le64(result + 16, 1920);
            put_le64(result + 24, 1080); put_le64(result + 32, 1); rn = 40;
        }
        under++;
        char rhex[96], reply[256];
        to_hex(result, rn, rhex);
        snprintf(reply, sizeof reply, "RESULT 4242 %d 2 0 %s", inv, rhex);
        send_frame("JOB", under, 1, 1, reply);
    }
    ASSERT(done, "no final answer before deadline");
    ASSERT(ntools == 3 && tools[0] == 1 && tools[1] == 2 && tools[2] == 3, "tool order");
    int status = -1;
    for (double wait_until = now() + 3; now() < wait_until;) {
        pid_t w = waitpid(oracle_pid, &status, WNOHANG);
        if (w == oracle_pid) { oracle_pid = 0; break; }
        usleep(20000);
    }
    ASSERT(oracle_pid == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "oracle did not exit cleanly");
    puts("LOCAL_TOOLS_UDP: PASS distinct transport/job IDs, three calls and results");

    /* The HTTP model is gone. Direct arithmetic must still call the native tool,
     * accept only its bound result, and render without a second model. */
    uint8_t forty_two[8], none[1] = { 0 };
    put_le64(forty_two, 42);
    expect_calc(4243, "What is 123 multiplied by 456?", (int64_t[]){ 3, 123, 456 }, 2, 0,
                forty_two, 8, "42.");
    expect_calc(4244, "1 / 0", (int64_t[]){ 4, 1, 0 }, 3, 2, none, 0, "Division by zero isn't defined.");
    expect_calc(4245, "9223372036854775807 + 1", (int64_t[]){ 1, INT64_MAX, 1 }, 3, 1, none, 0,
                "That result is outside the supported 64-bit integer range.");
    cleanup();
    return 0;
}
