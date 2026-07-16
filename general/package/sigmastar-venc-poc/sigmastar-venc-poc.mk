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
	$(INSTALL) -m 755 -D $(@D)/output/sigmastar_venc_poc \
		$(TARGET_DIR)/usr/bin/sigmastar_venc_poc
endef

$(eval $(generic-package))
