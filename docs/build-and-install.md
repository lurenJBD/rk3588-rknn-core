# Build, Install and Unload

## Dependencies (Debian / Ubuntu)

Standard build toolchain plus kernel headers matching the running kernel:

```sh
sudo apt update
sudo apt install -y \
    build-essential gcc make bc bison flex \
    libssl-dev libelf-dev pahole \
    linux-headers-$(uname -r)
```

Optional, for DKMS-managed builds:

```sh
sudo apt install -y dkms
```

## Build

```sh
git clone https://github.com/lurenJBD/rk3588-rknn-core.git
cd rk3588-rknn-core
make -j$(nproc)
```

`rknpu.ko` is produced in the repository root.

## Install

Install into `/lib/modules/$(uname -r)/extra/` and refresh the module index:

```sh
sudo make install
sudo depmod -a
```

## Load and verify

```sh
sudo modprobe rknpu

sudo dmesg | grep -i rknpu          # probe log
modinfo rknpu                        # module info / srcversion
cat /sys/class/devfreq/fdab0000.npu/cur_freq   # devfreq node (200-800 MHz)
cat /sys/kernel/debug/rknpu/load               # per-core load
```

A successful probe registers three cores, the DRM device and the devfreq
device. Confirm the running module matches the source you built by comparing
`srcversion` from `modinfo` with `/sys/module/rknpu/srcversion`.

## Clean unload

```sh
sudo rmmod rknpu
cat /proc/sys/kernel/tainted   # should stay 4096 (OOT module only)
```

The driver balances clocks, regulators and workqueues on unload; `taint` must
not gain `DIE`/`BUG` bits (i.e. stay at `4096`, not `4608` or higher).

## DKMS

A DKMS package for this driver is maintained separately at
<https://github.com/lurenJBD/rknpu-mainline-dkms>, which pins this repository as
a submodule. Prefer that package if you want `dkms` to rebuild the module on
kernel upgrades.
