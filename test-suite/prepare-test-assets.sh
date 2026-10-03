#!/usr/bin/env bash
#
# prepare-test-assets.sh — stage models, images and compiled tools into
# test-suite/assets/.
#
# The .rknn models, sample images and RKNN-Toolkit2 demo binaries are
# third-party material (https://github.com/airockchip/rknn-toolkit2). This
# script copies them out of your own toolkit checkout and builds the rest from
# source; the toolkit is not a submodule of this repository.
#
# Usage:
#   ./prepare-test-assets.sh [--toolkit /path/to/rknn-toolkit2]
#                            [--skip-demos] [--force]
#
#   --toolkit PATH   rknn-toolkit2 checkout. Defaults to $RKNN_TOOLKIT2, or a
#                    rknn-toolkit2 directory next to this repo / inside it.
#   --skip-demos     only stage models/images and build the driver tests
#   --force          rebuild even if the output already exists

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
ASSETS="$ROOT/assets"
TOOLKIT="${RKNN_TOOLKIT2:-}"
SKIP_DEMOS=0
FORCE=0
TARGET_SOC=rk3588
TARGET_ARCH=aarch64
GCC_COMPILER="${GCC_COMPILER:-aarch64-linux-gnu-}"

while [ $# -gt 0 ]; do
	case "$1" in
	--toolkit)    TOOLKIT="$2"; shift 2 ;;
	--skip-demos) SKIP_DEMOS=1; shift ;;
	--force)      FORCE=1; shift ;;
	-h|--help)    sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 1 ;;
	esac
done

die() { echo "ERROR: $*" >&2; exit 1; }

# Resolve the toolkit relative to this script — never a baked-in absolute path.
# Look, in order, for a rknn-toolkit2 checkout next to the repo, inside the
# repo, and inside this test-suite; otherwise fall back to the sibling name.
if [ -z "$TOOLKIT" ]; then
	for candidate in "$REPO/../rknn-toolkit2" "$REPO/rknn-toolkit2" \
	                 "$ROOT/rknn-toolkit2"; do
		if [ -d "$candidate/rknpu2/examples" ]; then
			TOOLKIT="$candidate"
			break
		fi
	done
fi
TOOLKIT="${TOOLKIT:-$REPO/../rknn-toolkit2}"
if [ -d "$TOOLKIT" ]; then
	TOOLKIT="$(cd "$TOOLKIT" && pwd)"
fi

EX="$TOOLKIT/rknpu2/examples"
RT_DIR="$TOOLKIT/rknpu2/runtime/Linux/librknn_api/$TARGET_ARCH"
[ -d "$EX" ] || die "toolkit examples not found under $TOOLKIT (set --toolkit)"
[ -f "$RT_DIR/librknnrt.so" ] || die "librknnrt.so not found: $RT_DIR"

if git -C "$TOOLKIT" rev-parse --git-dir >/dev/null 2>&1; then
	echo "toolkit:  $TOOLKIT @ $(git -C "$TOOLKIT" rev-parse HEAD)"
else
	echo "toolkit:  $TOOLKIT (not a git checkout)"
fi
echo "runtime:  $(grep -a -m1 -o 'librknnrt version: [0-9][0-9.]* ([^)]*)' "$RT_DIR/librknnrt.so" || echo unknown)"
echo

mkdir -p "$ASSETS"/{bin,lib,models,images,model}

copy() { # copy <src> <dst>
	local src="$1" dst="$2"
	[ -f "$src" ] || die "missing source file: $src"
	if [ -f "$dst" ] && [ "$FORCE" -eq 0 ]; then
		echo "  keep    $(basename "$dst")"
		return
	fi
	cp -f "$src" "$dst"
	echo "  copy    $dst"
}

echo "=== models / images (copied from the toolkit checkout) ==="
copy "$EX/rknn_api_demo/model/RK3588/mobilenet_v1.rknn" \
	"$ASSETS/models/mobilenet_v1.rknn"
copy "$EX/rknn_dynamic_shape_input_demo/model/RK3588/mobilenet_v2.rknn" \
	"$ASSETS/models/mobilenet_v2.rknn"
copy "$EX/rknn_yolov5_demo/model/RK3588/yolov5s-640-640.rknn" \
	"$ASSETS/models/yolov5s-640-640.rknn"
if [ -f "$EX/rknn_multiple_input_demo/model/RK3588/multiple_input_demo.rknn" ]; then
	copy "$EX/rknn_multiple_input_demo/model/RK3588/multiple_input_demo.rknn" \
		"$ASSETS/models/multiple_input_demo.rknn"
fi
copy "$EX/rknn_mobilenet_demo/model/dog_224x224.jpg" \
	"$ASSETS/images/dog_224x224.jpg"
copy "$EX/rknn_yolov5_demo/model/bus.jpg" \
	"$ASSETS/images/bus.jpg"
# rknn_yolov5_demo hard-codes ./model/coco_80_labels_list.txt and segfaults if
# it is missing (upstream bug), so keep a copy in the shape it expects.
copy "$EX/rknn_yolov5_demo/model/coco_80_labels_list.txt" \
	"$ASSETS/model/coco_80_labels_list.txt"

echo
echo "=== runtime library ==="
copy "$RT_DIR/librknnrt.so" "$ASSETS/lib/librknnrt.so"

echo
echo "=== driver-level test binaries (built from src/) ==="
build_test() { # build_test <output-name> <source> [extra cflags...]
	local out="$ASSETS/bin/$1" src="$ROOT/src/$2"
	shift 2
	[ -f "$src" ] || die "missing test source: $src"
	if [ -x "$out" ] && [ "$FORCE" -eq 0 ]; then
		echo "  keep    $(basename "$out")"
		return
	fi
	# shellcheck disable=SC2086
	gcc -std=gnu11 -O2 -Wall -Wextra -pthread \
		-I "$ROOT/../src/include" "$@" "$src" -o "$out"
	echo "  build   $out"
}

build_test gem_lifecycle_test          gem_lifecycle_test.c
build_test gem_iommu_isolation_test    gem_iommu_isolation_test.c
build_test gem_multicore_map_sync_test gem_multicore_map_sync_test.c
build_test dma_token_lookup_test       dma_token_lookup_test.c
build_test submit_validation_test      submit_validation_test.c
build_test gem_destroy_release_test    gem_destroy_release_test.c
build_test mem_sync_stress_test        mem_sync_stress_test.c

if [ -x "$ASSETS/bin/dma_fence_shim.so" ] && [ "$FORCE" -eq 0 ]; then
	echo "  keep    dma_fence_shim.so"
else
	gcc -shared -fPIC -O2 -Wall -o "$ASSETS/bin/dma_fence_shim.so" \
		"$ROOT/src/dma_fence_shim.c" -ldl
	echo "  build   $ASSETS/bin/dma_fence_shim.so"
fi

if [ -x "$ASSETS/bin/rknn_create_mem_demo" ] && [ "$FORCE" -eq 0 ]; then
	echo "  keep    rknn_create_mem_demo"
else
	g++ -std=gnu++11 -O2 -Wall -pthread \
		-I "$EX/rknn_api_demo/src" -I "$EX/rknn_api_demo/src/stb" \
		-I "$TOOLKIT/rknpu2/runtime/Linux/librknn_api/include" \
		"$ROOT/src/rknn_create_mem_demo.cpp" \
		-L "$ASSETS/lib" -lrknnrt -Wl,-rpath,'$ORIGIN/../lib' \
		-o "$ASSETS/bin/rknn_create_mem_demo"
	echo "  build   $ASSETS/bin/rknn_create_mem_demo"
fi

echo
echo "=== offline test helpers (need no device) ==="
python3 "$ROOT/scripts/test-gem-range-geometry.py" --output "$ASSETS/bin/gem-range" >/dev/null \
	&& echo "  pass    gem range geometry"
python3 "$ROOT/scripts/test-gem-token-lifetime.py" --output "$ASSETS/bin/gem-token" >/dev/null \
	&& echo "  pass    gem token lifetime"

if [ "$SKIP_DEMOS" -eq 0 ]; then
	echo
	echo "=== RKNN-Toolkit2 demo binaries (built from the toolkit checkout) ==="
	build_demo() { # build_demo <dir> <binary-name>
		local dir="$EX/$1" name="$2"
		[ -d "$dir" ] || die "missing demo dir: $dir"
		if [ -x "$ASSETS/bin/$name" ] && [ "$FORCE" -eq 0 ]; then
			echo "  keep    $name"
			return
		fi
		echo "  build   $name (this can take a while)"
		( cd "$dir" && GCC_COMPILER="$GCC_COMPILER" ./build-linux.sh \
			-t "$TARGET_SOC" -a "$TARGET_ARCH" -b Release >/dev/null )
		local found
		found="$(find "$dir/install" "$dir/build" -type f -name "$name" 2>/dev/null | head -n1)"
		[ -n "$found" ] || die "built $name but could not find the binary"
		cp -f "$found" "$ASSETS/bin/$name"
		echo "  copy    $ASSETS/bin/$name"
	}

	build_demo rknn_benchmark              rknn_benchmark
	build_demo rknn_mobilenet_demo         rknn_mobilenet_demo
	build_demo rknn_yolov5_demo            rknn_yolov5_demo
	build_demo rknn_matmul_api_demo        rknn_matmul_api_demo
	build_demo rknn_dynamic_shape_input_demo rknn_dynshape_inference
	build_demo rknn_dynamic_shape_input_demo rknn_dynshape_inference_zero_copy
fi

echo
echo "=== staged ==="
( cd "$ASSETS" && find bin lib models images model -type f -printf '%10s  %p\n' | sort -k2 )
echo
echo "assets ready under $ASSETS"
echo "run the official flow with:  $ROOT/run-official-demo.sh status"
