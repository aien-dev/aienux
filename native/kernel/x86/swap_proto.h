/* swap_proto.h -- AIENOS live kernel swap wire protocol (port 47034).
 * Shared between the running kernel (swap_x86.c) and the Spark push tool
 * (tools/ck_imac_swap.c). No heap, no I/O; freestanding C. All integers
 * big-endian in the wire format; host helpers below convert as needed.
 *
 * The payload is a signed slot object: CK_SLOT_UNIT bytes (manifest from
 * native/boot/slot_format.h) followed by the raw kernel PE image bytes.
 * The kernel verifies the manifest signature with the same Ed25519 check
 * the netboot loader uses; only then does it quiesce and jump. */
#ifndef AIENOS_CK_SWAP_PROTO_H
#define AIENOS_CK_SWAP_PROTO_H
#include <stdint.h>
#include <stddef.h>

#define CK_X86_SWAP_PORT 47034u

/* Header present in every swap packet (24 bytes). */
#define SWAP_MAGIC "ASWP"
#define SWAP_MAGIC_ACK "ASWK"
#define SWAP_VERSION 1u

/* Types for PUSH packets (Spark → iMac). */
#define SWAP_TYPE_DATA 1u  /* data chunk */
#define SWAP_TYPE_END  2u  /* all chunks sent; ask for status */

/* Status codes in ACK packets (iMac → Spark). */
#define SWAP_ACK_WANT  0u  /* more data wanted; want_offset = next expected */
#define SWAP_ACK_DONE  1u  /* verified, about to jump; done */
#define SWAP_ACK_FAIL  2u  /* rejected; see text */

#define SWAP_CHUNK  1024u  /* bytes per data chunk (NB_CHUNK) */
#define SWAP_WINDOW   16u  /* max outstanding chunks */
#define SWAP_TEXT_MAX 80u  /* max printable text in ACK */
#define SWAP_HDR_BYTES 24u /* fixed header, before data */

/* Wire layout of a PUSH packet (all integers big-endian):
 *   0 magic[4] 4 version u8 5 type u8 6 pad[2] 8 nonce[8]
 *  16 offset u32 20 total u32 24 len u16 26 pad[6] 32 data[len] */
#define SWAP_PUSH_MIN 32u

/* Wire layout of an ACK packet:
 *   0 magic[4] 4 version u8 5 status u8 6 pad[2] 8 nonce[8]
 *  16 want u32 20 total u32 24 text[n] (n <= SWAP_TEXT_MAX) */
#define SWAP_ACK_MIN  24u
#define SWAP_MSG_MAX  (SWAP_PUSH_MIN + SWAP_CHUNK)

/* Decode helpers (big-endian). */
static inline uint32_t swap_r32(const uint8_t *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static inline uint16_t swap_r16(const uint8_t *b) {
    return (uint16_t)((uint16_t)b[0] << 8 | b[1]); }
static inline void swap_w32(uint8_t *b, uint32_t v) {
    b[0]=(uint8_t)(v>>24); b[1]=(uint8_t)(v>>16); b[2]=(uint8_t)(v>>8); b[3]=(uint8_t)v; }
static inline void swap_w16(uint8_t *b, uint16_t v) {
    b[0]=(uint8_t)(v>>8); b[1]=(uint8_t)v; }

#endif /* AIENOS_CK_SWAP_PROTO_H */
