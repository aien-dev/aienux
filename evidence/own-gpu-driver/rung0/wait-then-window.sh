#!/usr/bin/env bash
# L332 rung 0: wait (5 min polls of a file, no locks held) until the coordinator's chip reservation is gone or names L332, then run window.sh.
here=$(cd "$(dirname "$0")" && pwd)
RES=$HOME/.local/state/aien-coordinator/CHIP-RESERVED
while [ -f "$RES" ] && ! head -1 "$RES" | cut -d"(" -f1 | tr " " "\n" | grep -qx L332; do sleep 300; done
exec bash "$here/window.sh"
