/* bench_proto.h -- AIENOS GPU bench console wire protocol (port 47035).
 * LAB BUILD ONLY (CK_X86_BENCH=1). Shared between the kernel (bench_x86.c)
 * and the Spark tool (tools/ck_gpu_bench.c). All integers big-endian on the
 * wire. HMAC-SHA256 with a 32-byte key bound into the signed image at build
 * time; replay protection via a monotone 64-bit counter. */
#ifndef AIENOS_CK_BENCH_PROTO_H
#define AIENOS_CK_BENCH_PROTO_H
#include <stdint.h>
#include <stddef.h>

#define CK_X86_BENCH_PORT 47035u
#define BENCH_MAGIC_REQ "ABCH"
#define BENCH_MAGIC_RESP "ABCR"
#define BENCH_VERSION 1u
#define BENCH_HMAC_BYTES 32u

/* Commands. */
#define BENCH_CMD_PING       0x00u
#define BENCH_CMD_PCI_CFG    0x01u  /* arg0=cfg_offset -> result=value */
#define BENCH_CMD_BAR5_READ  0x02u  /* arg0=reg_index  -> result=value */
#define BENCH_CMD_BAR5_WRITE 0x03u  /* arg0=reg_index, arg1=value -> before */
#define BENCH_CMD_BAR0_READ  0x04u  /* arg0=word_off   -> result=value */
#define BENCH_CMD_BAR0_WRITE 0x05u  /* arg0=word_off,  arg1=value -> before */
#define BENCH_CMD_BAR5_POLL  0x06u  /* arg0=reg_index, arg1=mask, arg2=expected, arg3=ms */
#define BENCH_CMD_BAR5_RMW   0x07u  /* arg0=reg_index, arg1=clear_mask, arg2=set_bits */

/* Status codes in responses. */
#define BENCH_OK              0x00u
#define BENCH_ERR_AUTH        0x01u  /* HMAC mismatch or counter replay */
#define BENCH_ERR_RANGE       0x02u  /* register not on allowlist */
#define BENCH_ERR_ALLONES     0x03u  /* read returned 0xFFFFFFFF */
#define BENCH_ERR_TIMEOUT     0x04u  /* TSC-bounded poll timed out */
#define BENCH_ERR_NODEV       0x05u  /* GPU BAR not mapped / not ready */
#define BENCH_ERR_BUSY        0x06u  /* kernel not bench-ready */
#define BENCH_ERR_CMD         0x07u  /* unknown command */

/* Wire layout of a request (all integers big-endian):
 *   0 magic[4] 4 version u8 5 cmd u8 6 pad[2] 8 counter u64
 *  16 arg0 u32 20 arg1 u32 24 arg2 u32 28 arg3 u32
 *  32 hmac[32]     total = 64 bytes */
#define BENCH_REQ_BYTES 64u
#define BENCH_HMAC_OFF  32u   /* offset of HMAC field in request */

/* Wire layout of a response:
 *   0 magic[4] 4 version u8 5 status u8 6 pad[2] 8 counter u64
 *  16 tsc u64 24 result u32 28 before u32
 *  32 hmac[32]     total = 64 bytes */
#define BENCH_RESP_BYTES 64u

/* BAR5 write allowlist (DWORD-index ranges, inclusive). Register numbers: AMD's MIT headers in
 * Linux, drivers/gpu/drm/amd/include/asic_reg/ (dce/dce_11_2_d.h, smu/smu_7_1_3_d.h). */
#define BENCH_BAR5_WRITE_LO0  0x4800u   /* DCE 11.2 UNIPHY link control, LVTMA panel power sequencer, backlight PWM */
#define BENCH_BAR5_WRITE_HI0  0x4900u
#define BENCH_BAR5_WRITE_LO1  0x4A00u   /* DCE 11.2 DIG 0/1 front ends (HDMI and DP encoder blocks) */
#define BENCH_BAR5_WRITE_HI1  0x4C00u
/* DCP0 graphics surface: mmGRPH_PRIMARY_SURFACE_ADDRESS 0x1a04 .. mmGRPH_UPDATE 0x1a11, mmGRPH_FLIP_CONTROL 0x1a12. */
#define BENCH_BAR5_WRITE_LO2  0x1A04u
#define BENCH_BAR5_WRITE_HI2  0x1A12u
/* SMC mailbox 0: mmSMC_MESSAGE_0 0x94, mmSMC_RESP_0 0x95 (the pair smu7_send_msg_to_smc uses). */
#define BENCH_BAR5_WRITE_LO3  0x0094u
#define BENCH_BAR5_WRITE_HI3  0x0095u
/* SMC message argument: mmSMC_MSG_ARG_0 0xa4. */
#define BENCH_BAR5_WRITE_LO4  0x00A4u
#define BENCH_BAR5_WRITE_HI4  0x00A4u

/* BAR5 read range: any DWORD index in [0, 65535]. */
#define BENCH_BAR5_READ_MAX   0xFFFFu

/* BAR0 (VRAM) window: first 256 KiB (DWORD words 0..65535). */
#define BENCH_BAR0_MAX_WORDS  0x10000u

/* All-ones sentinel. */
#define BENCH_ALLONES 0xFFFFFFFFu

/* Decode/encode helpers (big-endian). */
static inline uint32_t bench_r32(const uint8_t *b) {
    return (uint32_t)b[0]<<24|(uint32_t)b[1]<<16|(uint32_t)b[2]<<8|b[3]; }
static inline uint64_t bench_r64(const uint8_t *b) {
    return (uint64_t)bench_r32(b)<<32|(uint64_t)bench_r32(b+4); }
static inline void bench_w32(uint8_t *b, uint32_t v) {
    b[0]=(uint8_t)(v>>24);b[1]=(uint8_t)(v>>16);b[2]=(uint8_t)(v>>8);b[3]=(uint8_t)v; }
static inline void bench_w64(uint8_t *b, uint64_t v) {
    bench_w32(b,(uint32_t)(v>>32));bench_w32(b+4,(uint32_t)v); }

/* Test key (16 zero bytes + 0x01..0x10) used in QEMU/lab builds when
 * CK_X86_BENCH_KEY is not set at compile time. */
#define BENCH_TEST_KEY_INIT \
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, \
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, \
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08, \
    0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10

#endif /* AIENOS_CK_BENCH_PROTO_H */
