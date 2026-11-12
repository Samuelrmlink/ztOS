obj-m += zrn-hrm-probe.o

# make KDIR=<kernel source> KOUT=<its O= dir for the running kernel>
KDIR ?= /home/samuel/git-clone/ztOS/build/linux-7.1-dp
KOUT ?= $(KDIR)/out-target
TC ?= /home/samuel/git-clone/ztOS/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin

all:
	PATH="$(TC):$$PATH" $(MAKE) -C $(KDIR) O=$(KOUT) ARCH=arm64 \
		CROSS_COMPILE=aarch64-none-linux-gnu- KBUILD_MODPOST_WARN=1 \
		M=$(CURDIR) modules

clean:
	rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order
