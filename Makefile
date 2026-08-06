ms912x-y := \
	ms912x_registers.o \
	ms912x_connector.o \
	ms912x_transfer.o \
	ms912x_drv.o 

obj-m := ms912x.o

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build
CURDIR := $(shell pwd)

USE_SSE2 ?= 0

ifeq ($(USE_SSE2),1)
	CFLAGS_EXTRA += -msse2
endif

EXTRA_CFLAGS += $(CFLAGS_EXTRA)

all:	modules

modules:
	$(MAKE) CHECK="/usr/bin/sparse" -C $(KSRC) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KSRC) M=$(CURDIR) clean
	rm -f $(CURDIR)/Module.symvers $(CURDIR)/*.ur-safe
	
.PHONY: all modules clean
