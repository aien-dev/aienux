/* osh_err.h -- OSH platform error codes (OSH_PLATFORM_ABI.md section 8.6, v1 DRAFT, unfrozen; read at aien-protocols-mv
 * fb5fca98e667). One copy of the numbering, shared by the test-only shell adapter (osh_op.h) and the kernel pipe primitive
 * (pipe.h), so the two can never disagree. Pure definitions, no test-only guard: the pipe primitive is not test-only. */
#ifndef AIENOS_CK_OSH_ERR_H
#define AIENOS_CK_OSH_ERR_H

enum osh_err {
    OSH_E_OK = 0, OSH_E_DENIED = 1, OSH_E_REVOKED = 2, OSH_E_STALE = 3, OSH_E_NOT_FOUND = 4, OSH_E_UNAVAILABLE = 5,
    OSH_E_INVALID_ARG = 6, OSH_E_LIMIT = 7, OSH_E_INTERRUPTED = 8, OSH_E_BROKEN_PIPE = 9, OSH_E_IO = 10,
    OSH_E_OUTCOME_UNKNOWN = 11, OSH_E_CAP_DOMAIN_MISMATCH = 12, OSH_E_CAP_GEN_NARROW = 13, OSH_E_NOT_SUPPORTED = 14,
    OSH_E_WOULD_BLOCK = 15, OSH_E_PARTIAL_LAUNCH = 16
};
#define OSH_E_COUNT 17u

/* Name of a code; section 8.6: an unknown code is IO, never OK. */
static inline const char *osh_err_label(unsigned e)
{
    static const char *const n[OSH_E_COUNT] = { "OK", "DENIED", "REVOKED", "STALE", "NOT_FOUND", "UNAVAILABLE",
                                                "INVALID_ARG", "LIMIT", "INTERRUPTED", "BROKEN_PIPE", "IO",
                                                "OUTCOME_UNKNOWN", "CAP_DOMAIN_MISMATCH", "CAP_GEN_NARROW",
                                                "NOT_SUPPORTED", "WOULD_BLOCK", "PARTIAL_LAUNCH" };
    return e < OSH_E_COUNT ? n[e] : "IO";
}

#endif
