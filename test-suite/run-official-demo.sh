#!/usr/bin/env bash
#
# Run official RKNN Toolkit2 demos against the already-loaded DKMS rknpu.
#
# The DKMS package owns the module lifecycle. This script never runs
# insmod/rmmod/modprobe. It prints the state, switches every CPU policy and
# the NPU devfreq governor to the requested values (performance by default),
# prints the resulting state, and leaves those governors in place. NPU
# utilization can be observed separately with rkmon.
#
# Usage:
#   run-official-demo.sh [--governor name] [--cpu-governor name]
#                        [--npu-governor name] [--watch-load]
#                        [--load-interval seconds] <command> [arguments...]
#   run-official-demo.sh status
#   run-official-demo.sh benchmark [model.rknn [input [loop_count [core_mask]]]]
#     Use core_mask=7 for a three-core combined-mask workload. Add
#     --watch-load before benchmark to print live per-core debugfs load.
#   run-official-demo.sh graph [loop_count]
#   run-official-demo.sh matmul [matmul_type M,K,N B_layout AC_layout loop_count core_mask print_result iommu_domain_id]
#
# Hardware workloads require explicit user authorization.

set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
RUNTIME="${RKNN_RUNTIME:-$ROOT/assets/lib}"
DKMS_KO="/lib/modules/$(uname -r)/updates/dkms/rknpu.ko"
DEVFREQ=/sys/class/devfreq/fdab0000.npu

usage() {
	sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
}

fail() {
	echo "ABORT: $*" >&2
	exit 1
}

require_loaded_driver() {
	[ -r /sys/module/rknpu/srcversion ] || \
		fail "rknpu is not loaded; load it with 'modprobe rknpu' first"
	lsmod | grep -q '^rknpu[[:space:]]' || \
		fail "rknpu is not loaded; DKMS module is installed but not active"
}

require_runtime() {
	[ -x "$1" ] || fail "demo not found or not executable: $1"
	[ -f "$RUNTIME/librknnrt.so" ] || \
		fail "librknnrt.so not found: $RUNTIME/librknnrt.so"
}

CPU_POLICIES=()
CPU_GOVERNOR=performance
NPU_GOVERNOR=performance
WATCH_LOAD=0
LOAD_INTERVAL=1

configure_governors() {
	local policy

	CPU_POLICIES=()

	for policy in /sys/devices/system/cpu/cpufreq/policy*; do
		[ -r "$policy/scaling_governor" ] || continue
		[ -w "$policy/scaling_governor" ] || \
			fail "CPU governor is not writable: $policy/scaling_governor"
		CPU_POLICIES+=("$policy")
	done
	[ "${#CPU_POLICIES[@]}" -gt 0 ] || fail "no CPU cpufreq policies found"

	if [ -d "$DEVFREQ" ]; then
		[ -r "$DEVFREQ/governor" ] && [ -w "$DEVFREQ/governor" ] || \
			fail "NPU governor is not writable: $DEVFREQ/governor"
	else
		fail "NPU devfreq node not found: $DEVFREQ"
	fi

	grep -qw "$CPU_GOVERNOR" \
		"${CPU_POLICIES[0]}/scaling_available_governors" || \
		fail "unsupported CPU governor: $CPU_GOVERNOR"
	grep -qw "$NPU_GOVERNOR" "$DEVFREQ/available_governors" || \
		fail "unsupported NPU governor: $NPU_GOVERNOR"

	for policy in "${CPU_POLICIES[@]}"; do
		printf '%s\n' "$CPU_GOVERNOR" > "$policy/scaling_governor" || \
			fail "failed to set CPU governor: $policy/scaling_governor"
	done
	printf '%s\n' "$NPU_GOVERNOR" > "$DEVFREQ/governor" || \
		fail "failed to set NPU governor: $DEVFREQ/governor"

	echo
	echo "=== state for the run ==="
	echo "requested CPU governor: $CPU_GOVERNOR"
	echo "requested NPU governor: $NPU_GOVERNOR"
	show_performance_state
}

show_npu_irqs() {
	echo "NPU IRQs:"
	grep -iE 'rknpu|npu' /proc/interrupts 2>/dev/null || \
		echo "  unavailable (no matching IRQ label)"
}

show_performance_state() {
	local policy governor frequency maximum

	echo "CPU:"
	for policy in /sys/devices/system/cpu/cpufreq/policy*; do
		[ -r "$policy/scaling_governor" ] || continue
		governor=$(cat "$policy/scaling_governor" 2>/dev/null || echo unavailable)
		frequency=$(cat "$policy/scaling_cur_freq" 2>/dev/null || echo unavailable)
		maximum=$(cat "$policy/cpuinfo_max_freq" 2>/dev/null || echo unavailable)
		printf '  %-9s governor=%s cur_freq=%s max_freq=%s\n' \
			"$(basename "$policy")" "$governor" "$frequency" "$maximum"
	done

	echo "NPU:"
	echo "  governor=$(cat "$DEVFREQ/governor" 2>/dev/null || echo unavailable)"
	echo "  cur_freq=$(cat "$DEVFREQ/cur_freq" 2>/dev/null || echo unavailable)"
	echo "  min_freq=$(cat "$DEVFREQ/min_freq" 2>/dev/null || echo unavailable)"
	echo "  max_freq=$(cat "$DEVFREQ/max_freq" 2>/dev/null || echo unavailable)"
	echo "  debugfs_freq=$(cat /sys/kernel/debug/rknpu/freq 2>/dev/null || echo unavailable)"
	echo "  load=$(cat /sys/kernel/debug/rknpu/load 2>/dev/null || echo unavailable)"
}

load_monitor() {
	local demo_pid=$1
	local stamp load freq

	echo "=== live NPU load monitor (interval=${LOAD_INTERVAL}s) ==="
	echo "Stop with Ctrl-C; the workload remains bounded by timeout 60s."
	while kill -0 "$demo_pid" 2>/dev/null; do
		stamp=$(date '+%H:%M:%S')
		load=$(cat /sys/kernel/debug/rknpu/load 2>/dev/null || echo unavailable)
		freq=$(cat "$DEVFREQ/cur_freq" 2>/dev/null || echo unavailable)
		printf '[%s] npu_freq=%s load=%s\n' "$stamp" "$freq" "$load"
		sleep "$LOAD_INTERVAL"
	done
}

run_demo() {
	local demo=$1
	shift

	require_runtime "$demo"
	local taint
	taint=$(cat /proc/sys/kernel/tainted) || fail "cannot read kernel taint"
	case "$taint" in
	0|4096) ;;
	*) fail "kernel tainted=$taint; investigate before hardware testing" ;;
	esac
	local rc demo_pid monitor_pid

	echo "=== state before applying the requested governors ==="
	show_performance_state
	show_npu_irqs
	configure_governors
	echo "runtime: $RUNTIME/librknnrt.so"
	printf 'command: %q' "$demo"
	printf ' %q' "$@"
	printf '\n\n'

	export LD_LIBRARY_PATH="$RUNTIME"
	if [ "$WATCH_LOAD" -eq 1 ]; then
		timeout 60s "$demo" "$@" &
		demo_pid=$!
		load_monitor "$demo_pid" &
		monitor_pid=$!
		wait "$demo_pid"
		rc=$?
		kill "$monitor_pid" 2>/dev/null || true
		wait "$monitor_pid" 2>/dev/null || true
	else
		timeout 60s "$demo" "$@"
		rc=$?
	fi

	echo
	echo "=== state after the run (governors left as requested) ==="
	show_performance_state
	show_npu_irqs
	return "$rc"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--watch-load)
		WATCH_LOAD=1
		shift
		;;
	--load-interval)
		[ $# -ge 2 ] || fail "--load-interval requires seconds"
		LOAD_INTERVAL=$2
		[[ "$LOAD_INTERVAL" =~ ^[0-9]+([.][0-9]+)?$ ]] &&
			[[ "$LOAD_INTERVAL" =~ [1-9] ]] ||
			fail "--load-interval must be a positive number"
		shift 2
		;;
	--governor)
		[ $# -ge 2 ] || fail "--governor requires a value"
		CPU_GOVERNOR=$2
		NPU_GOVERNOR=$2
		shift 2
		;;
	--cpu-governor)
		[ $# -ge 2 ] || fail "--cpu-governor requires a value"
		CPU_GOVERNOR=$2
		shift 2
		;;
	--npu-governor)
		[ $# -ge 2 ] || fail "--npu-governor requires a value"
		NPU_GOVERNOR=$2
		shift 2
		;;
	*)
		break
		;;
	esac
done

mode=${1:-}
[ $# -gt 0 ] && shift

case "$mode" in
	status)
		require_loaded_driver
		show_performance_state
		;;
	benchmark)
		require_loaded_driver
		model=${1:-"$ROOT/assets/models/mobilenet_v1.rknn"}
		input=${2:-"$ROOT/assets/images/dog_224x224.jpg"}
		loop_count=${3:-100}
		core_mask=${4:-0}
		case "$core_mask" in
		0|1|2|3|4|7|65535) ;;
		*) fail "unsupported core_mask=$core_mask; use 0/1/2/3/4/7/65535" ;;
		esac
		run_demo "$ROOT/assets/bin/rknn_benchmark" \
			"$model" "$input" "$loop_count" "$core_mask"
		;;
	graph)
		require_loaded_driver
		loop_count=${1:-100}
		run_demo "$ROOT/assets/bin/rknn_create_mem_demo" \
			"$ROOT/assets/models/mobilenet_v1.rknn" \
			"$ROOT/assets/images/dog_224x224.jpg" "$loop_count"
		;;
	matmul)
		require_loaded_driver
		defaults=(2 4,64,32 0 0 1 1 1 0)
		if [ $# -eq 0 ]; then
			set -- "${defaults[@]}"
		elif [ $# -ne 8 ]; then
			fail "matmul expects 8 arguments"
		fi
		run_demo "$ROOT/assets/bin/rknn_matmul_api_demo" "$@"
		;;
	''|-h|--help|help)
		usage
		;;
	*)
		fail "unknown mode: $mode (use --help)"
		;;
esac
