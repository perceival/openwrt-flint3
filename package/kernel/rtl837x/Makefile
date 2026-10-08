include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=rtl837x-dsa
PKG_VERSION:=0.0.2
PKG_RELEASE:=7
PKG_MAINTAINER:=air jinkela (air_jinkela@163.com)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/$(PKG_NAME)
  SUBMENU:=Other modules
  TITLE:=$(PKG_NAME)
  # The switch uses its native RTL8_4 DSA tag.
  KCONFIG:=CONFIG_NET_DSA_TAG_RTL8_4
  FILES:= \
	$(PKG_BUILD_DIR)/rtl8372n_dsa.ko \
	$(LINUX_DIR)/net/dsa/tag_rtl8_4.ko
  AUTOLOAD:=$(call AutoLoad,17,rtl8372n_dsa)
  DEPENDS:=+kmod-dsa +kmod-regmap-core +kmod-mdio-devres
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) \
		M="$(PKG_BUILD_DIR)" \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
		CONFIG_RTL837X_DSA=m \
		modules
endef

$(eval $(call KernelPackage,rtl837x-dsa))
