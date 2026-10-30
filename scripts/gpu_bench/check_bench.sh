#!/bin/bash
# scripts/gpu_bench/check_bench.sh -- check .bench scripts against the ck_gpu_bench console.
#
# Grammar (docs/IMAC_GPU_BENCH.md on feat/imac-gpu-bench): one command per line; a line
# whose first character is # is a comment; numbers are 0x-hex or decimal.
#   ping 0 | pci_cfg 1 | bar5_read 1 | bar5_write 2 | bar5_rmw 3 | bar5_poll 4
#   bar0_read 1 | bar0_write 2
# Indices are DWORD indices in 0..65535 (BAR5 and the BAR0 window).
# BAR5 write allowlist (DWORD index): 0x4800-0x4900, 0x4A00-0x4C00, GRPH 0x1a04-0x1a12 (DCP0),
# SMC 0x94-0x95 and 0xa4 (feat/imac-gpu-bench, L142). A write outside it
# is reported as NEEDS-ALLOWLIST: a missing console allowlist entry, not a grammar error.
#
# Usage: check_bench.sh [file...]     (default: STEP*.bench beside this script)
# Exit: 0 clean, 1 grammar error, 3 only NEEDS-ALLOWLIST findings.

set -u
dir=$(cd "$(dirname "$0")" && pwd)
if [ $# -eq 0 ]; then set -- "$dir"/STEP*.bench; fi
files=("$@")

rc=0
need=0
for f in "${files[@]}"; do
    base=$(basename "$f")
    n=0
    writes=0
    cmds=0
    while IFS= read -r line || [ -n "$line" ]; do
        n=$((n + 1))
        case "$line" in
            ''|'#'*) continue ;;
        esac
        case "$line" in
            *'#'*) echo "$base:$n GRAMMAR inline comment; put it on its own line"; rc=1; continue ;;
        esac
        words=($line)
        cmd=${words[0]}
        nargs=$((${#words[@]} - 1))
        case "$cmd" in
            ping) want=0 ;;
            pci_cfg|bar5_read|bar0_read) want=1 ;;
            bar5_write|bar0_write) want=2 ;;
            bar5_rmw) want=3 ;;
            bar5_poll) want=4 ;;
            *) echo "$base:$n GRAMMAR unknown command '$cmd'"; rc=1; continue ;;
        esac
        if [ "$nargs" -ne "$want" ]; then
            echo "$base:$n GRAMMAR $cmd takes $want argument(s), got $nargs"; rc=1; continue
        fi
        bad=0
        vals=()
        for a in "${words[@]:1}"; do
            v=$(printf '%d' "$a" 2>/dev/null) || { bad=1; break; }
            vals+=("$v")
        done
        if [ "$bad" -eq 1 ]; then
            echo "$base:$n GRAMMAR non-numeric argument in: $line"; rc=1; continue
        fi
        cmds=$((cmds + 1))
        case "$cmd" in
            pci_cfg|bar5_read|bar0_read|bar5_write|bar0_write|bar5_rmw|bar5_poll)
                if [ "${vals[0]}" -lt 0 ] || [ "${vals[0]}" -gt 65535 ]; then
                    echo "$base:$n GRAMMAR index ${words[1]} outside 0..65535"; rc=1; continue
                fi ;;
        esac
        case "$cmd" in
            bar5_write|bar5_rmw)
                writes=$((writes + 1))
                idx=${vals[0]}
                if { [ "$idx" -ge $((0x4800)) ] && [ "$idx" -le $((0x4900)) ]; } ||
                   { [ "$idx" -ge $((0x4A00)) ] && [ "$idx" -le $((0x4C00)) ]; } ||
                   { [ "$idx" -ge $((0x1a04)) ] && [ "$idx" -le $((0x1a12)) ]; } ||
                   { [ "$idx" -ge $((0x94)) ] && [ "$idx" -le $((0x95)) ]; } || [ "$idx" -eq $((0xa4)) ]; then
                    :
                else
                    printf '%s:%s NEEDS-ALLOWLIST %s (BAR5 index 0x%x is outside the console allowlist, see the header of this script)\n' \
                        "$base" "$n" "$cmd" "$idx"
                    need=1
                fi ;;
        esac
    done < "$f"
    echo "$base: $cmds command(s), $writes BAR5 write(s)"
done

if [ "$rc" -ne 0 ]; then exit 1; fi
if [ "$need" -ne 0 ]; then exit 3; fi
exit 0
