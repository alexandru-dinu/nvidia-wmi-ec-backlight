KERNEL_UNAME := $(shell uname -r)
KERNEL_DIR := /lib/modules/$(KERNEL_UNAME)

MODULE := nvidia-wmi-ec-backlight
# PATCH_FILE=v2-nvidia-wmi-ec-backlight-Add-workarounds-for-confused-firmware.diff

all: modules

modules:
	$(MAKE) -C ${KERNEL_DIR}/build M=$(PWD) modules

clean:
	$(MAKE) -C ${KERNEL_DIR}/build M=$(PWD) clean

install: modules
	xz --check=crc32 --lzma2=dict=512KiB ${MODULE}.ko
	sudo rm -fv ${KERNEL_DIR}/kernel/drivers/platform/x86/${MODULE}.*
	sudo cp ${MODULE}.ko.xz ${KERNEL_DIR}/kernel/drivers/platform/x86/
	sudo depmod -a

# orig:
# cp ../5.16.14/linux-5.16.14/drivers/platform/x86/${MODULE}.c ./

# patch: orig
# patch -u ${MODULE}.c -i ${PATCH_FILE}
