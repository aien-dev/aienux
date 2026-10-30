#!/usr/bin/env bash
# rung0.sh: what NVIDIA's kernel driver costs, measured (L332 rung 0). Run INSIDE a spark-window.
#   rung0.sh OUTDIR idle LABEL SECONDS          baseline: nothing of ours runs (negative control)
#   rung0.sh OUTDIR run  LABEL -- CMD...  one workload, unprobed (time -v only)
#   rung0.sh OUTDIR probe LABEL -- CMD... same workload with perf (kernel CPU per module) + bpftrace (ioctl counts/time)
#   rung0.sh OUTDIR strace LABEL -- CMD... same workload under strace (call list and session-start timeline only)
#   rung0.sh OUTDIR attach LABEL PID SECONDS   live process (35B server): perf + bpftrace for SECONDS, no ptrace
# Read-only: sudo is used for perf/bpftrace and to read schedstat/interrupts; nothing is loaded, set or killed.
set -u
out=$1; mode=$2; shift 2
mkdir -p "$out"
snap() { # $1 = file: nvidia interrupt counts, nvidia/uvm kthread schedstat (ns on cpu), clock
  {
    echo "t_ns $(date +%s%N)"
    grep -iE "nvidia|nvgpu|000f:01:00" /proc/interrupts | awk '{s=0; for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i; else break; print "irq", $1, s, $NF}'
    for d in /proc/[0-9]*; do
      c=$(cat "$d/comm" 2>/dev/null) || continue
      case "$c" in nv_*|nvidia*|UVM*|uvm*) echo "kthread ${d#/proc/} $c $(awk '{print $1}' "$d/schedstat" 2>/dev/null)";; esac
    done
  } > "$1"
}
perf_start() { sudo perf record -a -e cpu-clock -F 99 -o "$out/$1.perf.data" -- sleep "$2" > "$out/$1.perf.log" 2>&1 & PERF_PID=$!; }
perf_report() { # kernel CPU per NVIDIA module (symbols from /proc/kallsyms), per thread name; samples at 99 Hz cpu-clock => seconds = samples/99
  sudo awk '$4 ~ /^\[nvidia/ {gsub(/[\[\]]/, "", $4); print $3, $4}' /proc/kallsyms > "$out/$1.modsyms.txt"
  sudo perf report --no-inline -g none -i "$out/$1.perf.data" --sort comm,sym --stdio --no-children -n -F sample,comm,sym 2>/dev/null \
    | grep -E "^ +[0-9]" > "$out/$1.commsym.txt"
  awk 'NR==FNR {mod[$1]=$2; next}
       { n=$1; sym=$NF; comm=$2; for (i=3; i<NF-1; i++) comm=comm "_" $i; tot+=n; m=(sym in mod) ? mod[sym] : "other";
         bym[m]+=n; if (m!="other") { byc[m" "comm]+=n; bys[m" "sym]+=n } }
       END { printf "total_samples %d\n", tot; for (k in bym) printf "module %s %d\n", k, bym[k];
             for (k in byc) printf "thread %s %d\n", k, byc[k]; for (k in bys) if (bys[k] >= 3) printf "sym %s %d\n", k, bys[k] }' \
    "$out/$1.modsyms.txt" "$out/$1.commsym.txt" | sort -k1,1 -k3,3nr > "$out/$1.modules.txt"
}
bpf_start() { # $1 label, $2 = pid filter expression target (comm name or pid)
  sudo bpftrace -q -e "
tracepoint:syscalls:sys_enter_ioctl /$2/ { @start[tid] = nsecs; @cmd[tid] = args->cmd; }
tracepoint:syscalls:sys_exit_ioctl /@start[tid]/ { @n[@cmd[tid]] = count(); @ns[@cmd[tid]] = sum(nsecs - @start[tid]); delete(@start[tid]); delete(@cmd[tid]); }
tracepoint:syscalls:sys_enter_mmap /$2/ { @mmap = count(); }
tracepoint:syscalls:sys_enter_munmap /$2/ { @munmap = count(); }
" > "$out/$1.bpf.txt" 2>&1 & BPF_PID=$!
}
case "$mode" in
  idle)
    label=$1 secs=$2; snap "$out/$label.before"; perf_start "$label" "$secs"; wait "$PERF_PID"; snap "$out/$label.after"; perf_report "$label" ;;
  run|probe|strace)
    label=$1; shift; [ "$1" = "--" ] && shift
    snap "$out/$label.before"
    if [ "$mode" = probe ]; then
      bpf_start "$label" "comm == \"$(basename "$1" | cut -c1-15)\""; sleep 2
      perf_start "$label" 600
    fi
    t0=$(date +%s%N)
    if [ "$mode" = strace ]; then
      strace -f -r -T -e trace=openat,ioctl,mmap,munmap -o "$out/$label.strace" /usr/bin/time -v -o "$out/$label.time" "$@" > "$out/$label.stdout" 2>&1
    else
      /usr/bin/time -v -o "$out/$label.time" "$@" > "$out/$label.stdout" 2>&1
    fi
    rc=$?; t1=$(date +%s%N)
    snap "$out/$label.after"
    echo "exit $rc wall_ns $((t1 - t0))" > "$out/$label.result"
    if [ "$mode" = probe ]; then
      sudo pkill -INT -f "perf record -a -e cpu-clock -F 99 -o $out/$label.perf.data" ; sleep 1
      sudo kill -INT "$BPF_PID" 2>/dev/null; sleep 1; perf_report "$label"
    fi ;;
  attach)
    label=$1 pid=$2 secs=$3
    snap "$out/$label.before"
    bpf_start "$label" "pid == $pid"; sleep 2
    perf_start "$label" "$secs"; wait "$PERF_PID"
    sudo kill -INT "$BPF_PID" 2>/dev/null; sleep 1
    snap "$out/$label.after"; perf_report "$label" ;;
  *) echo "usage: see header" >&2; exit 2 ;;
esac
