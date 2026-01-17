# Build + sign + load out-of-tree kernel module: leocc.c -> leocc.ko

obj-m += leocc.o

MODULE   := leocc
KDIR     ?= /lib/modules/$(shell uname -r)/build
PWD      := $(shell pwd)

# Signing (keys already exist)
SIGN_KEY  ?= /its/home/keys/MOK.key
SIGN_CERT ?= /its/home/keys/MOK.crt
HASH_ALG  ?= sha256
SIGN_FILE := $(KDIR)/scripts/sign-file

.PHONY: all clean sign load unload reload enable fq

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

sign: all
	$(SIGN_FILE) $(HASH_ALG) $(SIGN_KEY) $(SIGN_CERT) $(MODULE).ko

load: sign
	sudo insmod $(MODULE).ko

unload:
	- sudo rmmod $(MODULE)

reload: clean unload load

enable:
	sysctl net.ipv4.tcp_congestion_control
	sysctl net.ipv4.tcp_available_congestion_control
