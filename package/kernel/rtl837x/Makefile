include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=rtl837x-dsa
PKG_VERSION:=0.0.2
PKG_RELEASE:=3
PKG_MAINTAINER:=air jinkela (air_jinkela@163.com)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/$(PKG_NAME)
  SUBMENU:=Other modules
  TITLE:=$(PKG_NAME)
  # Both taggers are built: the driver implements .change_tag_protocol so the
  # trade-off between them (PPE-parsable 802.1Q tag vs precise per-port identity
  # from the chip's own 0x8899 tag) can be measured at runtime rather than
  # assumed at compile time.
  KCONFIG:= \
	CONFIG_NET_DSA_TAG_VSC73XX_8021Q \
	CONFIG_NET_DSA_TAG_RTL8_4
  FILES:= \
	$(PKG_BUILD_DIR)/rtl837x_dsa.ko \
	$(LINUX_DIR)/net/dsa/tag_vsc73xx_8021q.ko \
	$(LINUX_DIR)/net/dsa/tag_rtl8_4.ko
  AUTOLOAD:=$(call AutoLoad,99,rtl837x_dsa,1)
  DEPENDS:=+kmod-dsa +kmod-regmap-core
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) \
		M="$(PKG_BUILD_DIR)" \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
		CONFIG_RTL837X_DSA=m \
		modules
endef

$(eval $(call KernelPackage,rtl837x-dsa))
