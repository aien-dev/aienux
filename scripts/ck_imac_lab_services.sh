#!/usr/bin/env bash
# ck_imac_lab_services.sh -- keep the iMac lab services running on the Spark:
#   ck_imac_netboot serve   answers NETLDR.EFI on the iMac (UDP 47032) with queued kernels
#   ck_imac_report_listen   collects the lab kernel's UNAUTHENTICATED report (UDP 47031)
#   ck_imac_bsdp            answers the iMac's Apple NetBoot (N key held, UDP 67) and hands it
#                           NETLDR.EFI over TFTP (UDP 69): the iMac starts with no stick at all
# Detached with setsid/nohup (no systemd). For a restart after a Spark reboot, a crontab line:
#   @reboot <repo>/scripts/ck_imac_lab_services.sh start
#
#   scripts/ck_imac_lab_services.sh start|stop|status
#
# State (spool, logs, reports, pids) lives in ${IMAC_LAB_STATE:-~/.local/state/aienos/imac-netboot}:
#   next.job (queued by scripts/ck_imac_netboot_push.sh), serve.log, reports/listen.log.
# Binaries are built into target/native-kernel-imac-netboot. IMAC_LAB_BCAST (default
# 192.168.1.255) is where server answers go; IMAC_LAB_MAC limits answers to one loader.
#
# Ownership: a pid file is one integer, and after a reboot or a long uptime that number can name an
# unrelated process. A pid counts as ours only when /proc says the live process is that daemon:
# its argv[0] names the expected program (a rebuilt or deleted binary does not matter), its state is
# not zombie, and its start time matches <name>.id (boot id + start ticks, written by start). A pid
# file without <name>.id (written by an older launcher) is accepted only when the process cannot
# have started after the pid file was written. Anything else is "not running" and is never killed.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
state="${IMAC_LAB_STATE:-${HOME}/.local/state/aienos/imac-netboot}"
out="${repo_root}/target/native-kernel-imac-netboot"
bcast="${IMAC_LAB_BCAST:-192.168.1.255}"
imac_mac="${IMAC_LAB_IMAC_MAC:-78:7b:8a:bd:d0:51}"
server_ip="${IMAC_LAB_SERVER_IP:-192.168.1.108}"
router="${IMAC_LAB_ROUTER:-192.168.1.1}"
# This LAN filters UDP 67 -> 68; the iMac accepts BSDP answers from 1067.
reply_source_port="${IMAC_LAB_BSDP_REPLY_SOURCE_PORT:-1067}"
booter="${IMAC_LAB_BOOTER:-${out}/NETLDR.EFI}"
mkdir -p "${state}/reports"

pid_of() { local p=""; [[ -r "${state}/$1.pid" ]] && read -r p <"${state}/$1.pid" || true; echo "${p}"; }
boot_id() { cat /proc/sys/kernel/random/boot_id; }
daemon_prog() { case "$1" in serve) echo ck_imac_netboot ;; listen) echo ck_imac_report_listen ;; bsdp) echo ck_imac_bsdp ;; esac; }

# Start time of a live, non-zombie pid in clock ticks after boot (/proc/PID/stat field 22); fails if none.
proc_ticks() {
    local stat rest
    { read -r stat <"/proc/$1/stat"; } 2>/dev/null || return 1
    rest="${stat##*) }" # the command name may hold spaces and parens; fields resume after the last ") "
    set -- ${rest}
    [[ "$1" != Z && "$1" != X ]] || return 1
    echo "${20}"
}

# owned_pid NAME PID: is PID the live daemon this launcher started for NAME?
owned_pid() {
    local s="$1" pid="$2" ticks argv0="" id_boot="" id_ticks="" btime="" k v hz mtime
    [[ "${pid}" =~ ^[1-9][0-9]*$ ]] || return 1
    ticks="$(proc_ticks "${pid}")" || return 1
    { IFS= read -r -d '' argv0 <"/proc/${pid}/cmdline"; } 2>/dev/null || true
    [[ "${argv0##*/}" == "$(daemon_prog "${s}")" ]] || return 1
    if [[ -r "${state}/${s}.id" ]]; then
        read -r id_boot id_ticks <"${state}/${s}.id" || true
        [[ "${id_boot}" == "$(boot_id)" && "${id_ticks}" == "${ticks}" ]]
        return
    fi
    # Pid file from an older launcher: the daemon is forked just before its pid is written, so a
    # process that started later than that (10 s of clock slack) is a reused pid, not our daemon.
    while read -r k v; do [[ "${k}" != btime ]] || btime="${v}"; done </proc/stat
    hz="$(getconf CLK_TCK)"
    mtime="$(stat -c %Y "${state}/${s}.pid")"
    (( btime + ticks / hz <= mtime + 10 ))
}

alive() { owned_pid "$1" "$(pid_of "$1")"; }

# record_owner NAME PID: remember which process this launcher started, so a reused pid is told apart later.
record_owner() {
    local ticks
    rm -f "${state}/$1.id"
    if ticks="$(proc_ticks "$2")"; then echo "$(boot_id) ${ticks}" >"${state}/$1.id"; fi
}

start() {
    make -s -C "${repo_root}/native/kernel" OUT="${out}" imac-netboot-tool >/dev/null
    mkdir -p "${out}/host"
    cc -O2 -Wall -Wextra -o "${out}/host/ck_imac_report_listen" \
        "${repo_root}/native/kernel/tools/ck_imac_report_listen.c"
    if alive serve; then echo "serve: already running (pid $(pid_of serve))"; else
        local mac=()
        [[ -z "${IMAC_LAB_MAC:-}" ]] || mac=(--mac "${IMAC_LAB_MAC}")
        setsid nohup "${out}/host/ck_imac_netboot" serve "${state}" --bcast "${bcast}" "${mac[@]}" \
            >>"${state}/serve.log" 2>&1 </dev/null &
        echo $! >"${state}/serve.pid"
        record_owner serve $!
        echo "serve: started (pid $!, log ${state}/serve.log)"
    fi
    if alive listen; then echo "listen: already running (pid $(pid_of listen))"
    elif ss -Hulpn 'sport = :47031' | grep -q .; then
        echo "listen: another receiver already holds UDP 47031, left as is: $(ss -Hulpn 'sport = :47031' | grep -o 'users:.*')"
    else
        # 30 days is the listener's longest deadline; the @reboot line or a rerun starts the next one.
        setsid nohup "${out}/host/ck_imac_report_listen" 47031 "${state}/reports" 2592000 \
            >>"${state}/reports/listen.out" 2>&1 </dev/null &
        echo $! >"${state}/listen.pid"
        record_owner listen $!
        echo "listen: started (pid $!, log ${state}/reports/listen.log)"
    fi
    if alive bsdp; then echo "bsdp: already running (pid $(pid_of bsdp))"; else
        # The booter is copied so a rebuild cannot change it under a running transfer; ports 67/69
        # are below 1024, so the binary gets CAP_NET_BIND_SERVICE (never run as root).
        install -m 0644 "${booter}" "${state}/netldr.efi"
        sudo -n setcap cap_net_bind_service=+ep "${out}/host/ck_imac_bsdp"
        setsid nohup "${out}/host/ck_imac_bsdp" serve --booter "${state}/netldr.efi" --server-ip "${server_ip}" \
            --mac "${imac_mac}" --router "${router}" --also-broadcast yes --reply-source-port "${reply_source_port}" >>"${state}/bsdp.log" 2>&1 </dev/null &
        echo $! >"${state}/bsdp.pid"
        record_owner bsdp $!
        echo "bsdp: started (pid $!, log ${state}/bsdp.log, booter sha256 $(sha256sum "${state}/netldr.efi" | cut -c1-16))"
    fi
}

stop() {
    for s in serve listen bsdp; do
        if alive "${s}"; then kill "$(pid_of "${s}")"; echo "${s}: stopped"
        elif [[ -e "${state}/${s}.pid" ]]; then echo "${s}: not running (stale pid file dropped, that pid is not our daemon and was left alone)"
        else echo "${s}: not running"; fi
        rm -f "${state}/${s}.pid" "${state}/${s}.id"
    done
}

status() {
    for s in serve listen bsdp; do
        if alive "${s}"; then echo "${s}: running (pid $(pid_of "${s}"))"; else echo "${s}: not running"; fi
    done
    echo "UDP 47031 (reports): $(ss -Hulpn 'sport = :47031' | grep -o 'users:.*' || echo 'nobody')"
    ls "${state}" | grep -E '\.job$' || echo "jobs: none"
}

# Sourced by scripts/ck_imac_lab_services_test.sh, which calls the functions above directly.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    case "${1:-}" in
    start) start ;;
    stop) stop ;;
    status) status ;;
    *) echo "usage: ck_imac_lab_services.sh start|stop|status"; exit 2 ;;
    esac
fi
