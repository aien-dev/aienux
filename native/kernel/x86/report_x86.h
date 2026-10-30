/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Drake Stapleton (AIEN / aien-dev)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* report_x86.h -- the iMac kernel's network report (report_x86.c), a
 * LAB-ONLY diagnostic: compiled and linked only into the opt-in lab image
 * (make imac-kernel CK_X86_REPORT=1 -> IMACKERN-LAB.EFI). The default and
 * any resident or service build carry none of it, and the Makefile fails a
 * default image that contains it. The report is an unauthenticated UDP
 * broadcast (header field auth=none): its run id and the UDP checksum are not
 * cryptographic authenticity, so a received report is never a receipt,
 * evidence or a command. It carries only console lines whose key is on a
 * fixed allowlist (ck_x86_report_filter), never the raw console. No machine
 * access in here, so the filter and the frame builder are also compiled and
 * tested on the host. */
#ifndef AIENOS_CK_X86_REPORT_H
#define AIENOS_CK_X86_REPORT_H
#include <stddef.h>
#include <stdint.h>

#define CK_X86_REPORT_PORT 47031u   /* UDP, source and destination */
#define CK_X86_REPORT_CHUNK 1024u   /* report bytes per datagram */
#define CK_X86_REPORT_MAX 65536u    /* the console's RAM copy (console_log.h), 64 parts */
#define CK_X86_REPORT_NOTE_MAX 80u  /* room kept for the truncation note */
#define CK_X86_REPORT_LINE_MAX 240u /* longest line kept, newline excluded */

/* Copies into out (cap bytes) only the complete lines of log[0, len) that
 * are on the allowlist: the line starts with one of the fixed kernel
 * diagnostic keys followed by ':' (or is one of the fixed title lines), is at
 * most CK_X86_REPORT_LINE_MAX bytes and holds printable ASCII only (0x20 to
 * 0x7e). Every other line is dropped whole, as is an unterminated last line
 * and any line that no longer fits. Kept lines are copied byte for byte, each
 * ending in '\n'. Returns the bytes written. Pure: no device, no clock. */
size_t ck_x86_report_filter(const char *log, size_t len, char *out, size_t cap);
/* When the console's RAM copy filled and later lines were not stored
 * (dropped > 0), appends the line "report: console_copy_full dropped_bytes=N\n"
 * to out[0, len) so the report never reads as complete. Call after the filter
 * with the filter's cap reduced by CK_X86_REPORT_NOTE_MAX. Returns the new
 * length; len unchanged when dropped is 0 or the line does not fit. Pure. */
size_t ck_x86_report_seal(char *out, size_t len, size_t cap, size_t dropped);
/* Datagrams needed for a report of len bytes; 0 when len is 0 or too big. */
unsigned ck_x86_report_parts(size_t len);
/* The Ethernet frame (no FCS) carrying part `part` of the report: broadcast
 * from `mac`, IPv4 0.0.0.0 -> 255.255.255.255, UDP PORT -> PORT, TTL 1,
 * payload "AIENOS-REPORT v=2 auth=none run=<16 hex> part=<i>/<n>
 * bytes=<total>\n" + chunk. `report` must be ck_x86_report_filter output.
 * Bytes written, 0 on a bad argument or a buffer too small. */
size_t ck_x86_report_frame(const uint8_t mac[6], uint64_t run, const char *report, size_t len, unsigned part,
                           uint8_t *out, size_t cap);
#endif
