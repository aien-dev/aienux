/* console_log.h -- the console's RAM copy of what was written (console_x86.c),
 * kept apart so the host can test the exact append the kernel runs. No
 * device, no clock. The copy keeps the FIRST CK_X86_LOG_BYTES written, so the
 * boot commit lines stay; anything past that is not stored, but it is
 * counted, so a full copy is reported (report_x86.c) and never looks complete.
 * CK_X86_REPORT_MAX (report_x86.h) is the same size: the filtered report is
 * never larger than the copy it is made from. */
#ifndef AIENOS_CK_X86_CONSOLE_LOG_H
#define AIENOS_CK_X86_CONSOLE_LOG_H
#include <stddef.h>
/* memcpy: the includer provides it (ck_internal.h in the kernel, string.h on the host). */

#define CK_X86_LOG_BYTES 65536u

struct ck_x86_log {
    char buf[CK_X86_LOG_BYTES];
    size_t len;     /* bytes stored */
    size_t dropped; /* bytes that did not fit, after the copy filled */
};

static inline void ck_x86_log_append(struct ck_x86_log *l, const char *s, size_t n)
{
    size_t k = n < CK_X86_LOG_BYTES - l->len ? n : CK_X86_LOG_BYTES - l->len;
    memcpy(l->buf + l->len, s, k);
    l->len += k;
    l->dropped += n - k;
}
#endif
