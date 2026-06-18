#!/bin/bash
# Full in-container build of `ascend_prl`. Run INSIDE cann_container (CANN 9.0.0) from the
# repo root, after the repo has been docker-cp'd in. Builds kernel -> proof -> miner.
#
#   RANK=128  -> K=4096 (default).  RANK=1024 -> escape the HBM wall.
set -e
RANK="${RANK:-128}"
source /usr/local/Ascend/cann-9.0.0/set_env.sh
export ASC_MODULES=/usr/local/Ascend/cann-9.0.0/aarch64-linux/tikcpp/ascendc_kernel_cmake/asc_modules

# container clock skews ~200s vs the host; touch so make/cmake don't see future mtimes
find . -type f -exec touch {} +

make RANK="$RANK" kernel     # libpearl_hlc2.so  (Ascend-C, needs set_env.sh)
make RANK="$RANK" proof      # libpearl_proof.so (Rust, needs /root/pearl-rust checkout)
make RANK="$RANK" miner      # ascend_prl_kryptex + ascend_prl_k1

echo "=== built (RANK=$RANK) ==="
ls -la build/
