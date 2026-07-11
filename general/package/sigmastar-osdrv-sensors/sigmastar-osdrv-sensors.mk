################################################################################
#
# sigmastar-osdrv-sensors
#
################################################################################

SIGMASTAR_OSDRV_SENSORS_SITE = $(call github,openipc,sensors,$(SIGMASTAR_OSDRV_SENSORS_VERSION))
SIGMASTAR_OSDRV_SENSORS_VERSION = HEAD

SIGMASTAR_OSDRV_SENSORS_MODULE_SUBDIRS = $(OPENIPC_SOC_VENDOR)/$(OPENIPC_SOC_FAMILY)
SIGMASTAR_OSDRV_SENSORS_MODULE_MAKE_OPTS = \
	SENSOR_VERSION=$(OPENIPC_SOC_FAMILY) \
	INSTALL_MOD_DIR=$(OPENIPC_SOC_VENDOR) \
	KSRC=$(LINUX_DIR)

$(eval $(kernel-module))
$(eval $(generic-package))

define SIGMASTAR_OSDRV_SENSORS_TRIM_KEEP_ONLY_IMX415
	find $(TARGET_DIR)/lib/modules/*/sigmastar -name 'sensor_*.ko' \
		! -name 'sensor_config.ko' \
		! -name 'sensor_$(OPENIPC_SNS_MODEL)_mipi.ko' -delete
endef

SIGMASTAR_OSDRV_SENSORS_POST_INSTALL_TARGET_HOOKS += SIGMASTAR_OSDRV_SENSORS_TRIM_KEEP_ONLY_IMX415
