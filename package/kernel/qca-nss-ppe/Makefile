#
# Qualcomm PPE (Packet Processing Engine) core driver -- ppe_drv. This is the vendor
# flow-programming driver that would back a real mainline hardware NAT/flow-offload shim
# (not yet written -- see project_flint3_ppe_hw_offload_research.md). Packaging this alone
# does NOT give you hardware NAT acceleration; it's the prerequisite dependency for that
# future work, and is otherwise inert (nothing in this tree calls into it yet).
#
# Vendor source, permissively licensed (ISC-style), from Qualcomm's CodeLinaro OSS mirror.
# Adapted from GL.iNet's own qca-nss-ppe.Makefile
# (upstream/refs/qsdk-feeds/qca-nss-ppe.Makefile) -- that Makefile's own DEPENDS line
# already lists TARGET_ipq53xx, confirming this exact package is what GL.iNet's stock
# firmware ships for hardware acceleration on this same silicon.
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=qca-nss-ppe
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://git.codelinaro.org/clo/qsdk/oss/lklm/nss-ppe.git
PKG_SOURCE_VERSION:=2af93823d75ab89c7caaf7569d784dcc7fa16b69
PKG_MIRROR_HASH:=skip
PKG_LICENSE:=ISC

include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca-nss-ppe
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=Kernel driver for PPE (core driver) -- ipq53xx
  DEPENDS:=@TARGET_qualcommbe_ipq53xx +kmod-qca-ssdk
  FILES:=$(PKG_BUILD_DIR)/drv/ppe_drv/qca-nss-ppe.ko
  AUTOLOAD:=$(call AutoLoad,31,qca-nss-ppe)
endef

define KernelPackage/qca-nss-ppe/description
Kernel module for Qualcomm's PPE flow-table driver (ppe_drv). Exports
ppe_drv_v4_create()/ppe_drv_v4_destroy() and IPv6 equivalents for
programming hardware-accelerated NAT/forwarding flows -- currently unused
by any mainline hook in this tree; see project_flint3_ppe_hw_offload_research.md.
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/qca-nss-ppe
	$(CP) $(PKG_BUILD_DIR)/exports/* $(1)/usr/include/qca-nss-ppe/
	$(CP) $(PKG_BUILD_DIR)/drv/exports/* $(1)/usr/include/qca-nss-ppe/
endef

EXTRA_CFLAGS:= \
	-I$(STAGING_DIR)/usr/include/qca-ssdk \
	-I$(STAGING_DIR)/usr/include/qca-ssdk/fal \
	-I$(STAGING_DIR)/usr/include/qca-ssdk/init

define Build/Compile
	$(KERNEL_MAKE) \
		M="$(PKG_BUILD_DIR)" \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
		SoC="ipq53xx" \
		modules
endef

$(eval $(call KernelPackage,qca-nss-ppe))
