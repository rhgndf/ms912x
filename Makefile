ms912x-y := \
	ms912x_registers.o \
	ms912x_connector.o \
	ms912x_transfer.o \
	ms912x_drv.o

obj-m := ms912x.o

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build

all: modules

modules:
	make CHECK="/usr/bin/sparse" -C $(KSRC) M=$(CURDIR) modules

clean:
	make -C $(KSRC) M=$(CURDIR) clean
	rm -f $(CURDIR)/Module.symvers $(CURDIR)/*.ur-safe
