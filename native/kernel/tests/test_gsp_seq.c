/* test_gsp_seq.c -- the sequencer must match the pinned driver's rules, and a copy
 * with the save-index check removed must fail the out-of-range case. */
#include <stdio.h>
#include <string.h>
#include "gsp_seq.h"

static int fails, checks;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; if (fails <= 20) printf("FAIL %s\n", m); } } while (0)

static uint32_t regs[8];
static uint32_t delays, core_ops;

static uint32_t rd(void *c, uint32_t a) { (void)c; return a < 8 ? regs[a] : 0; }
static void wr(void *c, uint32_t a, uint32_t v) { (void)c; if (a < 8) regs[a] = v; }
static void delay(void *c, uint32_t us) { (void)c; delays += us; }
static int core(void *c, uint32_t op) { (void)c; core_ops++; return op == 99 ? -1 : 0; }

static gsp_seq_io io(void)
{
    gsp_seq_io x;
    memset(&x, 0, sizeof x);
    x.rd = rd; x.wr = wr; x.delay_us = delay; x.core = core; x.poll_budget = 4;
    return x;
}

int main(void)
{
    const char *why = 0;
    gsp_seq_io x = io();
    /* cmdIndex must be strictly inside bufferSize, as kernel_gsp.c asserts */
    uint32_t wr1[] = { GSP_SEQ_REG_WRITE, 1, 0xabc, 0 };
    CHECK(gsp_seq_run(wr1, 4, 4, &x, &why) != 0, "cmdIndex == bufferSize must refuse");
    CHECK(gsp_seq_run(wr1, 4, 3, &x, &why) == 0 && regs[1] == 0xabc, "REG_WRITE");
    regs[2] = 0xff;
    uint32_t mod[] = { GSP_SEQ_REG_MODIFY, 2, 0x0f, 0x05, 0 };
    CHECK(gsp_seq_run(mod, 5, 4, &x, &why) == 0 && regs[2] == 0xf5, "REG_MODIFY (old & ~mask) | val");
    regs[3] = 0;
    /* poll sees the value the write in the same buffer stored */
    uint32_t both[] = { GSP_SEQ_REG_WRITE, 3, 7, GSP_SEQ_REG_POLL, 3, 0xff, 7, 10, 1, 0 };
    CHECK(gsp_seq_run(both, 10, 9, &x, &why) == 0, "poll after write settles");
    regs[3] = 0;
    uint32_t stuck[] = { GSP_SEQ_REG_POLL, 3, 1, 1, 10, 42, 0 };
    CHECK(gsp_seq_run(stuck, 7, 6, &x, &why) != 0, "poll that never settles refuses");
    uint32_t dly[] = { GSP_SEQ_DELAY_US, 25, 0 };
    delays = 0;
    CHECK(gsp_seq_run(dly, 3, 2, &x, &why) == 0 && delays == 25, "DELAY_US is counted, not slept");
    regs[4] = 0x11;
    uint32_t st[] = { GSP_SEQ_REG_STORE, 4, 0, 0 };
    CHECK(gsp_seq_run(st, 4, 3, &x, &why) == 0 && x.save[0] == 0x11, "REG_STORE");
    uint32_t st8[] = { GSP_SEQ_REG_STORE, 4, 8, 0 };
    CHECK(gsp_seq_run(st8, 4, 3, &x, &why) != 0, "save index 8 refuses");
    uint32_t bad[] = { 99, 0 };
    CHECK(gsp_seq_run(bad, 2, 1, &x, &why) != 0, "unknown opcode refuses");
    uint32_t cut[] = { GSP_SEQ_REG_WRITE, 1, 0 };
    CHECK(gsp_seq_run(cut, 4, 2, &x, &why) != 0, "short payload refuses");
    x.core = 0;
    uint32_t resume[] = { GSP_SEQ_CORE_RESUME, 0 };
    CHECK(gsp_seq_run(resume, 2, 1, &x, &why) != 0, "CORE_RESUME with no callback refuses");
    x.core = core;
    core_ops = 0;
    CHECK(gsp_seq_run(resume, 2, 1, &x, &why) == 0 && core_ops == 1, "CORE_RESUME calls the callback once");
    CHECK(gsp_seq_run(0, 1, 0, &x, &why) != 0, "null buffer refuses");
    if (fails) { printf("GSP_SEQ_FAIL checks=%d failed=%d\n", checks, fails); return 1; }
    printf("GSP_SEQ_PASS checks=%d\n", checks);
    return 0;
}
