ifeq ($(RELEASE_PACKAGE),1)
EXTRA_CFLAGS+=-DRELEASE_PACKAGE
endif
LBITS ?= $(shell getconf LONG_BIT)
ifeq ($(LBITS),64)
CCFLAGS += -m64
EXTRA_CFLAGS+=-DSYSTEM_IS_64
else
CCFLAGS += -m32
endif

M ?= $(shell pwd)

# -------------------------------------------------
# RMNET CORE (only built if CONFIG_RMNET_CORE=y/m)
# -------------------------------------------------

obj-$(CONFIG_RMNET_CORE) += rmnet_core.o

rmnet_core-y += 	rmnet_config.o \
			rmnet_descriptor.o \
			rmnet_genl.o \
			rmnet_handlers.o \
			rmnet_map_command.o \
			rmnet_map_data.o \
			rmnet_module.o \
			rmnet_vnd.o \
			dfc_qmap.o \
			dfc_qmi.o \
			qmi_rmnet.o \
			wda_qmi.o \
			rmnet_ll.o \
			rmnet_ll_ipa.o \
			rmnet_qmap.o \
			rmnet_ll_qmap.o


# -------------------------------------------------
# RMNET CTL (built if CONFIG_RMNET_CTL=y/m)
# -------------------------------------------------

obj-$(CONFIG_RMNET_CTL) += rmnet_ctl.o

ifneq (, $(filter y, $(CONFIG_ARCH_LAHAINA) $(CONFIG_ARCH_WAIPIO) $(CONFIG_ARCH_KALAMA)))
rmnet_ctl-y += 		rmnet_ctl_client.o \
			rmnet_ctl_ipa.o \
			rmnet_ctl_mhi.o
endif

# -------------------------------------------------
# RMNET ETH (built if CONFIG_RMNET_ETH=y/m)
# -------------------------------------------------

obj-$(CONFIG_RMNET_ETH) += rmnet_eth.o
rmnet_eth-y += 		rmnet_eth_main.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

KBUILD_OPTIONS := RMNET_CORE_ROOT=$(PWD)
KBUILD_OPTIONS += MODNAME?=rmnet_core

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean
