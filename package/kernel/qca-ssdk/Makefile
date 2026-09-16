#
# Qualcomm SSDK (Switch/Silicon SDK) -- provides the FAL (Forwarding Abstraction Layer)
# that qca-nss-ppe needs for register-level PPE hardware access on IPQ53xx.
#
# Vendor source, permissively licensed (ISC-style), from Qualcomm's CodeLinaro OSS mirror.
# Adapted from GL.iNet's own qca-ssdk.Makefile (upstream/refs/qsdk-feeds/qca-ssdk.Makefile),
# stripped to just the ipq53xx path this port targets -- see
# project_flint3_ppe_hw_offload_research.md for the full investigation this came out of.
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=qca-ssdk
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://git.codelinaro.org/clo/qsdk/oss/lklm/qca-ssdk.git
PKG_SOURCE_VERSION:=199052ad04a8de97cff8fb4b91b9de86861da79b
PKG_MIRROR_HASH:=skip
PKG_LICENSE:=ISC

hal_arch:=ipq53xx

include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca-ssdk
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=Kernel driver for QCA SSDK (FAL) -- ipq53xx
  DEPENDS:=@TARGET_qualcommbe_ipq53xx
  FILES:=$(PKG_BUILD_DIR)/build/bin/qca-ssdk.ko
  AUTOLOAD:=$(call AutoLoad,30,qca-ssdk)
endef

define KernelPackage/qca-ssdk/description
Kernel module providing Qualcomm's FAL (Forwarding Abstraction Layer) for
register-level access to the IPQ53xx PPE hardware. Required by qca-nss-ppe.
endef

QCASSDK_CONFIG_OPTS:= \
	TOOL_PATH=$(TOOLCHAIN_DIR)/bin \
	SYS_PATH=$(LINUX_DIR) \
	TOOLPREFIX=$(TARGET_CROSS) \
	KVER=$(LINUX_VERSION) \
	ARCH=$(LINUX_KARCH) \
	SoC=$(hal_arch) \
	CHIP_TYPE=MPPE \
	CFLAGS=-I$(STAGING_DIR)/usr/include \
	EXTRA_CFLAGS=-Wno-missing-prototypes

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/qca-ssdk
	$(INSTALL_DIR) $(1)/usr/include/qca-ssdk/api
	$(INSTALL_DIR) $(1)/usr/include/qca-ssdk/ref
	$(INSTALL_DIR) $(1)/usr/include/qca-ssdk/fal
	$(INSTALL_DIR) $(1)/usr/include/qca-ssdk/sal
	$(INSTALL_DIR) $(1)/usr/include/qca-ssdk/init
	$(CP) -rf $(PKG_BUILD_DIR)/include/api/sw_ioctl.h $(1)/usr/include/qca-ssdk/api
	$(CP) -rf $(PKG_BUILD_DIR)/include/init $(1)/usr/include/qca-ssdk
	$(CP) -rf $(PKG_BUILD_DIR)/include/fal $(1)/usr/include/qca-ssdk
	$(CP) -rf $(PKG_BUILD_DIR)/include/common/*.h $(1)/usr/include/qca-ssdk
	$(CP) -rf $(PKG_BUILD_DIR)/include/sal/os/linux/*.h $(1)/usr/include/qca-ssdk
	$(CP) -rf $(PKG_BUILD_DIR)/include/sal/os/*.h $(1)/usr/include/qca-ssdk
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) $(strip $(QCASSDK_CONFIG_OPTS)) \
		LNX_MAKEOPTS='$(KERNEL_MAKEOPTS) KCFLAGS="-Wno-error=missing-prototypes -Wno-error=missing-declarations"' \
		MODULE_TYPE=KSLIB modules
endef

$(eval $(call KernelPackage,qca-ssdk))
