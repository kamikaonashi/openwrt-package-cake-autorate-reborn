include $(TOPDIR)/rules.mk

PKG_NAME    := cake-autorate-reborn
PKG_VERSION := 1.0.2
PKG_RELEASE := 1

PKG_LICENSE         := GPL-2.0-or-later
PKG_MAINTAINER      := kamikaonashi
PKG_BUILD_PARALLEL  := 1

include $(INCLUDE_DIR)/package.mk

# $(CURDIR) is always the package directory regardless of SDK nesting depth.
PKG_ROOT := $(CURDIR)/..

define Package/cake-autorate-reborn
  SECTION  := net
  CATEGORY := Network
  TITLE    := CAKE Autorate Reborn
  DEPENDS  := +libubox +libuci +kmod-ifb +kmod-sched-cake \
              +kmod-sched-core +luci-base \
              +CAKE_MQ:kmod-sched-cake-mq
endef

define Package/cake-autorate-reborn/description
  Adaptive CAKE qdisc bandwidth controller for OpenWrt, with integrated
  LuCI web interface.  Standalone - no sqm-scripts required.

  Supports ICMP Timestamp (type 13) for true per-direction OWD measurement.
  Manages the full IFB + CAKE plumbing lifecycle via netlink.
endef

define Package/cake-autorate-reborn/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/cake-autorate-reborn enable
/etc/init.d/cake-autorate-reborn start
exit 0
endef

define Package/cake-autorate-reborn/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/cake-autorate-reborn stop
/etc/init.d/cake-autorate-reborn disable
exit 0
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(PKG_ROOT)/src/*.c \
	      $(PKG_ROOT)/src/*.h \
	      $(PKG_ROOT)/src/Makefile.local \
	      $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		-f Makefile.local \
		--no-print-directory \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -O2 -pipe -I$(STAGING_DIR)/usr/include" \
		LDFLAGS="$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib -lubox -luci"
	$(TARGET_CROSS)strip --strip-unneeded \
		$(PKG_BUILD_DIR)/cake-autorate-reborn
endef

define Package/cake-autorate-reborn/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/cake-autorate-reborn $(1)/usr/sbin/

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_ROOT)/root/etc/init.d/cake-autorate-reborn \
		$(1)/etc/init.d/

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(PKG_ROOT)/root/etc/config/cake_autorate_reborn \
		$(1)/etc/config/

	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) \
		$(PKG_ROOT)/root/usr/share/luci/menu.d/luci-app-cake-autorate-reborn.json \
		$(1)/usr/share/luci/menu.d/

	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) \
		$(PKG_ROOT)/root/usr/share/rpcd/acl.d/luci-app-cake-autorate-reborn.json \
		$(1)/usr/share/rpcd/acl.d/

	$(INSTALL_DIR) $(1)/www/luci-static/resources/view
	$(INSTALL_DATA) \
		$(PKG_ROOT)/luci-app-cake-autorate-reborn/www/luci-static/resources/view/cake-autorate-reborn.js \
		$(1)/www/luci-static/resources/view/
endef

$(eval $(call BuildPackage,cake-autorate-reborn))
