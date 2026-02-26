include $(TOPDIR)/rules.mk

PKG_NAME    := cake-autorate-reborn
PKG_VERSION := 1.0.0
PKG_RELEASE := 1

PKG_LICENSE         := GPL-2.0-or-later
PKG_MAINTAINER      := kamikaonashi
PKG_BUILD_PARALLEL  := 1

include $(INCLUDE_DIR)/package.mk

# ── C daemon ─────────────────────────────────────────────────────

define Package/cake-autorate-reborn
  SECTION  := net
  CATEGORY := Network
  TITLE    := CAKE Autorate Reborn daemon
  DEPENDS  := +libubox +libuci +fping +tc-full
endef

define Package/cake-autorate-reborn/description
  C rewrite of cake-autorate: adaptively adjusts CAKE qdisc bandwidth
  based on measured one-way delay (OWD) via fping. Supports multiple
  instances, automatic reflector replacement, and procd supervision.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		--no-print-directory \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -I$(STAGING_DIR)/usr/include" \
		LDFLAGS="$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib -lubox -luci -lm"
endef

define Package/cake-autorate-reborn/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/cake-autorate-reborn \
	               $(1)/usr/sbin/

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./root/etc/init.d/cake-autorate-reborn \
	               $(1)/etc/init.d/

	$(INSTALL_DIR)  $(1)/etc/config
	$(INSTALL_CONF) ./root/etc/config/cake_autorate_reborn \
	                $(1)/etc/config/
endef

$(eval $(call BuildPackage,cake-autorate-reborn))

# ── LuCI companion ───────────────────────────────────────────────

define Package/luci-app-cake-autorate-reborn
  SECTION   := luci
  CATEGORY  := LuCI
  SUBMENU   := 3. Applications
  TITLE     := LuCI support for CAKE Autorate Reborn
  DEPENDS   := +luci-base +cake-autorate-reborn
  PKGARCH   := all
endef

define Package/luci-app-cake-autorate-reborn/description
  LuCI web interface for cake-autorate-reborn.
  Provides per-instance configuration with General, Advanced,
  and Reflector Health tabs, plus Start/Stop/Restart controls.
endef

define Package/luci-app-cake-autorate-reborn/install
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) \
		./root/usr/share/luci/menu.d/luci-app-cake-autorate-reborn.json \
		$(1)/usr/share/luci/menu.d/

	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) \
		./root/usr/share/rpcd/acl.d/luci-app-cake-autorate-reborn.json \
		$(1)/usr/share/rpcd/acl.d/

	$(INSTALL_DIR) $(1)/www/luci-static/resources/view
	$(INSTALL_DATA) \
		./luci-app-cake-autorate-reborn/www/luci-static/resources/view/cake-autorate-reborn.js \
		$(1)/www/luci-static/resources/view/
endef

$(eval $(call BuildPackage,luci-app-cake-autorate-reborn))
