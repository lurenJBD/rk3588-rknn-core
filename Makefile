# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build of the Rockchip RKNPU driver for mainline three-core DT.

obj-m += rknpu.o

rknpu-y += src/rknpu_drv.o
rknpu-y += src/rknpu_core.o
rknpu-y += src/rknpu_job.o
rknpu-y += src/rknpu_gem.o
rknpu-y += src/rknpu_iommu.o
rknpu-y += src/rknpu_reset.o
rknpu-y += src/rknpu_fence.o
rknpu-y += src/rknpu_debugger.o
rknpu-y += src/rknpu_devfreq.o

ccflags-y += -I$(src)/src/include -I$(src)/include

SRC := $(shell pwd)
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules

install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) clean
