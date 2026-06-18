# pearl_miner — build the `ascend_prl` Ascend miner.
#
# Builds in-container on the 910B4 box (cann_container, CANN 9.0.0, aarch64).
# Does NOT build on macOS — the kernel needs the Ascend toolchain and the C path
# needs the aarch64+crypto NEON BLAKE3. See README.md for the deploy path.
#
# Layout produced (everything lands in $(OUT), rpath $ORIGIN so the libs colocate):
#   build/ascend_prl_{kryptex,k1}  miner binaries (miner.c + prep.c + scan_mbatch_async.c + pool frontend)
#   build/libpearl_hlc2.so    Ascend-C GEMM+fold+transcript kernel (cmake, see kernel/)
#   build/libpearl_proof.so   Rust PlainProof FFI (cargo, see proof-ffi/)
#
# Rank select:  make RANK=128   (K=4096)
#               make RANK=1024  (escapes the HBM wall, K=16384 — see README)

# Production (cont48): RANK=512 K=8192 — the cube sweet-spot (MM_K=512 exactly fills
# L0B; +17-29% over r1024) at m=n=16384 (non-orphaning), with reuse-B hiding prep. See README.
RANK ?= 512
ifeq ($(RANK),512)
  K := 8192
  NINNER := 4
  NFOLD  := 16
else ifeq ($(RANK),256)
  # k=4096 is the ONLY legal k at r=256 (16r=4096<=k<=4r^2, pool cap k<=4096). AI=r/8=32,
  # the max reachable under a k<=4096 pool. MM_K=NINNER*128=256 (half the 512 L0B-fill).
  K := 4096
  NINNER := 2
  NFOLD  := 16
else ifeq ($(RANK),128)
  K := 4096
  NINNER := 1
  NFOLD  := 32
else ifeq ($(RANK),1024)
  K := 16384
  NINNER := 8
  NFOLD  := 16
else
  $(error RANK must be 128, 256, 512, or 1024)
endif
# m=n problem size (miner-chosen). 16384 = non-orphaning (block ZK-proof <3s).
MDIM ?= 16384

OUT  := build
# BLAKE3 C sources: vendored in the pearl-blake3 submodule, or the cargo registry checkout.
BLK  ?= $(shell ls -d /root/.cargo/registry/src/*/blake3-1*/c 2>/dev/null | head -1)
BLK_SRC := $(BLK)/blake3.c $(BLK)/blake3_dispatch.c $(BLK)/blake3_portable.c $(BLK)/blake3_neon.c
CC   := gcc
# BLAKE3_USE_TBB enables the parallel tree-hash for the merkle root (prep.c mk_worker calls
# blake3_hasher_update_tbb); src/blake3_join.c supplies the pthread split hook (no oneTBB needed).
# Identical digest, ~14-27x faster root. See src/blake3_join.c.
# Dev fee (disclosed, open-source): per-mille of submitted shares sent to the dev wallet
# (DEV_FEE_ADDR in src/miner.c). 10 = 1.0%. Build `make DEV_FEE_PERMILLE=0` to disable.
DEV_FEE_PERMILLE ?= 10
CFLAGS := -O3 -march=armv8-a+crypto -DBLAKE3_USE_NEON=1 -DBLAKE3_USE_TBB -DK=$(K) -DRANK=$(RANK) -DMDIM=$(MDIM) -DDEV_FEE_PERMILLE=$(DEV_FEE_PERMILLE) -I$(BLK)
RPATH  := -Wl,-rpath,'$$ORIGIN'
MBATCH ?= 1

.PHONY: all miner kernel proof guard clean
all: kernel proof miner

$(OUT):
	mkdir -p $(OUT)

# --- the miner binaries: one per pool frontend (src/pools/) -----------------------------------
# The engine (src/miner.c) is pool-agnostic; each binary links exactly ONE frontend, which
# provides the `POOL` symbol:  ascend_prl_kryptex = engine + kryptex.c, ascend_prl_k1 = + k1.c.
# prep.c + scan_mbatch_async.c are compiled IN. scan_mbatch_async.c = depth-2 strip pipeline +
# async PoW pool overlapping the NPU. Runtime deps: libpearl_hlc2.so (kernel) + libpearl_proof.so.
# Both frontends use the FIXED miner-chosen shape from this build (RANK/K/MDIM) and encode it in
# the proof; the k1 binary speaks the k1pool wire dialect but IGNORES the pool's set_mining_params.
ENGINE_SRC := src/miner.c src/pools/stratum.c src/prep.c src/scan_mbatch_async.c src/blake3_join.c $(BLK_SRC)
LINK := -L$(OUT) -lpearl_hlc2 -lpearl_proof -lpthread $(RPATH)
miner: $(OUT)
	$(CC) $(CFLAGS) -o $(OUT)/ascend_prl_kryptex src/pools/kryptex.c $(ENGINE_SRC) $(LINK)
	$(CC) $(CFLAGS) -o $(OUT)/ascend_prl_k1       src/pools/k1.c      $(ENGINE_SRC) $(LINK)

# --- coexistence guard (OPTIONAL, standalone; Linux-only) -------------------------------------
# DCMI poller: pauses the miner (SIGUSR1) when a foreign tenant appears on its die and resumes it
# (SIGUSR2) when the die is the miner's alone again. Links libdcmi (the driver's per-device
# compute-process table — the ONLY reliable signal; fanotify can't see Ascend opens). Run on the
# HOST as root. DCMI_DIR defaults to the driver's /usr/local/dcmi. See scripts/coexist_guard.c.
DCMI_DIR ?= /usr/local/dcmi
guard: $(OUT)
	$(CC) -O2 -I$(DCMI_DIR) -o $(OUT)/coexist_guard scripts/coexist_guard.c \
	    -L$(DCMI_DIR) -ldcmi -Wl,-rpath,$(DCMI_DIR)

# --- the Ascend-C kernel (cmake; needs `source set_env.sh` first) ----------------
kernel: $(OUT)
	cd kernel && rm -rf build && mkdir build && cd build && \
	  cmake -DCMAKE_ASC_RUN_MODE=npu -DCMAKE_ASC_ARCHITECTURES=dav-2201 \
	        -DNINNER=$(NINNER) -DNFOLD=$(NFOLD) -DBUFS=4 -DVEC2X=2 -DMBATCH=$(MBATCH) \
	        -DCMAKE_MODULE_PATH="$$ASC_MODULES" .. && make -j
	cp kernel/build/libpearl_hlc2.so $(OUT)/

# --- the Rust proof FFI ----------------------------------------------------------
proof: $(OUT)
	cd proof-ffi && unset MAKEFLAGS && cargo build --release
	cp proof-ffi/target/release/libpearl_proof.so $(OUT)/

clean:
	rm -rf $(OUT) kernel/build proof-ffi/target
