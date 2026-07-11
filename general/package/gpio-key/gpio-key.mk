################################################################################
#
# gpio-key
#
################################################################################

GPIO_KEY_SITE_METHOD = local
GPIO_KEY_SITE = $(GPIO_KEY_PKGDIR)/src

define GPIO_KEY_BUILD_CMDS
	$(MAKE) CC=$(TARGET_CC) -C $(@D)
endef

define GPIO_KEY_INSTALL_TARGET_CMDS
	$(INSTALL) -m 755 -t $(TARGET_DIR)/usr/bin $(@D)/output/*
	$(INSTALL) -D -m 644 $(GPIO_KEY_PKGDIR)/files/gpio-key.conf \
		$(TARGET_DIR)/etc/gpio-key.conf
	$(RM) $(TARGET_DIR)/etc/init.d/S90gpio-key
	$(INSTALL) -D -m 755 $(GPIO_KEY_PKGDIR)/files/S99gpio-key \
		$(TARGET_DIR)/etc/init.d/S99gpio-key
endef

$(eval $(generic-package))
