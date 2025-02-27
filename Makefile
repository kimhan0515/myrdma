obj-m += server.o client.o

#ccflags-y += -include /usr/src/mlnx-ofed-kernel-5.8/include/uapi/rdma/rdma_user_cm.h
#ccflags-y += -include /usr/src/mlnx-ofed-kernel-5.8/include/rdma/rdma_cm.h

# ccflags-y += -iquote $(PWD)/include

LINUXINCLUDE := $(subst -I, -isystem, $(LINUXINCLUDE))
ccflags-y += -I/usr/src/ofa_kernel/x86_64/5.4.0-100-generic/include

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) -I/usr/src/mlnx-ofed-kernel-5.8/include/ modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
