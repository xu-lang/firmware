################################################################################
#
# xl-utils Makefile
#
################################################################################

XL_UTILS_SITE_METHOD = local
XL_UTILS_SITE = $(XL_UTILS_PKGDIR)/src

XL_UTILS_LICENSE = MIT
XL_UTILS_LICENSE_FILES = LICENSE

define XL_UTILS_BUILD_CMDS
	(cd $(@D); $(TARGET_CC) -Os -s helloworld.c -o helloworld)
endef

define XL_UTILS_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -t $(TARGET_DIR)/usr/bin $(@D)/helloworld
endef

$(eval $(generic-package))

