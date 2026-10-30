/* swap_x86.h -- live kernel swap declarations (kernel-internal). */
#ifndef AIENOS_CK_SWAP_X86_H
#define AIENOS_CK_SWAP_X86_H
#include "../mm/frames.h"
#include "../../boot/handoff.h"
/* Init: pre-allocate the swap buffer. Call once in kernel_main. */
void ck_x86_swap_init(struct ck_frames *fr);
/* Step: check for swap triggers (UDP or disk-staged). Called from hold_step.
 * Returns 1 only if a swap was performed (and never returns in that case).
 * hand = current handoff, tramp_pa = AP trampoline page physical address. */
int  ck_x86_swap_step(const struct ck_handoff *hand, uint64_t tramp_pa);
#endif
