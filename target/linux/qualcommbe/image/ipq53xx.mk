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
		e2fsprogs kmod-fs-ext4 losetup
endef
TARGET_DEVICES += glinet_gl-be9300
