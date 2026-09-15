#
# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=glinet-app-compat
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=MNeroba

include $(INCLUDE_DIR)/package.mk

define Package/glinet-app-compat
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Local GL.iNet App compatibility core
  DEPENDS:=+uhttpd +uhttpd-mod-ucode +ucode +ucode-mod-digest \
	+ucode-mod-fs +ucode-mod-log +ucode-mod-ubus +ucode-mod-uci \
	+rpcd-mod-iwinfo
endef

define Package/glinet-app-compat/description
 The package provides a local, LAN-only JSON-RPC compatibility endpoint for
 the GL.iNet mobile application. It exposes authenticated, read-only status
 data through standard OpenWrt ubus and UCI interfaces.
endef

define Package/glinet-app-compat/conffiles
/etc/config/glinet-app-compat
endef

define Package/glinet-app-compat/install
	$(INSTALL_DIR) $(1)/etc/config $(1)/etc/uci-defaults \
		$(1)/usr/share/glinet-app-compat
	$(INSTALL_CONF) ./files/etc/config/glinet-app-compat \
		$(1)/etc/config/glinet-app-compat
	$(INSTALL_BIN) ./files/etc/uci-defaults/90-glinet-app-compat \
		$(1)/etc/uci-defaults/90-glinet-app-compat
	$(INSTALL_DATA) ./files/usr/share/glinet-app-compat/rpc.uc \
		$(1)/usr/share/glinet-app-compat/rpc.uc
endef

define Package/glinet-app-compat/postinst
#!/bin/sh

[ -n "$${IPKG_INSTROOT}" ] || {
	UHTTPD_PREFIX='/rpc=/usr/share/glinet-app-compat/rpc.uc'
	case "$$(uci -q get uhttpd.main.ucode_prefix)" in
		*"$${UHTTPD_PREFIX}"*)
			;;
		*)
			uci -q add_list uhttpd.main.ucode_prefix="$${UHTTPD_PREFIX}" || exit 1
			uci -q commit uhttpd || exit 1
			;;
	esac
	/etc/init.d/uhttpd reload
}

exit 0
endef

define Package/glinet-app-compat/prerm
#!/bin/sh

[ -n "$${IPKG_INSTROOT}" ] || {
	UHTTPD_PREFIX='/rpc=/usr/share/glinet-app-compat/rpc.uc'
	case "$$(uci -q get uhttpd.main.ucode_prefix)" in
		*"$${UHTTPD_PREFIX}"*)
			uci -q del_list uhttpd.main.ucode_prefix="$${UHTTPD_PREFIX}" || exit 1
			uci -q commit uhttpd || exit 1
			/etc/init.d/uhttpd reload
			;;
	esac
}

exit 0
endef

$(eval $(call BuildPackage,glinet-app-compat))
