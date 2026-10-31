/* gsp_seq.h -- our CPU-side GSP sequencer. Replaces NVIDIA's kgspExecuteSequencerBuffer
 * (open-gpu-kernel-modules 580.173.02, kernel_gsp.c:5297) for the opcodes in rmgspseq.h.
 * Register and core operations are callbacks: this file never touches a device.
 * Core opcodes (reset, start, wait, resume) refuse unless the caller supplies core(),
 * because those restart the chip and must not be guessed. */
#ifndef AIEN_GSP_SEQ_H
#define AIEN_GSP_SEQ_H
#include <stdint.h>

enum {
    GSP_SEQ_REG_WRITE = 0,
    GSP_SEQ_REG_MODIFY = 1,
    GSP_SEQ_REG_POLL = 2,
    GSP_SEQ_DELAY_US = 3,
    GSP_SEQ_REG_STORE = 4,
    GSP_SEQ_CORE_RESET = 5,
    GSP_SEQ_CORE_START = 6,
    GSP_SEQ_CORE_WAIT = 7,
    GSP_SEQ_CORE_RESUME = 8
};

#define GSP_SEQ_REG_SAVE 8

typedef struct gsp_seq_io {
    uint32_t (*rd)(void *ctx, uint32_t addr);
    void (*wr)(void *ctx, uint32_t addr, uint32_t val);
    /* 0 = done, -1 = failed. NULL refuses every core opcode. */
    int (*core)(void *ctx, uint32_t opcode);
    void (*delay_us)(void *ctx, uint32_t us);
    void *ctx;
    uint32_t poll_budget; /* reads allowed per REG_POLL before it refuses */
    uint32_t save[GSP_SEQ_REG_SAVE];
} gsp_seq_io;

/* buf is commandBuffer, nwords is bufferSizeDWord, end is cmdIndex.
 * Matches the driver: end must be < nwords, and the walk stops at end.
 * Returns 0, or -1 with *why set. */
int gsp_seq_run(const uint32_t *buf, uint32_t nwords, uint32_t end, gsp_seq_io *io, const char **why);

#endif
