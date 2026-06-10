DTS_DIR := $(DTS_DIR)/qcom

define Device/glinet_gl-be9300
	$(call Device/FitImage)
	$(call Device/EmmcImage)
	DEVICE_VENDOR := GL.iNet
	DEVICE_MODEL := GL-BE9300
	DEVICE_ALT0_VENDOR := GL.iNet
	DEVICE_ALT0_MODEL := Flint 3
	SOC := ipq5332
	DEVICE_DTS_CONFIG := config-1
	DEVICE_PACKAGES := \
		ipq-wifi-glinet_gl-be9300 \
		ath12k-firmware-qcn9274 \
		ipq5332-wifi-firmware \
		e2fsprogs kmod-fs-ext4 losetup \
		kmod-fs-f2fs mkf2fs f2fsck \
		-wpad-basic-mbedtls wpad-mbedtls dawn umdns
endef
TARGET_DEVICES += glinet_gl-be9300
