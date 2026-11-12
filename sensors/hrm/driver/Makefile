obj-m += zrn-hrm.o

# zrn-i2c20.h is a copy of research/nfc's nfc/driver/zrn-i2c20.h (a65b476);
# zrn-i2c20.ko itself is built from that branch. Point I2C20_SYMVERS at its
# Module.symvers to link the symbols cleanly.
KDIR ?= /home/samuel/git-clone/ztOS/build/linux-7.1-dp
KOUT ?= $(KDIR)/out-target
TC ?= /home/samuel/git-clone/ztOS/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
I2C20_SYMVERS ?=

all:
	PATH="$(TC):$$PATH" $(MAKE) -C $(KDIR) O=$(KOUT) ARCH=arm64 \
		CROSS_COMPILE=aarch64-none-linux-gnu- KBUILD_MODPOST_WARN=1 \
		KBUILD_EXTRA_SYMBOLS="$(I2C20_SYMVERS)" M=$(CURDIR) modules

clean:
	rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order
