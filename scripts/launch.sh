#!/bin/bash
# Launch one miner per visible Ascend NPU, each pinned to its OWN NUMA node's CPUs (detected from
# sysfs), with prep+PoW thread-budgeted to the node (avoids cross-node oversubscription).
# Generic: works on any NUMA/PCI layout. Placement policy per device:
#   1. the NPU's home NUMA node (from /sys/bus/pci/.../numa_node);
#   2. if that node is already taken (by an earlier device here, or another running miner), the
#      nearest FREE node by NUMA distance — i.e. the home node's sibling;
#   3. round-robin across nodes if the home node can't be determined.
#
# Env:  WALLET (required)  POOL (default 127.0.0.1)  PORT (default 7000)
#       ASCEND_RT_VISIBLE_DEVICES (comma list; default = all NPUs)  WORKER (default npu)
#       BIN (default: first of ./build/ascend_prl_kryptex, _k1, ascend_prl that exists)
#       PRL_PIN_AVOID_RE (regex of already-running miners whose nodes to avoid; default below)
set -u
cd "$(dirname "$0")/.."
: "${WALLET:?set WALLET=prl1...}"
POOL="${POOL:-127.0.0.1}"; PORT="${PORT:-7000}"
WORKER="${WORKER:-npu}"
PASS="${PASS:-x}"          # pool password field; some pools encode difficulty here, e.g. 'x;d=20000'
# Explicit per-device CPU pin map, OVERRIDES the NUMA auto-detect below: "dev:cpulist dev:cpulist".
# Pin each NPU to cores on (or nearest to) its HOME NUMA node so H2D/D2H DMA stays local — a
# cross-socket pin keeps DMA off its home node. Verify the auto-detect with the numa audit; if it's wrong
# for your box, set this. Example (8×910B4 Kunpeng, 2 NPUs/home-node spilling to the sibling node):
#   PRL_PIN_MAP="0:192-223 1:224-255 2:128-159 3:160-191 4:0-31 5:32-63 6:64-95 7:96-127"
declare -A PINMAP
if [ -n "${PRL_PIN_MAP:-}" ]; then for kv in $PRL_PIN_MAP; do PINMAP[${kv%%:*}]=${kv#*:}; done; fi
# binaries are per-pool frontends (ascend_prl_kryptex / ascend_prl_k1); pick one if BIN unset
if [ -z "${BIN:-}" ]; then
  for cand in ./build/ascend_prl_kryptex ./build/ascend_prl_k1 ./build/ascend_prl; do
    [ -x "$cand" ] && { BIN="$cand"; break; }
  done
  BIN="${BIN:-./build/ascend_prl_kryptex}"
fi
[ -x "$BIN" ] || { echo "miner binary not found: $BIN (build it, or run scripts/get_release.sh)"; exit 1; }
source /usr/local/Ascend/cann-*/set_env.sh 2>/dev/null

# --- which devices ---
if [ -n "${ASCEND_RT_VISIBLE_DEVICES:-}" ]; then
  IFS=',' read -ra DEVS <<< "$ASCEND_RT_VISIBLE_DEVICES"
else
  mapfile -t DEVS < <(npu-smi info -l 2>/dev/null | awk '/NPU ID/{print $NF}'); [ ${#DEVS[@]} -gt 0 ] || DEVS=(0)
fi
unset ASCEND_RT_VISIBLE_DEVICES   # we pin per-process below

# --- NUMA topology: node -> cpulist, node -> distance row (indexed by node id) ---
declare -a NODES; declare -A NODE_CPUS NODE_DIST
for nd in /sys/devices/system/node/node[0-9]*; do
  n=${nd##*/node}; NODES+=("$n")
  NODE_CPUS[$n]=$(cat "$nd/cpulist" 2>/dev/null)
  NODE_DIST[$n]=$(cat "$nd/distance" 2>/dev/null)
done
[ ${#NODES[@]} -gt 0 ] || { NODES=(0); NODE_CPUS[0]=$(nproc --all 2>/dev/null | awk '{print "0-"$1-1}'); NODE_DIST[0]=10; }

# --- the NPU's home NUMA node: full PCI BDF -> /sys numa_node (>=0), else empty ---
npu_home() {  # echo home NUMA node for NPU $1, or empty
  local id=$1 bdf nn
  bdf=$(npu-smi info -t board -i "$id" 2>/dev/null | grep -oiE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]' | head -1)
  bdf=${bdf,,}
  [ -n "$bdf" ] && nn=$(cat "/sys/bus/pci/devices/$bdf/numa_node" 2>/dev/null)
  [ -n "${nn:-}" ] && [ "$nn" -ge 0 ] 2>/dev/null && echo "$nn"
}

# --- which node owns CPU $1 (empty if none) ---
node_of_cpu() { local c=$1 n p lo hi; for n in "${NODES[@]}"; do
  IFS=',' read -ra parts <<<"${NODE_CPUS[$n]}"; for p in "${parts[@]}"; do
    lo=${p%-*}; hi=${p#*-}; [ "$c" -ge "$lo" ] 2>/dev/null && [ "$c" -le "$hi" ] 2>/dev/null && { echo "$n"; return; }
  done; done; }

# core count of a cpulist
node_cores() { local cl=$1 c=0 p; IFS=',' read -ra parts <<<"$cl"; for p in "${parts[@]}"; do
  if [[ $p == *-* ]]; then c=$((c + ${p#*-} - ${p%-*} + 1)); else c=$((c+1)); fi; done; echo "$c"; }

# --- seed BUSY nodes from other already-running miners so we spill to siblings, not collide ---
declare -A USED
ALLCORES=$(nproc --all 2>/dev/null); ALLLIST="0-$((ALLCORES-1))"
AVOID_RE="${PRL_PIN_AVOID_RE:-ascend_prl|prl_release}"
for pid in $(pgrep -f "$AVOID_RE" 2>/dev/null); do
  cal=$(awk '/Cpus_allowed_list/{print $2}' "/proc/$pid/status" 2>/dev/null)
  [ -z "$cal" ] && continue
  [ "$cal" = "$ALLLIST" ] && continue          # not pinned (whole machine) -> ignore
  bn=$(node_of_cpu "${cal%%[-,]*}"); [ -n "$bn" ] && USED[$bn]=ext
done

# --- pick a node for a device: home, else nearest FREE node by distance, else round-robin ---
pick_node() {  # $1=home (may be empty)  $2=round-robin index
  local home=$1 idx=$2 best="" bestd=99999 n d
  if [ -n "$home" ]; then
    # order candidates by NUMA distance from home; take the closest that's still free
    read -ra drow <<<"${NODE_DIST[$home]:-}"
    for n in "${NODES[@]}"; do
      [ -n "${USED[$n]:-}" ] && continue
      d=${drow[$n]:-50}; if [ "$d" -lt "$bestd" ]; then bestd=$d; best=$n; fi
    done
    [ -n "$best" ] && { echo "$best"; return; }
    echo "$home"; return            # everything taken -> share the home node
  fi
  # no home info: round-robin, preferring a free node
  for ((k=0; k<${#NODES[@]}; k++)); do n=${NODES[$(((idx+k)%${#NODES[@]}))]}; [ -z "${USED[$n]:-}" ] && { echo "$n"; return; }; done
  echo "${NODES[$((idx%${#NODES[@]}))]}"
}

echo "devices: ${DEVS[*]} | NUMA nodes: ${NODES[*]}${USED[*]:+ | busy(other miners): ${!USED[*]}}"
i=0
for d in "${DEVS[@]}"; do
  if [ -n "${PINMAP[$d]:-}" ]; then         # explicit override wins
    cpus=${PINMAP[$d]}; node="explicit"; home=""
  else
    home=$(npu_home "$d")
    node=$(pick_node "$home" "$i"); USED[$node]=self
    cpus=${NODE_CPUS[$node]:-}
    # defensive: if auto-detect produced no cpulist, fall back to the home node — NEVER run a miner
    # unpinned (its threads + DMA-pinned memory would float across sockets).
    if [ -z "$cpus" ] && [ -n "$home" ]; then cpus=${NODE_CPUS[$home]:-}; node="home$home(fallback)"; fi
  fi
  [ -z "$cpus" ] && echo "  WARNING: NPU $d UNPINNED (no NUMA info) — H2D/D2H may cross sockets, killing util"
  ncore=$( [ -n "$cpus" ] && node_cores "$cpus" || nproc )
  prep=$(( ncore*3/4 )); pow=$(( ncore/4 )); [ $prep -lt 1 ] && prep=1; [ $pow -lt 1 ] && pow=1
  pin=(); [ -n "$cpus" ] && pin=(taskset -c "$cpus")
  PRL_REUSE_B=1 PRL_PREP_THREADS=$prep PRL_POW_THREADS=$pow \
    setsid "${pin[@]}" "$BIN" "$d" "$WORKER-$d" "$POOL" "$PORT" "$WALLET" "$PASS" \
    > "prl-dev$d.log" 2>&1 < /dev/null &
  echo "  NPU $d -> node $node${home:+ (home $home)} (cpus ${cpus:-all}, prep=$prep pow=$pow) pid=$!"
  i=$((i+1)); sleep 1
done
echo "launched ${#DEVS[@]} miner(s). logs: prl-dev*.log ; stop: pkill -f '$BIN'"

# --- optional coexistence guard: yield each die to other NPU tenants (vLLM, training, ...) ----
# PRL_COEXIST_GUARD=1 starts scripts/coexist_guard alongside the miners. It polls DCMI per device
# and pauses a miner (SIGUSR1) the moment another process appears on its die, resuming (SIGUSR2)
# once the die is the miner's alone again. Runs in THIS container, so DCMI returns this namespace's
# pids (matching the miners) and the miner ACK is precise. A tenant in another container shows up as
# an unidentifiable process and still counts as foreign, so the miner yields correctly either way;
# run the container --pid=host only if you also want the guard to name that pid in its logs.
# Tunables: PRL_GUARD_POLL_MS (default 500), PRL_GUARD_ACK_S (default 8).
if [ -n "${PRL_COEXIST_GUARD:-}" ]; then
  GUARD="$(dirname "$BIN")/coexist_guard"
  if [ -x "$GUARD" ]; then
    pkill -9 -f "$(basename "$GUARD")" 2>/dev/null; sleep 1   # drop any prior guard (one per box)
    setsid "$GUARD" -p "${PRL_GUARD_POLL_MS:-500}" -t "${PRL_GUARD_ACK_S:-8}" "${DEVS[@]}" \
      > "prl-guard.log" 2>&1 < /dev/null &
    echo "coexist guard: ON (pid=$!, devices ${DEVS[*]}) -> prl-guard.log"
  else
    echo "coexist guard: REQUESTED but $GUARD not found — build it (make guard); miners run UNGUARDED"
  fi
else
  echo "coexist guard: off (PRL_COEXIST_GUARD=1 to yield dies to other NPU tenants)"
fi

# --- container entrypoint: stay in the foreground -----------------------------------
# As a container's PID 1, this script must NOT return: the miners are backgrounded
# (setsid ... &), so if PID 1 exits the kernel reaps them and the container stops. So
# when we ARE PID 1 (or PRL_FOREGROUND=1), hold the foreground and stream the logs so
# `docker logs -f` shows mining. Bare-metal `./scripts/launch.sh` (PID != 1) returns to
# the shell as before.
if [ "${PRL_FOREGROUND:-}" = 1 ] || [ "$$" -eq 1 ]; then
  trap 'echo "stopping miners..."; pkill -f "$(basename "$BIN")" 2>/dev/null; exit 0' INT TERM
  echo "foreground: holding container (docker stop / Ctrl-C to stop) — streaming logs:"
  tail -n +1 -F prl-dev*.log &
  wait
fi
