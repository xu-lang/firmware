################################################################################
#
# sigmastar-venc-poc
#
################################################################################

SIGMASTAR_VENC_POC_SITE_METHOD = local
SIGMASTAR_VENC_POC_SITE = $(SIGMASTAR_VENC_POC_PKGDIR)/src

define SIGMASTAR_VENC_POC_BUILD_CMDS
	$(MAKE) CC=$(TARGET_CC) -C $(@D)
endef

define SIGMASTAR_VENC_POC_INSTALL_TARGET_CMDS
	$(INSTALL) -m 755 -t $(TARGET_DIR)/usr/bin $(@D)/output/*
endef

$(eval $(generic-package))
