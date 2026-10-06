#!/usr/bin/env bash
#
# run-rk-llama-bench.sh — Run RKNPU2 Flash-Attention and embedding performance
# benchmarks on jina-embeddings-v5-small retrieval Q8_0 using rk-llama.cpp.
#
# Usage:
#   run-rk-llama-bench.sh [--compare|--quick|--devices|--microbench]
#                         [--model PATH] [--lengths "53,128,256,512,1024,2048"]
#                         [--repetitions N] [--threads N] [--build-only]
#
# Notes:
#   - Paths are resolved dynamically relative to this script directory.
#   - Execution is pinned to Cortex-A76 cores (taskset -c 4-7) with 4 threads.
#   - Hardware execution is independently verified via /proc/interrupts.
#

set -euo pipefail

SUITE_DIR="$(cd "$(dirname "$0")" && pwd)"
ASSETS_DIR="$SUITE_DIR/assets"
LLAMA_DIR="$ASSETS_DIR/rk-llama.cpp"
BUILD_DIR="$LLAMA_DIR/build"
BIN_DIR="$BUILD_DIR/bin"
BENCH_BIN="$BIN_DIR/fa-hardware-bench"
LLAMA_BENCH_BIN="$BIN_DIR/llama-bench"
LLAMA_EMBD_BIN="$BIN_DIR/llama-embedding"
RKNPU_LIB_DIR="$LLAMA_DIR/ggml/src/ggml-rknpu2/libs"
DEFAULT_MODEL="$ASSETS_DIR/models/v5-small-retrieval-Q8_0.gguf"
MODEL_URL="https://huggingface.co/jinaai/jina-embeddings-v5-text-small-retrieval-GGUF/resolve/main/v5-small-retrieval-Q8_0.gguf"

MODE="compare"
MODEL_PATH="$DEFAULT_MODEL"
LENGTHS="53,128,256,512,1024,2048"
REPETITIONS=3
THREADS=4
BUILD_ONLY=0

while [ $# -gt 0 ]; do
	case "$1" in
	--compare)     MODE="compare"; shift ;;
	--quick)       MODE="quick"; shift ;;
	--devices)     MODE="devices"; shift ;;
	--microbench)  MODE="microbench"; shift ;;
	--model)       MODEL_PATH="$2"; shift 2 ;;
	--lengths)     LENGTHS="$2"; shift 2 ;;
	--repetitions) REPETITIONS="$2"; shift 2 ;;
	--threads)     THREADS="$2"; shift 2 ;;
	--build-only)  BUILD_ONLY=1; shift ;;
	-h|--help)
		sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
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

	if [ ! -f "$LLAMA_BENCH_BIN" ] || [ ! -f "$LLAMA_EMBD_BIN" ]; then
		echo "Building llama-bench and llama-embedding via CMake..."
		(
			cd "$LLAMA_DIR"
			cmake -B build -DLLAMA_RKNPU2=ON -DCMAKE_BUILD_TYPE=Release
			cmake --build build --target llama-bench llama-embedding -j"$(nproc)"
		) || die "failed to build llama-bench / llama-embedding"
		echo "Built benchmark binaries successfully."
	fi
}

ensure_binaries

if [ "$BUILD_ONLY" -eq 1 ]; then
	echo "Build completed. Exiting as requested by --build-only."
	exit 0
fi

if [ "$MODE" = "devices" ]; then
	echo "Listing detected devices via llama-bench:"
	export LD_LIBRARY_PATH="$RKNPU_LIB_DIR:$BIN_DIR:${LD_LIBRARY_PATH:-}"
	"$LLAMA_BENCH_BIN" --list-devices
	exit 0
fi

# Ensure model exists for model-based modes
ensure_model() {
	if [ ! -f "$MODEL_PATH" ]; then
		if [ "$MODEL_PATH" = "$DEFAULT_MODEL" ]; then
			echo "Model not found at $MODEL_PATH."
			echo "Downloading jina-embeddings-v5-small (v5-small-retrieval-Q8_0.gguf)..."
			mkdir -p "$(dirname "$MODEL_PATH")"
			curl -L -C - -o "$MODEL_PATH" "$MODEL_URL" || die "failed to download model"
		else
			die "Specified model does not exist: $MODEL_PATH"
		fi
	fi
}

show_irqs_diff() {
	local label="$1"
	echo "=== $label ==="
	awk '/112:.*npu/ {printf "  IRQ %-4s (Core 0): %d\n", $1, $6+$7+$8}
	     /113:.*npu/ {printf "  IRQ %-4s (Core 1): %d\n", $1, $6+$7+$8}
	     /114:.*npu/ {printf "  IRQ %-4s (Core 2): %d\n", $1, $6+$7+$8}' /proc/interrupts
}

export LD_LIBRARY_PATH="$RKNPU_LIB_DIR:$BIN_DIR:${LD_LIBRARY_PATH:-}"

echo "========================================================"
echo " RKNPU2 Benchmark: jina-embeddings-v5-small (Q8_0)"
echo " Mode: $MODE | CPUs: 4-7 (A76 cluster) | Threads: $THREADS"
echo " Driver: rknpu ($(cat /sys/module/rknpu/srcversion)) | Taint: $TAINT"
echo "========================================================"

show_irqs_diff "NPU IRQs Before Run"

case "$MODE" in
microbench)
	echo
	echo "--- Running Flash-Attention Microbenchmark (Synthetic Tensors) ---"
	run_micro() {
		local adapt="$1"
		timeout 60s taskset -c 4-7 "$BENCH_BIN" "$adapt"
	}
	TMP_ADAPT="$(mktemp /tmp/rknpu_fa_adapt_XXXXXX.jsonl)"
	TMP_BASE="$(mktemp /tmp/rknpu_fa_base_XXXXXX.jsonl)"
	trap 'rm -f "$TMP_ADAPT" "$TMP_BASE"' EXIT

	run_micro 1 > "$TMP_ADAPT"
	run_micro 0 > "$TMP_BASE"

	echo
	echo "### Microbenchmark: Adaptive Tiles vs Baseline Fixed Tiles"
	printf "| %-10s | %-6s | %-12s | %-12s | %-9s | %-12s | %-10s |\n" \
		"Pattern" "Tokens" "Adaptive(ms)" "Baseline(ms)" "Speedup" "Max Error" "Verified"
	echo "|------------|--------|--------------|--------------|-----------|--------------|------------|"

	python3 - "$TMP_ADAPT" "$TMP_BASE" << 'EOF'
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

quick)
	ensure_model
	echo
	echo "--- Quick Run on Real Model: jina-embeddings-v5-small (N=53 tokens) ---"
	python3 - "$MODEL_PATH" "$LLAMA_BENCH_BIN" "$THREADS" << 'EOF'
import sys, os, subprocess, json

model, bench_bin, threads = sys.argv[1], sys.argv[2], sys.argv[3]
env = os.environ.copy()

def run_p53(fa):
    e = env.copy()
    e['RKNPU_FA'] = str(fa)
    cmd = ['taskset', '-c', '4-7', bench_bin, '-m', model, '-p', '53', '-n', '0',
           '-t', threads, '-b', '2048', '-ub', '2048', '-embd', '1', '-r', '2', '-o', 'jsonl']
    res = subprocess.run(cmd, env=e, capture_output=True, text=True, check=True)
    for line in res.stdout.splitlines():
        if line.strip().startswith('{'):
            return json.loads(line)
    return {}

d1 = run_p53(1)
d0 = run_p53(0)
print(f"RKNPU_FA=1 (NPU FA ON) : {d1.get('avg_ts', 0):.2f} tok/s ({d1.get('avg_ns', 0)/1e6:.2f} ms)")
print(f"RKNPU_FA=0 (NPU FA OFF): {d0.get('avg_ts', 0):.2f} tok/s ({d0.get('avg_ns', 0)/1e6:.2f} ms)")
EOF
	;;

compare)
	ensure_model
	echo
	echo "--- Real Model Benchmark: jina-embeddings-v5-small (v5-small-retrieval-Q8_0.gguf) ---"
	echo "Lengths: $LENGTHS | Repetitions: $REPETITIONS | Threads: $THREADS"
	echo

	python3 - "$MODEL_PATH" "$LLAMA_BENCH_BIN" "$LLAMA_EMBD_BIN" "$LENGTHS" "$REPETITIONS" "$THREADS" << 'EOF'
import sys, os, subprocess, json, math

model, bench_bin, embd_bin, lengths_str, reps, threads = sys.argv[1:7]
env = os.environ.copy()
lengths = [int(x.strip()) for x in lengths_str.split(',') if x.strip()]

def run_bench(fa):
    e = env.copy()
    e['RKNPU_FA'] = str(fa)
    cmd = ['taskset', '-c', '4-7', bench_bin,
           '-m', model, '-p', lengths_str, '-n', '0',
           '-t', threads, '-b', '2048', '-ub', '2048',
           '-embd', '1', '-r', reps, '-o', 'jsonl']
    res = subprocess.run(cmd, env=e, capture_output=True, text=True, check=True)
    items = {}
    for line in res.stdout.splitlines():
        if line.strip().startswith('{'):
            d = json.loads(line)
            items[d['n_prompt']] = d
    return items

def check_cosine():
    def get_emb(fa):
        e = env.copy()
        e['RKNPU_FA'] = str(fa)
        cmd = ['taskset', '-c', '4-7', embd_bin,
               '-m', model,
               '-p', 'Rockchip RK3588 NPU acceleration benchmark for jina-embeddings-v5-small retrieval Q8_0 model.',
               '--pooling', 'last', '--embd-normalize', '2', '-c', '2048', '-b', '2048', '-ub', '2048', '-t', threads,
               '--embd-output-format', 'array']
        res = subprocess.run(cmd, env=e, capture_output=True, text=True, check=True)
        out = res.stdout.strip()
        idx = out.find('[[')
        return json.loads(out[idx:])[0]
    v1 = get_emb(1)
    v0 = get_emb(0)
    dot = sum(a*b for a,b in zip(v1, v0))
    cos = dot / (math.sqrt(sum(a*a for a in v1)) * math.sqrt(sum(b*b for b in v0)))
    return cos

print("Running pass with RKNPU_FA=1 (NPU Flash Attention ON)...")
on = run_bench(1)
print("Running pass with RKNPU_FA=0 (NPU Flash Attention OFF / CPU FA)...")
off = run_bench(0)
print("Verifying embedding vector numerical similarity...")
cos = check_cosine()

print("\n### Performance Comparison: jina-embeddings-v5-small (Q8_0)")
print()
printf_hdr = "| {:<6} | {:<18} | {:<15} | {:<19} | {:<16} | {:<9} | {:<8} |"
print(printf_hdr.format("Tokens", "NPU FA ON (tok/s)", "NPU FA ON (ms)", "NPU FA OFF (tok/s)", "NPU FA OFF (ms)", "Speedup", "Status"))
print("|--------|--------------------|-----------------|---------------------|------------------|-----------|----------|")

for p in lengths:
    d_on = on.get(p)
    d_off = off.get(p)
    if d_on and d_off:
        ts_on = d_on['avg_ts']
        ms_on = d_on['avg_ns'] / 1e6
        ts_off = d_off['avg_ts']
        ms_off = d_off['avg_ns'] / 1e6
        sp = ts_on / ts_off if ts_off > 0 else 1.0
        stat = "PASS" if math.isfinite(ts_on) else "FAIL"
        print(f"| {p:<6} | {ts_on:>18.2f} | {ms_on:>15.2f} | {ts_off:>19.2f} | {ms_off:>16.2f} | {sp:>8.2f}x | {stat:<8} |")

print()
cos_status = "PASS" if cos >= 0.996 else "FAIL"
print(f"Embedding Cosine Similarity: {cos:.6f} (Threshold >= 0.996: {cos_status})")
EOF
	;;
esac

echo
show_irqs_diff "NPU IRQs After Run"
echo
echo "Benchmark completed successfully."
