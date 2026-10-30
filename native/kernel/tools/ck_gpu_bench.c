/* ck_gpu_bench.c -- Spark-side GPU bench console tool (LAB BUILD: CK_X86_REPORT=1 kernel).
 *
 * Usage: ck_gpu_bench [--key FILE] [--target IP] [--serial PATH] [--transcript FILE] <script.bench>
 *        ck_gpu_bench ping [same options]
 *
 * Transports:
 *   default (UDP): datagrams to the iMac, port 47035. This is the hardware path. The kernel side
 *     uses tg3 RX/TX, which is compiled in the lab build but is NOT tested in this repo.
 *   --serial PATH: a unix socket to the kernel's COM1 (QEMU: -serial unix:PATH,server=on,wait=on).
 *     The test build (CK_X86_BENCH_TEST=1) answers the same packets as text lines:
 *       request  "ABCHREQ " + 128 hex digits
 *       reply    "ABCHRESP " + 128 hex digits
 *     The tool waits for "bench: GPU bench console ready" before it sends.
 *
 * Script lines (one command per line, # comments, numbers decimal or 0x hex):
 *   ping
 *   pci_cfg <offset>            bar5_read <reg>            bar5_write <reg> <value>
 *   bar0_read <word>            bar0_write <word> <value>
 *   bar5_poll <reg> <mask> <expected> <timeout_ms>
 *   bar5_rmw <reg> <clear_mask> <set_bits>
 *   badauth <command ...>   the same command with one HMAC byte flipped (kernel must answer ERR_AUTH)
 *   stale <command ...>     the same command with counter 1 (not above the kernel's last: ERR_AUTH)
 *   replay                  the previous request again, byte for byte (same counter: ERR_AUTH)
 *
 * Counter: starts from the clock (seconds * 1000) so a second run is still above the kernel's
 * last counter. A status of ERR_ALLONES stops the script (device gone); the tool sends nothing after it.
 *
 * Output: one line per transaction, "<label> <STATUS>: result=0x.. before=0x..". Every byte the
 * serial link carries is copied to --transcript FILE if given.
 * Exit: 0 all OK; 1 an error status or an ALLONES stop; 2 transport failure.
 *
 * Build: cc -O2 -std=c11 -o ck_gpu_bench ck_gpu_bench.c   (make -C native/kernel ck-gpu-bench) */
#define _POSIX_C_SOURCE 200809L /* clock_gettime */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>

#include "../x86/bench_proto.h"

/* Minimal SHA-256 for HMAC (sufficient for test; production uses OpenSSL or libsodium). */
static uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR32(v,n) (((v)>>(n))|((v)<<(32-(n))))
static void sha256_block(uint32_t h[8], const uint8_t *b)
{
    uint32_t w[64]; unsigned i;
    for (i=0;i<16;i++) w[i]=((uint32_t)b[4*i]<<24)|((uint32_t)b[4*i+1]<<16)|((uint32_t)b[4*i+2]<<8)|b[4*i+3];
    for (i=16;i<64;i++){uint32_t s0=ROR32(w[i-15],7)^ROR32(w[i-15],18)^(w[i-15]>>3);uint32_t s1=ROR32(w[i-2],17)^ROR32(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
    uint32_t a=h[0],b2=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for(i=0;i<64;i++){uint32_t S1=ROR32(e,6)^ROR32(e,11)^ROR32(e,25);uint32_t ch=(e&f)^(~e&g);uint32_t t1=hh+S1+ch+sha256_k[i]+w[i];uint32_t S0=ROR32(a,2)^ROR32(a,13)^ROR32(a,22);uint32_t maj=(a&b2)^(a&c)^(b2&c);uint32_t t2=S0+maj;hh=g;g=f;f=e;e=d+t1;d=c;c=b2;b2=a;a=t1+t2;}
    h[0]+=a;h[1]+=b2;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t buf[64]; size_t off=0; uint64_t bit_len=(uint64_t)len*8;
    while(off+64<=len){sha256_block(h,data+off);off+=64;}
    size_t rem=len-off; memcpy(buf,data+off,rem); buf[rem++]=0x80;
    if(rem>56){while(rem<64)buf[rem++]=0;sha256_block(h,buf);rem=0;}
    while(rem<56)buf[rem++]=0;
    for(int i=7;i>=0;i--){buf[56+(7-i)]=(uint8_t)(bit_len>>(8*i));}
    sha256_block(h,buf);
    for(int i=0;i<8;i++){out[4*i]=(uint8_t)(h[i]>>24);out[4*i+1]=(uint8_t)(h[i]>>16);out[4*i+2]=(uint8_t)(h[i]>>8);out[4*i+3]=(uint8_t)h[i];}
}
static void hmac_sha256(const uint8_t key[32], const uint8_t *msg, size_t mlen, uint8_t out[32])
{
    uint8_t ipad[64],opad[64],inner[32];
    for(int i=0;i<32;i++){ipad[i]=key[i]^0x36;opad[i]=key[i]^0x5c;}
    for(int i=32;i<64;i++){ipad[i]=0x36;opad[i]=0x5c;}
    uint8_t *tmp=malloc(64+mlen); if(!tmp)exit(1);
    memcpy(tmp,ipad,64); memcpy(tmp+64,msg,mlen);
    sha256(tmp,64+mlen,inner); free(tmp);
    uint8_t tmp2[96]; memcpy(tmp2,opad,64); memcpy(tmp2+64,inner,32);
    sha256(tmp2,96,out);
}

static const uint8_t test_key[32] = { BENCH_TEST_KEY_INIT };
static uint8_t bench_key[32];
static uint64_t counter;
static int sock = -1;                 /* UDP socket */
static struct sockaddr_in target;
static int serial_fd = -1;            /* COM1 socket (QEMU test) */
static int use_serial;
static FILE *transcript;
static uint8_t last_req[BENCH_REQ_BYTES];
static int have_last;

enum mode { M_NORMAL, M_BADAUTH, M_STALE, M_REPLAY };

/* ---- transports ---- */
static void init_net(const char *ip)
{
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); exit(2); }
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof bcast);
    memset(&target, 0, sizeof target);
    target.sin_family = AF_INET;
    target.sin_port = htons(CK_X86_BENCH_PORT);
    target.sin_addr.s_addr = inet_addr(ip);
    struct sockaddr_in local; memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_port = htons(CK_X86_BENCH_PORT);
    local.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (struct sockaddr *)&local, sizeof local);
}

static void nap_ms(int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    select(0, NULL, NULL, NULL, &tv);
}

static int serial_connect(const char *path)
{
    for (int i = 0; i < 300; i++) {          /* QEMU may still be starting: retry for 60 s */
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_un a; memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, path, sizeof a.sun_path - 1);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) return fd;
        close(fd);
        nap_ms(200);
    }
    return -1;
}

/* Serial receive buffer: text from the kernel (console lines and replies). */
static char rx[1 << 16];
static size_t rxn, rxpos;

/* Reads whatever arrives within timeout_ms into rx. Returns bytes read, or -1. */
static int serial_fill(int timeout_ms)
{
    fd_set fds; FD_ZERO(&fds); FD_SET(serial_fd, &fds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    if (select(serial_fd + 1, &fds, NULL, NULL, &tv) <= 0) return -1;
    if (rxpos) { memmove(rx, rx + rxpos, rxn - rxpos); rxn -= rxpos; rxpos = 0; }
    if (rxn >= sizeof rx - 1) rxn = 0;      /* nothing useful can be this long: drop it */
    ssize_t n = read(serial_fd, rx + rxn, sizeof rx - 1 - rxn);
    if (n <= 0) return -1;
    if (transcript) fwrite(rx + rxn, 1, (size_t)n, transcript);
    rxn += (size_t)n;
    rx[rxn] = 0;
    return (int)n;
}

/* The needle and the end of its line: a request sent while the kernel is still
 * printing the line would land inside it in the transcript. */
static int text_line_done(const char *needle)
{
    const char *at = strstr(rx + rxpos, needle);
    return at && strchr(at, '\n') != NULL;
}

static int wait_for_text(const char *needle, int seconds)
{
    for (int i = 0; i < seconds * 5; i++) {
        if (text_line_done(needle)) return 1;
        serial_fill(200);
    }
    return text_line_done(needle);
}

static void hex_encode(const uint8_t *b, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = hx[b[i] >> 4]; out[2 * i + 1] = hx[b[i] & 15]; }
    out[2 * n] = 0;
}

static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Finds "ABCHRESP " + 128 hex digits in the buffer and decodes it into resp. The reply line
 * arrives a few bytes per read, so the wait is bounded by time, not by the number of reads. */
#define SERIAL_REPLY_MS 30000
static int serial_recv_resp(uint8_t resp[BENCH_RESP_BYTES])
{
    static const char tag[] = "ABCHRESP ";
    for (int64_t until = mono_ms() + SERIAL_REPLY_MS; mono_ms() < until;) {
        char *p = strstr(rx + rxpos, tag);
        if (p) {
            size_t need = strlen(tag) + 2 * BENCH_RESP_BYTES;
            if ((size_t)(p - rx) + need <= rxn) {
                char *h = p + strlen(tag);
                for (unsigned i = 0; i < BENCH_RESP_BYTES; i++) {
                    int hi = hexnib(h[2 * i]), lo = hexnib(h[2 * i + 1]);
                    if (hi < 0 || lo < 0) { rxpos = (size_t)(p - rx) + 1; goto next; }
                    resp[i] = (uint8_t)(hi << 4 | lo);
                }
                rxpos = (size_t)(p - rx) + need;
                return 0;
            }
        }
    next:
        serial_fill(1000);
    }
    return -1;
}

/* ---- one transaction ---- */
static int transact(uint8_t cmd, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, enum mode m,
                    uint32_t *result, uint32_t *before, char *status_name)
{
    uint8_t req[BENCH_REQ_BYTES];
    if (m == M_REPLAY) {
        if (!have_last) { fprintf(stderr, "replay: nothing was sent yet\n"); return -1; }
        memcpy(req, last_req, sizeof req);
    } else {
        memset(req, 0, sizeof req);
        memcpy(req, BENCH_MAGIC_REQ, 4);
        req[4] = BENCH_VERSION; req[5] = cmd;
        bench_w64(req + 8, m == M_STALE ? 1u : counter++);
        bench_w32(req + 16, a0); bench_w32(req + 20, a1);
        bench_w32(req + 24, a2); bench_w32(req + 28, a3);
        hmac_sha256(bench_key, req, BENCH_HMAC_OFF, req + BENCH_HMAC_OFF);
        if (m == M_BADAUTH) req[BENCH_HMAC_OFF] ^= 0x01;
    }
    memcpy(last_req, req, sizeof req);
    have_last = 1;

    uint8_t resp[BENCH_RESP_BYTES];
    if (use_serial) {
        char line[8 + 2 * BENCH_REQ_BYTES + 2];
        char hx[2 * BENCH_REQ_BYTES + 1];
        hex_encode(req, sizeof req, hx);
        snprintf(line, sizeof line, "ABCHREQ %s\n", hx);
        if (write(serial_fd, line, strlen(line)) < 0) { perror("serial write"); return -1; }
        if (transcript) fputs(line, transcript);
        if (serial_recv_resp(resp) != 0) { fprintf(stderr, "no reply on the serial link\n"); return -1; }
    } else {
        if (sendto(sock, req, BENCH_REQ_BYTES, 0, (struct sockaddr *)&target, sizeof target) < 0)
            { perror("sendto"); return -1; }
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = {2, 0};
        if (select(sock + 1, &fds, NULL, NULL, &tv) <= 0) { fprintf(stderr, "timeout\n"); return -1; }
        ssize_t n = recv(sock, resp, sizeof resp, 0);
        if (n != BENCH_RESP_BYTES) { fprintf(stderr, "bad resp len %zd\n", n); return -1; }
    }
    if (memcmp(resp, BENCH_MAGIC_RESP, 4)) { fprintf(stderr, "bad magic\n"); return -1; }
    /* Every response is HMAC'd by the kernel, auth errors included. */
    uint8_t expected[32];
    hmac_sha256(bench_key, resp, BENCH_HMAC_OFF, expected);
    if (memcmp(expected, resp + BENCH_HMAC_OFF, 32)) { fprintf(stderr, "HMAC mismatch\n"); return -1; }
    uint8_t st = resp[5];
    if (result) *result = bench_r32(resp + 24);
    if (before) *before = bench_r32(resp + 28);
    static const char *names[] = {"OK","ERR_AUTH","ERR_RANGE","ERR_ALLONES","ERR_TIMEOUT","ERR_NODEV","ERR_BUSY","ERR_CMD"};
    if (status_name) strcpy(status_name, st < 8 ? names[st] : "ERR_UNKNOWN");
    return (int)st;
}

/* ---- script ---- */
struct cmdspec { const char *name; uint8_t cmd; int nargs; };
static const struct cmdspec specs[] = {
    {"ping", BENCH_CMD_PING, 0},
    {"pci_cfg", BENCH_CMD_PCI_CFG, 1},
    {"bar5_read", BENCH_CMD_BAR5_READ, 1},
    {"bar5_write", BENCH_CMD_BAR5_WRITE, 2},
    {"bar0_read", BENCH_CMD_BAR0_READ, 1},
    {"bar0_write", BENCH_CMD_BAR0_WRITE, 2},
    {"bar5_poll", BENCH_CMD_BAR5_POLL, 4},
    {"bar5_rmw", BENCH_CMD_BAR5_RMW, 3},
};

/* Returns 0 to go on, 1 when the script must stop (device gone), 2 on transport failure. */
static int run_script(FILE *f, int *any_error)
{
    char line[256]; int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *tok[10]; int nt = 0;
        for (char *p = strtok(line, " \t\r\n"); p && nt < 10; p = strtok(NULL, " \t\r\n"))
            tok[nt++] = p;
        if (!nt) continue;

        enum mode m = M_NORMAL;
        int i = 0;
        if (!strcmp(tok[0], "badauth")) { m = M_BADAUTH; i = 1; }
        else if (!strcmp(tok[0], "stale")) { m = M_STALE; i = 1; }
        else if (!strcmp(tok[0], "replay")) m = M_REPLAY;

        const char *label = tok[0];
        uint32_t a[4] = {0, 0, 0, 0};
        const struct cmdspec *spec = NULL;
        if (m == M_REPLAY) {
            if (nt != 1) { fprintf(stderr, "line %d: replay takes no arguments\n", lineno); *any_error = 1; continue; }
            spec = &specs[0];   /* placeholder: replay sends the stored bytes */
        } else {
            if (i >= nt) { fprintf(stderr, "line %d: missing command\n", lineno); *any_error = 1; continue; }
            for (size_t k = 0; k < sizeof specs / sizeof specs[0]; k++)
                if (!strcmp(tok[i], specs[k].name)) spec = &specs[k];
            if (!spec) { fprintf(stderr, "line %d: unknown command %s\n", lineno, tok[i]); *any_error = 1; continue; }
            if (nt - (i + 1) != spec->nargs) {
                fprintf(stderr, "line %d: %s takes %d argument(s)\n", lineno, spec->name, spec->nargs);
                *any_error = 1; continue;
            }
            int bad = 0;
            for (int k = 0; k < spec->nargs; k++) {
                char *end = NULL;
                unsigned long v = strtoul(tok[i + 1 + k], &end, 0);
                if (!end || *end || tok[i + 1 + k][0] == '-') bad = 1;
                a[k] = (uint32_t)v;
            }
            if (bad) { fprintf(stderr, "line %d: bad number in %s\n", lineno, spec->name); *any_error = 1; continue; }
            label = (m == M_BADAUTH) ? "badauth" : (m == M_STALE) ? "stale" : "";
        }

        uint32_t result = 0, before = 0; char sname[32] = "";
        int st = transact(spec->cmd, a[0], a[1], a[2], a[3], m, &result, &before, sname);
        if (st < 0) { fprintf(stderr, "line %d: %s: no response\n", lineno, tok[0]); return 2; }
        char what[96];
        if (m == M_REPLAY) snprintf(what, sizeof what, "replay");
        else if (m == M_NORMAL) snprintf(what, sizeof what, "%s", spec->name);
        else snprintf(what, sizeof what, "%s %s", label, spec->name);
        printf("%s %s: result=0x%08x before=0x%08x\n", what, sname, result, before);
        fflush(stdout);
        if (transcript) fprintf(transcript, "tool: %s %s result=0x%08x before=0x%08x\n", what, sname, result, before);
        if (st == BENCH_ERR_ALLONES) {
            fprintf(stderr, "line %d: device gone (all-ones); stopping\n", lineno);
            return 1;
        }
        if (st != BENCH_OK) *any_error = 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *key_path = NULL, *target_ip = "255.255.255.255", *script_path = NULL;
    const char *serial_path = NULL, *transcript_path = NULL;
    int ping_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--key") && i + 1 < argc) key_path = argv[++i];
        else if (!strcmp(argv[i], "--target") && i + 1 < argc) target_ip = argv[++i];
        else if (!strcmp(argv[i], "--serial") && i + 1 < argc) serial_path = argv[++i];
        else if (!strcmp(argv[i], "--transcript") && i + 1 < argc) transcript_path = argv[++i];
        else if (!strcmp(argv[i], "ping")) ping_only = 1;
        else script_path = argv[i];
    }

    if (key_path) {
        FILE *kf = fopen(key_path, "rb");
        if (!kf) { perror(key_path); return 2; }
        if (fread(bench_key, 1, 32, kf) != 32) { fprintf(stderr, "key too short\n"); return 2; }
        fclose(kf);
    } else {
        memcpy(bench_key, test_key, 32);
        fprintf(stderr, "bench: using built-in test key\n");
    }
    if (transcript_path) {
        transcript = fopen(transcript_path, "w");
        if (!transcript) { perror(transcript_path); return 2; }
    }
    counter = (uint64_t)time(NULL) * 1000u;

    if (serial_path) {
        serial_fd = serial_connect(serial_path);
        if (serial_fd < 0) { fprintf(stderr, "cannot connect to %s\n", serial_path); return 2; }
        use_serial = 1;
        if (!wait_for_text("bench: GPU bench console ready", 900)) {
            fprintf(stderr, "the kernel never said the bench console is ready\n");
            return 2;
        }
    } else {
        init_net(target_ip);
    }

    int rc = 0;
    if (ping_only) {
        uint32_t result = 0; char sname[32] = "";
        int st = transact(BENCH_CMD_PING, 0, 0, 0, 0, M_NORMAL, &result, NULL, sname);
        if (st < 0) { printf("ping: no response\n"); rc = 2; }
        else { printf("ping: %s\n", st == 0 ? "PONG" : sname); rc = st == 0 ? 0 : 1; }
    } else {
        if (!script_path) {
            fprintf(stderr, "usage: ck_gpu_bench [--key F] [--target IP | --serial PATH] [--transcript F] <script.bench> | ping\n");
            return 2;
        }
        FILE *f = strcmp(script_path, "-") ? fopen(script_path, "r") : stdin;
        if (!f) { perror(script_path); return 2; }
        int any_error = 0;
        int r = run_script(f, &any_error);
        if (f != stdin) fclose(f);
        rc = r == 2 ? 2 : (r == 1 || any_error) ? 1 : 0;
    }
    if (transcript) fclose(transcript);
    return rc;
}
