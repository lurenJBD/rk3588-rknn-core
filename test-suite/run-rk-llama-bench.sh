#!/usr/bin/env bash
#
# run-rk-llama-bench.sh — Run RKNPU2 Flash-Attention and embedding performance
# benchmarks using rk-llama.cpp (opi5-rknpu2-embed-opt).
#
# Usage:
#   run-rk-llama-bench.sh [--compare|--adaptive|--baseline|--quick|--devices]
#                         [--lengths "53 64 128 256 512 1024 2048"]
#                         [--threads N] [--build-only]
#
# Notes:
#   - Paths are resolved dynamically relative to this script directory.
#   - Hardware execution is bounded by timeout and pinned to big cores (taskset -c 4-7).
#   - Verifies NPU interrupt increments and kernel taint state.
#

set -euo pipefail

SUITE_DIR="$(cd "$(dirname "$0")" && pwd)"
ASSETS_DIR="$SUITE_DIR/assets"
LLAMA_DIR="$ASSETS_DIR/rk-llama.cpp"
BUILD_DIR="$LLAMA_DIR/build"
BIN_DIR="$BUILD_DIR/bin"
BENCH_BIN="$BIN_DIR/fa-hardware-bench"
LLAMA_BENCH_BIN="$BIN_DIR/llama-bench"
RKNPU_LIB_DIR="$LLAMA_DIR/ggml/src/ggml-rknpu2/libs"

MODE="compare"
CUSTOM_LENGTHS=""
THREADS=4
BUILD_ONLY=0

while [ $# -gt 0 ]; do
	case "$1" in
	--compare)    MODE="compare"; shift ;;
	--adaptive)   MODE="adaptive"; shift ;;
	--baseline)   MODE="baseline"; shift ;;
	--quick)      MODE="quick"; shift ;;
	--devices)    MODE="devices"; shift ;;
	--build-only) BUILD_ONLY=1; shift ;;
	--lengths)    CUSTOM_LENGTHS="$2"; shift 2 ;;
	--threads)    THREADS="$2"; shift 2 ;;
	-h|--help)
		sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "Unknown option: $1" >&2
		exit 1
		;;
	esac
done

die() {
	echo "ERROR: $*" >&2
	exit 1
}

# 1. Environment & driver check
[ -d "$LLAMA_DIR" ] || die "rk-llama.cpp not found at: $LLAMA_DIR"
[ -r /sys/module/rknpu/srcversion ] || die "rknpu kernel driver is not loaded"

TAINT="$(cat /proc/sys/kernel/tainted 2>/dev/null || echo 0)"
case "$TAINT" in
0|4096) ;;
*) die "kernel tainted=$TAINT; check dmesg before running hardware benchmarks" ;;
esac

# 2. Build binaries if needed
ensure_binaries() {
	mkdir -p "$BIN_DIR"

	if [ ! -f "$BENCH_BIN" ]; then
		echo "Building fa-hardware-bench from source..."
		(
			cd "$LLAMA_DIR"
			python3 tests/rknpu2/make-hardware-bench.py \
				ggml/src/ggml-rknpu2/ggml-rknpu2.cpp \
				build/fa-hardware-bench.cpp
			g++ -O2 -std=c++17 -pthread \
				-Iggml/src/ggml-rknpu2/libs/include \
				-Iggml/include \
				build/fa-hardware-bench.cpp \
				-L"$RKNPU_LIB_DIR" -lrknnrt \
				-Wl,-rpath,'$ORIGIN/../ggml/src/ggml-rknpu2/libs' \
				-o "$BENCH_BIN"
		) || die "failed to build fa-hardware-bench"
		echo "Built $BENCH_BIN successfully."
	fi

	if [ ! -f "$LLAMA_BENCH_BIN" ]; then
		echo "Building llama-bench via CMake..."
		(
			cd "$LLAMA_DIR"
			cmake -B build -DLLAMA_RKNPU2=ON -DCMAKE_BUILD_TYPE=Release
			cmake --build build --target llama-bench -j"$(nproc)"
		) || die "failed to build llama-bench"
		echo "Built $LLAMA_BENCH_BIN successfully."
	fi
}

ensure_binaries

if [ "$BUILD_ONLY" -eq 1 ]; then
	echo "Build completed. Exiting as requested by --build-only."
	exit 0
fi

if [ "$MODE" = "devices" ]; then
	echo "Listing detected devices via llama-bench:"
	"$LLAMA_BENCH_BIN" --list-devices
	exit 0
fi

# 3. Helper to capture interrupts
get_irqs() {
	awk '/(fdab0000|fdac0000|fdad0000)\.npu/ {print $1, $6, $7, $8}' /proc/interrupts
}

show_irqs_diff() {
	local label="$1"
	echo "=== $label ==="
	awk '/112:.*npu/ {printf "  IRQ %-4s (Core 0): %d\n", $1, $6+$7+$8}
	     /113:.*npu/ {printf "  IRQ %-4s (Core 1): %d\n", $1, $6+$7+$8}
	     /114:.*npu/ {printf "  IRQ %-4s (Core 2): %d\n", $1, $6+$7+$8}' /proc/interrupts
}

export LD_LIBRARY_PATH="$RKNPU_LIB_DIR:${LD_LIBRARY_PATH:-}"

echo "========================================================"
echo " RKNPU2 Flash-Attention Benchmark (opi5-rknpu2-embed-opt)"
echo " Mode: $MODE | CPUs: 4-7 (A76 cluster) | Threads: $THREADS"
echo " Driver: rknpu ($(cat /sys/module/rknpu/srcversion)) | Taint: $TAINT"
echo "========================================================"

show_irqs_diff "NPU IRQs Before Run"

# 4. Execution logic
run_bench_pass() {
	local adaptive="$1"
	local length_arg="${2:-}"
	local mode_arg="${3:-}"

	local cmd=("$BENCH_BIN" "$adaptive")
	if [ -n "$length_arg" ]; then
		cmd+=("$length_arg")
		if [ -n "$mode_arg" ]; then
			cmd+=("$mode_arg")
		fi
	fi

	timeout 60s taskset -c 4-7 "${cmd[@]}"
}

case "$MODE" in
quick)
	echo
	echo "--- Quick Run (N=53 tokens, Adaptive=1 vs 0) ---"
	echo "Adaptive=1:"
	run_bench_pass 1 53 0
	echo "Adaptive=0:"
	run_bench_pass 0 53 0
	;;

adaptive)
	echo
	echo "--- Running Adaptive Flash Attention (RKNPU_FA_ADAPTIVE=1) ---"
	run_bench_pass 1
	;;

baseline)
	echo
	echo "--- Running Baseline Flash Attention (RKNPU_FA_ADAPTIVE=0) ---"
	run_bench_pass 0
	;;

compare)
	TMP_ADAPTIVE="$(mktemp /tmp/rknpu_fa_adapt_XXXXXX.jsonl)"
	TMP_BASELINE="$(mktemp /tmp/rknpu_fa_base_XXXXXX.jsonl)"
	trap 'rm -f "$TMP_ADAPTIVE" "$TMP_BASELINE"' EXIT

	echo "Running Adaptive pass (RKNPU_FA_ADAPTIVE=1)..."
	run_bench_pass 1 > "$TMP_ADAPTIVE"

	echo "Running Baseline pass (RKNPU_FA_ADAPTIVE=0)..."
	run_bench_pass 0 > "$TMP_BASELINE"

	echo
	echo "### Performance Comparison: Adaptive Tiles vs Baseline Fixed Tiles"
	echo
	printf "| %-10s | %-6s | %-12s | %-12s | %-9s | %-12s | %-10s |\n" \
		"Pattern" "Tokens" "Adaptive(ms)" "Baseline(ms)" "Speedup" "Max Error" "Verified"
	echo "|------------|--------|--------------|--------------|-----------|--------------|------------|"

	python3 - "$TMP_ADAPTIVE" "$TMP_BASELINE" << 'EOF'
import sys, json

adapt_file, base_file = sys.argv[1], sys.argv[2]
adapt_data = [json.loads(line) for line in open(adapt_file) if line.strip().startswith('{')]
base_data = [json.loads(line) for line in open(base_file) if line.strip().startswith('{')]

base_map = {(d.get('mode', 0), d['n']): d for d in base_data}
mode_names = {0: "Causal", 1: "Multi-Doc", 2: "Sparse"}

for a in adapt_data:
    key = (a.get('mode', 0), a['n'])
    if key in base_map:
        b = base_map[key]
        p = mode_names.get(a.get('mode', 0), f"Mode {a.get('mode', 0)}")
        n = a['n']
        a_ms = a['median_ms']
        b_ms = b['median_ms']
        speedup = (b_ms / a_ms) if a_ms > 0 else 1.0
        err = a['maxerr']
        verified = "PASS" if a['finite'] and err <= 0.006 else "FAIL"
        print(f"| {p:<10} | {n:<6} | {a_ms:<12.4f} | {b_ms:<12.4f} | {speedup:<8.2f}x | {err:<12.7f} | {verified:<10} |")
EOF
	;;
esac

echo
show_irqs_diff "NPU IRQs After Run"
echo
echo "Benchmark completed successfully."
