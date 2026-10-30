/* ck_imac_swap.c -- Spark-side sender: ship a SIGNED slot object to a running iMac
 * over UDP port 47034 (the live-swap design in docs/IMAC_GPU_BENCH.md).
 *
 * Usage: ck_imac_swap push <SLOT_OBJECT> [--target <ip>]
 *
 * SLOT_OBJECT is a signed slot object: one 4096-byte manifest unit followed by the
 * raw kernel PE. Make one with the signing tool, never with this file:
 *   ck_slot_tool make-manifest --image IMACKERN-LAB.EFI --generation N --out M.bin
 *   ck_slot_tool sign-test M.bin --out MS.bin          (TEST key: QEMU only)
 *   cat MS.bin IMACKERN-LAB.EFI > SLOT_OBJECT
 * (Owner keys: ck_slot_tool tbs / offline signer / attach-sig.) This tool does not
 * sign and does not check signatures; the kernel verifies with ck_slot_verify and
 * refuses anything that does not verify.
 *
 * STATUS: the kernel side of this protocol is NOT BUILT in this branch. swap_x86.c's
 * UDP receiver is a stub, so no ACK comes back and this tool times out against a real
 * iMac. The QEMU swap test stages the same slot object on a disk instead.
 *
 * C standard library only. Build: make -C native/kernel ck-imac-swap */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

#include "../x86/swap_proto.h"

/* Size of the slot manifest unit (slot_format.h: CK_SLOT_UNIT). */
#define SLOT_UNIT 4096
#define SLOT_MAGIC "AIENSLT1"

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

/* Read the whole file into a malloc'd buffer. Returns the length. */
static size_t read_file(const char *path, uint8_t **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno)); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); *out = 0; return 0; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) die("malloc");
    if ((long)fread(buf, 1, (size_t)sz, f) != sz) die("fread");
    fclose(f);
    *out = buf;
    return (size_t)sz;
}

static void cmd_push(int argc, char **argv)
{
    const char *obj_path = NULL, *target_ip = "255.255.255.255";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--target") && i + 1 < argc) target_ip = argv[++i];
        else if (!obj_path) obj_path = argv[i];
    }
    if (!obj_path) { fprintf(stderr, "usage: ck_imac_swap push <SLOT_OBJECT> [--target IP]\n"); exit(1); }

    uint8_t *obj; size_t obj_len = read_file(obj_path, &obj);
    if (obj_len < SLOT_UNIT || memcmp(obj, SLOT_MAGIC, 8) != 0) {
        fprintf(stderr, "%s is not a signed slot object (no %s manifest unit)\n", obj_path, SLOT_MAGIC);
        exit(1);
    }
    printf("swap: read %s (%zu bytes, manifest + image)\n", obj_path, obj_len);
    printf("swap: sending %zu bytes to %s:%u\n", obj_len, target_ip, CK_X86_SWAP_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) die("socket");
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof bcast);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(CK_X86_SWAP_PORT);
    dst.sin_addr.s_addr = inet_addr(target_ip);

    /* Session nonce, echoed by the kernel's ACKs. */
    uint8_t nonce[8];
    srand((unsigned)time(NULL));
    for (int i = 0; i < 8; i++) nonce[i] = (uint8_t)rand();

    /* Stop-and-wait: one DATA packet, wait up to 500 ms for an ACK, resend on timeout. */
    uint32_t offset = 0;
    uint32_t total = (uint32_t)obj_len;
    uint8_t pkt[SWAP_PUSH_MIN + SWAP_CHUNK];
    uint8_t ack[64];
    unsigned chunks = (total + SWAP_CHUNK - 1) / SWAP_CHUNK;
    unsigned chunk_no = 0;

    while (offset < total) {
        uint32_t len = total - offset;
        if (len > SWAP_CHUNK) len = SWAP_CHUNK;

        memset(pkt, 0, SWAP_PUSH_MIN);
        memcpy(pkt, SWAP_MAGIC, 4);
        pkt[4] = SWAP_VERSION;
        pkt[5] = SWAP_TYPE_DATA;
        memcpy(pkt + 8, nonce, 8);
        swap_w32(pkt + 16, offset);
        swap_w32(pkt + 20, total);
        swap_w16(pkt + 24, (uint16_t)len);
        memcpy(pkt + SWAP_PUSH_MIN, obj + offset, len);

        size_t pkt_len = SWAP_PUSH_MIN + len;
        if (sendto(sock, pkt, pkt_len, 0, (struct sockaddr *)&dst, sizeof dst) < 0)
            die("sendto");

        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = {0, 500000};
        int r = select(sock + 1, &fds, NULL, NULL, &tv);
        if (r > 0) {
            socklen_t al = sizeof dst;
            ssize_t n = recvfrom(sock, ack, sizeof ack, 0, (struct sockaddr *)&dst, &al);
            if (n >= (ssize_t)SWAP_ACK_MIN && !memcmp(ack, SWAP_MAGIC_ACK, 4)) {
                uint8_t status = ack[5];
                uint32_t want = swap_r32(ack + 16);
                if (status == SWAP_ACK_DONE) {
                    printf("swap: kernel verified, jumping (offset %u/%u)\n", offset, total);
                    free(obj); close(sock); return;
                } else if (status == SWAP_ACK_FAIL) {
                    char text[81] = {0};
                    if (n > (ssize_t)SWAP_ACK_MIN) {
                        size_t tlen = (size_t)(n - SWAP_ACK_MIN);
                        if (tlen > 80) tlen = 80;
                        memcpy(text, ack + SWAP_ACK_MIN, tlen);
                    }
                    fprintf(stderr, "swap: REJECTED by iMac: %s\n", text);
                    free(obj); close(sock); exit(1);
                } else {
                    offset = want;   /* the ACK advances the offset */
                }
            }
        }
        /* no ACK in time: the same chunk goes out again on the next pass */
        chunk_no++;
        if (chunk_no % 16 == 0)
            printf("swap: sent chunk %u/%u (%.0f%%)\n", chunk_no, chunks,
                   100.0 * chunk_no / chunks);
    }

    memset(pkt, 0, SWAP_PUSH_MIN);
    memcpy(pkt, SWAP_MAGIC, 4);
    pkt[4] = SWAP_VERSION;
    pkt[5] = SWAP_TYPE_END;
    memcpy(pkt + 8, nonce, 8);
    swap_w32(pkt + 20, total);
    sendto(sock, pkt, SWAP_PUSH_MIN, 0, (struct sockaddr *)&dst, sizeof dst);
    printf("swap: all %u chunks sent\n", chunks);

    for (int i = 0; i < 30; i++) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = {1, 0};
        if (select(sock + 1, &fds, NULL, NULL, &tv) > 0) {
            socklen_t al = sizeof dst;
            ssize_t n = recvfrom(sock, ack, sizeof ack, 0, (struct sockaddr *)&dst, &al);
            if (n >= (ssize_t)SWAP_ACK_MIN && !memcmp(ack, SWAP_MAGIC_ACK, 4)) {
                uint8_t status = ack[5];
                if (status == SWAP_ACK_DONE) {
                    printf("swap: done, new kernel jumping\n");
                    free(obj); close(sock); return;
                } else if (status == SWAP_ACK_FAIL) {
                    fprintf(stderr, "swap: kernel rejected the image\n");
                    free(obj); close(sock); exit(1);
                }
            }
        }
    }
    printf("swap: no final ACK (the iMac has no swap receiver in this build, or it is unreachable)\n");
    free(obj); close(sock);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ck_imac_swap push <SLOT_OBJECT> [--target IP]\n");
        return 1;
    }
    if (!strcmp(argv[1], "push")) { cmd_push(argc - 2, argv + 2); return 0; }
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 1;
}
