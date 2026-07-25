################################################################################
#
# majestic
#
################################################################################

MAJESTIC_SITE = https://openipc.s3-eu-west-1.amazonaws.com
MAJESTIC_SOURCE = majestic.$(MAJESTIC_FAMILY).$(MAJESTIC_VARIANT).master.tar.bz2
MAJESTIC_LICENSE = PROPRIETARY
MAJESTIC_LICENSE_FILES = LICENSE

MAJESTIC_FAMILY = $(OPENIPC_SOC_FAMILY)
MAJESTIC_VARIANT = $(OPENIPC_MAJESTIC)

MAJESTIC_DEPENDENCIES += \
	libevent-openipc \
	libogg-openipc \
	mbedtls-openipc \
	opus-openipc \
	json-c

define MAJESTIC_PATCH_EXPOSURE_MIN
	perl -0777 -pi -e 's/\x05\x28\x4f\xf4\x7a\x73\xb8\xbf\x05\x20\xc8\x28/\x01\x28\x4f\xf4\x7a\x73\xb8\xbf\x01\x20\xc8\x28/ or die "majestic exposure clamp pattern not found\n"' $(@D)/majestic
endef
MAJESTIC_POST_EXTRACT_HOOKS += MAJESTIC_PATCH_EXPOSURE_MIN

define MAJESTIC_INSTALL_TARGET_CMDS
	$(INSTALL) -m 755 -d $(TARGET_DIR)/etc
	$(INSTALL) -m 644 -t $(TARGET_DIR)/etc $(MAJESTIC_PKGDIR)/files/majestic.yaml

	$(INSTALL) -m 755 -d $(TARGET_DIR)/etc/init.d
	$(INSTALL) -m 755 -t $(TARGET_DIR)/etc/init.d $(MAJESTIC_PKGDIR)/files/S95majestic

	$(INSTALL) -m 755 -d $(TARGET_DIR)/usr/bin
	$(INSTALL) -m 755 -t $(TARGET_DIR)/usr/bin $(@D)/majestic
endef

$(eval $(generic-package))
