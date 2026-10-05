# Kernel module build (out-of-tree).
# Usage: make [KDIR=/lib/modules/$(uname -r)/build]

obj-m += edu_pci.o

ccflags-y := -I$(src)/../include

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules compile_commands.json

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

check:
	@echo "KDIR=$(KDIR)"
	@test -d $(KDIR) || (echo "missing kernel headers for $$(uname -r)" >&2; exit 1)
	modinfo ./edu_pci.ko || true
