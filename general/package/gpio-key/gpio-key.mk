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
endef

$(eval $(generic-package))
