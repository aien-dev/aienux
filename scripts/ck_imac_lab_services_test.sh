#!/usr/bin/env bash
# ck_imac_lab_services_test.sh -- host test for the pid ownership check in ck_imac_lab_services.sh.
# The launcher is sourced with IMAC_LAB_STATE in a temp dir, make/cc/sudo/ss are shadowed by
# functions, and the three daemons are replaced by a tiny C stub that only waits for a signal, so
# nothing real is built, bound, started or stopped. The cases: an unrelated live pid in a pid file
# is not ours (status says not running, stop leaves it alive); a started stub is recognised, start
# again does not launch a second one, and a deleted binary does not matter; a changed start time or
# boot id, a pid file from an older launcher older than the process, a dead pid, a pid whose program
# has another name, and junk or zero in the pid file are all refused; an older launcher's pid file
# for the real stub is still accepted; a foreign receiver on UDP 47031 is left alone.
# Final line: AIENOS_IMAC_LAB_SERVICES: PASS|FAIL   exit 0 / 1
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tag="AIENOS_IMAC_LAB_SERVICES"
launcher="${CK_LAB_SERVICES:-${repo_root}/scripts/ck_imac_lab_services.sh}"
W="$(mktemp -d)"
extra=()
trap 'for p in "${extra[@]}"; do kill "${p}" 2>/dev/null || true; done; for f in "${W}"/state*/*.pid; do [[ -e "${f}" ]] && read -r p <"${f}" && [[ "${p}" =~ ^[1-9][0-9]*$ ]] && kill "${p}" 2>/dev/null || true; done; rm -rf "${W}"' EXIT
fail() { echo "FAIL  $1"; echo "${tag}: FAIL"; exit 1; }
pass() { echo "PASS  $1"; }

mkdir -p "${W}/stubsrc"
printf '#include <unistd.h>\nint main(void) { for (;;) pause(); }\n' >"${W}/stubsrc/stub.c"
command cc -O2 -Wall -Wextra -Werror -o "${W}/stubsrc/stub" "${W}/stubsrc/stub.c"

export IMAC_LAB_STATE="${W}/state"
# shellcheck source=ck_imac_lab_services.sh
source "${launcher}"
[[ "${state}" == "${W}/state" ]] || fail "launcher state is ${state}, not the temp dir"
out="${W}/out"
booter="${W}/NETLDR.EFI"
mkdir -p "${out}/host"
echo booter >"${booter}"
for n in ck_imac_netboot ck_imac_report_listen ck_imac_bsdp; do cp "${W}/stubsrc/stub" "${out}/host/${n}"; done
# Nothing is built, granted capabilities or looked up on the real machine.
make() { :; }
cc() { :; }
sudo() { :; }
ss_holder=""
ss() { [[ -z "${ss_holder}" ]] || echo "${ss_holder}"; }

gone() { ! proc_ticks "$1" >/dev/null; }
wait_gone() { local i; for i in $(seq 50); do gone "$1" && return 0; sleep 0.1; done; return 1; }
reset_state() { rm -f "${state}"/*.pid "${state}"/*.id; }

unrelated="${W}/unrelated"
cp /bin/sleep "${unrelated}"
"${unrelated}" 300 &
upid=$!
extra+=("${upid}")

# 1. an unrelated live process named in the pid files
reset_state
for s in serve listen bsdp; do echo "${upid}" >"${state}/${s}.pid"; done
for s in serve listen bsdp; do alive "${s}" && fail "unrelated pid accepted as ${s}"; done
pass "unrelated live pid is not treated as owned"
out_status="$(status)"
[[ "$(grep -c ': not running' <<<"${out_status}")" == 3 ]] || fail "status shows unrelated pid as running: ${out_status}"
pass "status rejects the unrelated pid"
stop >"${W}/stop.out"
kill -0 "${upid}" 2>/dev/null && ! gone "${upid}" || fail "stop killed the unrelated process"
grep -q 'left alone' "${W}/stop.out" || fail "stop did not say it left the pid alone"
[[ ! -e "${state}/serve.pid" ]] || fail "stale pid file kept"
pass "stop leaves the unrelated process alive and drops the stale pid file"

# 2. a started set of daemons is recognised, a second start adds nothing, a deleted binary still counts
start >"${W}/start1.out"
declare -A pid1
for s in serve listen bsdp; do
    [[ -s "${state}/${s}.pid" && -s "${state}/${s}.id" ]] || fail "start left no pid/id for ${s}"
    pid1[${s}]="$(pid_of "${s}")"
    alive "${s}" || fail "started ${s} (pid ${pid1[${s}]}) not recognised"
done
pass "started daemons are recognised"
start >"${W}/start2.out"
[[ "$(grep -c 'already running' "${W}/start2.out")" == 3 ]] || fail "second start did not report all three running"
for s in serve listen bsdp; do [[ "$(pid_of "${s}")" == "${pid1[${s}]}" ]] || fail "second start replaced ${s}"; done
pass "second start launches no competing instance"
rm -f "${out}/host/ck_imac_bsdp"
alive bsdp || fail "deleted (replaced) binary no longer recognised"
pass "a replaced binary still counts as the owned daemon"

# 3. changed identity is refused, restoring it is accepted
good="$(<"${state}/serve.id")"
read -r gboot gticks <<<"${good}"
echo "${gboot} $((gticks + 1))" >"${state}/serve.id"
alive serve && fail "changed start time accepted"
echo "00000000-0000-0000-0000-000000000000 ${gticks}" >"${state}/serve.id"
alive serve && fail "changed boot id accepted"
echo "${good}" >"${state}/serve.id"
alive serve || fail "restored identity refused"
pass "changed start time or boot id is refused, the valid identity is accepted"

# 4. a pid file from an older launcher (no .id)
rm -f "${state}/listen.id"
alive listen || fail "older launcher's pid file for the live daemon refused"
touch -d '1 hour ago' "${state}/listen.pid"
alive listen && fail "older launcher's pid file accepted for a process that started after it"
touch "${state}/listen.pid"
alive listen || fail "fresh older-style pid file refused"
pass "older one-integer pid files: live daemon accepted, later-started process refused"

# 5. a process with another program name, a dead pid and junk
echo "${pid1[bsdp]}" >"${state}/serve.pid"
rm -f "${state}/serve.id"
alive serve && fail "bsdp process accepted as serve"
echo "${pid1[serve]}" >"${state}/serve.pid"
echo "${good}" >"${state}/serve.id"
cp "${W}/stubsrc/stub" "${W}/ck_imac_bsdp"
"${W}/ck_imac_bsdp" &
dpid=$!
until [[ "$(tr '\0' ' ' <"/proc/${dpid}/cmdline")" == "${W}/ck_imac_bsdp "* ]]; do sleep 0.05; done # exec done
kill "${dpid}"; wait "${dpid}" 2>/dev/null || true
echo "${dpid}" >"${state}/bsdp.pid"
alive bsdp && fail "dead pid accepted"
for junk in "" 0 -1 1 12x "1 2" "${upid}"; do
    echo "${junk}" >"${state}/bsdp.pid"
    alive bsdp && fail "junk pid file '${junk}' accepted"
done
pass "another program name, a dead pid and junk pid files are refused"

# 6. stop ends only the owned daemons
echo "${pid1[bsdp]}" >"${state}/bsdp.pid"
echo "${good}" >"${state}/serve.id"
stop >/dev/null
for s in serve listen bsdp; do wait_gone "${pid1[${s}]}" || fail "stop left ${s} running"; done
kill -0 "${upid}" 2>/dev/null && ! gone "${upid}" || fail "stop killed the unrelated process"
pass "stop ends the owned daemons and nothing else"

# 7. a foreign receiver on UDP 47031 is left as is
reset_state
ss_holder='users:(("other",pid=1,fd=3))'
cp "${W}/stubsrc/stub" "${out}/host/ck_imac_bsdp"
start >"${W}/start3.out"
grep -q 'another receiver already holds' "${W}/start3.out" || fail "foreign receiver not detected"
[[ ! -e "${state}/listen.pid" ]] || fail "listen started next to a foreign receiver"
pass "a foreign receiver on UDP 47031 is left alone"
ss_holder=""
stop >/dev/null

echo "${tag}: PASS"
