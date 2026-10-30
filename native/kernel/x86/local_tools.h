/* Intel-only, bounded native Omega tools. No external executable admission. */
#ifndef CK_LOCAL_TOOLS_H
#define CK_LOCAL_TOOLS_H
#include <stddef.h>
#include <stdint.h>
#include "x86.h"
#define CK_LOCAL_INPUT 4096u
#define CK_LOCAL_OUTPUT 4096u
#define CK_LOCAL_CALLS 4u
enum { CK_LOCAL_IDLE, CK_LOCAL_RUNNING, CK_LOCAL_RETURNED, CK_LOCAL_TRAPPED,
       CK_LOCAL_REFUSED, CK_LOCAL_ABNORMAL };
enum { CK_LOCAL_FAULT = 1, CK_LOCAL_CPU, CK_LOCAL_ELAPSED, CK_LOCAL_CANCELLED,
       CK_LOCAL_BAD_RETURN, CK_LOCAL_BAD_REQUEST, CK_LOCAL_LIMIT, CK_LOCAL_ADMISSION };
struct ck_local_result {
    uint64_t request, invocation;
    unsigned state, reason, length;
    uint8_t output[CK_LOCAL_OUTPUT];
};
void ck_local_init(uint64_t root, uint64_t hz);
void ck_local_begin(uint64_t request);
const struct ck_local_result *ck_local_start(uint64_t request, uint64_t invocation,
    unsigned tool, const void *input, size_t length);
const struct ck_local_result *ck_local_status(uint64_t request, uint64_t invocation);
void ck_local_step(void);
void ck_local_cancel(void);
void ck_local_snapshot(uint64_t cores, uint64_t ram, uint64_t width, uint64_t height, uint64_t network);
void ck_local_trap(struct ck_x86_frame *f);
void ck_local_selftest(void);
/* kmain owns BSP TSS and APIC. All entry/exit paths have interrupts disabled. */
void ck_local_machine_stack(uint64_t top);
int ck_local_machine_timer(int enable);
void ck_local_machine_eoi(uint64_t vector);
#endif
