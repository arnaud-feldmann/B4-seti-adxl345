ifneq ($(KERNELRELEASE),)
# kbuild part of Makefile
obj-m := adxl345.o

else
# normal Makefile
CROSS_COMPILE ?= arm-linux-gnueabihf-
ARCH ?= arm
KDIR ?= ../linux-6.13.1/build/

all: adxl345.ko main

adxl345.ko: adxl345.c
	$(MAKE) -C $(KDIR) M=$$PWD ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

main: main.c
	arm-linux-gnueabihf-gcc -Wall -lpthread -I$(KDIR)/usr/include -L$(KDIR)/usr/lib -o main main.c

clean:
	$(MAKE) -C $(KDIR) M=$$PWD ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -rf main
endif
