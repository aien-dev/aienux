/* gsp_seq.c -- see gsp_seq.h. Opcode sizes from rmgspseq.h GSP_SEQUENCER_PAYLOAD_SIZE_DWORDS.
 * REG_MODIFY is (old & ~mask) | val, as kernel_gsp.c writes it. REG_POLL waits until
 * (reg & mask) == val. REG_STORE refuses an index of 8 or more (GSP_SEQ_BUF_REG_SAVE_SIZE is 8). */
#include "gsp_seq.h"

static uint32_t payload_dwords(uint32_t op)
{
    switch (op) {
    case GSP_SEQ_REG_WRITE: return 2;
    case GSP_SEQ_REG_MODIFY: return 3;
    case GSP_SEQ_REG_POLL: return 5;
    case GSP_SEQ_DELAY_US: return 1;
    case GSP_SEQ_REG_STORE: return 2;
    case GSP_SEQ_CORE_RESET:
    case GSP_SEQ_CORE_START:
    case GSP_SEQ_CORE_WAIT:
    case GSP_SEQ_CORE_RESUME: return 0;
    default: return 0xffffffffu;
    }
}

int gsp_seq_run(const uint32_t *buf, uint32_t nwords, uint32_t end, gsp_seq_io *io, const char **why)
{
    if (why) *why = 0;
    if (!buf || !io || !io->rd || !io->wr) { if (why) *why = "null argument"; return -1; }
    if (nwords == 0) { if (why) *why = "empty buffer"; return -1; }
    if (end >= nwords) { if (why) *why = "cmdIndex not inside the buffer"; return -1; }
    uint32_t i = 0;
    while (i < end) {
        uint32_t op = buf[i++];
        uint32_t n = payload_dwords(op);
        if (n == 0xffffffffu) { if (why) *why = "unknown opcode"; return -1; }
        if (i + n > end) { if (why) *why = "opcode overruns cmdIndex"; return -1; }
        const uint32_t *a = buf + i;
        if (op == GSP_SEQ_REG_WRITE) {
            io->wr(io->ctx, a[0], a[1]);
        } else if (op == GSP_SEQ_REG_MODIFY) {
            uint32_t v = io->rd(io->ctx, a[0]);
            io->wr(io->ctx, a[0], (v & ~a[1]) | a[2]);
        } else if (op == GSP_SEQ_REG_POLL) {
            uint32_t left = io->poll_budget ? io->poll_budget : 1;
            uint32_t v = io->rd(io->ctx, a[0]);
            while ((v & a[1]) != a[2]) {
                if (left == 0) { if (why) *why = "poll timed out"; return -1; }
                left--;
                v = io->rd(io->ctx, a[0]);
            }
        } else if (op == GSP_SEQ_DELAY_US) {
            if (io->delay_us) io->delay_us(io->ctx, a[0]);
        } else if (op == GSP_SEQ_REG_STORE) {
#ifdef GSP_SEQ_MUTANT_INDEX
            /* planted bug: an index past the save area is accepted */
            if (a[1] < GSP_SEQ_REG_SAVE) io->save[a[1]] = io->rd(io->ctx, a[0]);
#else
            if (a[1] >= GSP_SEQ_REG_SAVE) { if (why) *why = "save index out of range"; return -1; }
            io->save[a[1]] = io->rd(io->ctx, a[0]);
#endif
        } else {
            if (!io->core) { if (why) *why = "core opcode with no core callback"; return -1; }
            if (io->core(io->ctx, op)) { if (why) *why = "core opcode failed"; return -1; }
        }
        i += n;
    }
    return 0;
}
