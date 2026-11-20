################################################################################
#
# wifi-hichannel
#
# Ingenic T31 port of the LuckfoxTECH/luckfox-pico "hichannel"/"hcc" driver
# for the HiSilicon Hi3861L over SDIO (camera MSC1 host).
#
# Unlike wifi-hi3881, the Hi3861L on this board self-boots its own firmware
# (SPC021) from internal flash, so NO host firmware download is performed.
# The driver is a pure host<->chip transport (hcc) that brings up wlan0.
#
# Source is vendored in ./src (already carries the T31 port: MSC1 pin-mux +
# jzmmc_manual_detect in probe, falling-edge host-wake on GPIO 43/PB11, and
# the kernel-3.10 compat shims).  Built directly with kbuild (obj-m), so the
# driver Makefile's Luckfox-SDK "all:" recipe is bypassed.
#
################################################################################

WIFI_HICHANNEL_VERSION = 1.0
WIFI_HICHANNEL_SITE = $(WIFI_HICHANNEL_PKGDIR)/src
WIFI_HICHANNEL_SITE_METHOD = local
WIFI_HICHANNEL_LICENSE = GPL-2.0

HICHANNEL_MODULE_NAME = hichannel

# Mirror wifi-hi3881's wireless kernel-config requirements.
define WIFI_HICHANNEL_LINUX_CONFIG_FIXUPS
	$(call KCONFIG_ENABLE_OPT,CONFIG_WLAN)
	$(call KCONFIG_ENABLE_OPT,CONFIG_WIRELESS)
	$(call KCONFIG_ENABLE_OPT,CONFIG_WIRELESS_EXT)
	$(call KCONFIG_ENABLE_OPT,CONFIG_WEXT_CORE)
	$(call KCONFIG_ENABLE_OPT,CONFIG_WEXT_PROC)
	$(call KCONFIG_ENABLE_OPT,CONFIG_WEXT_PRIV)
	$(call KCONFIG_SET_OPT,CONFIG_CFG80211,y)
	$(call KCONFIG_DISABLE_OPT,CONFIG_MMC1_PIO_MODE)
endef

# tools/hinl.c is the userspace side: it talks to the chip's application
# firmware over netlink 28 (watchdog off, Wi-Fi connect, copy the chip's
# DHCP lease to wlan0).  package/wifi's S38wpa_supplicant calls "hinl up"
# with the credentials from /etc/wpa_supplicant.conf.
#
# Build the module via kbuild against the kernel tree.  The driver Makefile
# defines obj-m / EXTRA_CFLAGS unconditionally at parse time (env_config.mk
# sets -D_PRE_OS_PLATFORM=_PRE_PLATFORM_JZ), so "M=$(@D) modules" is enough.
define WIFI_HICHANNEL_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(LINUX_DIR) \
		ARCH=$(KERNEL_ARCH) \
		CROSS_COMPILE=$(TARGET_CROSS) \
		M=$(@D) modules
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall \
		-o $(@D)/tools/hinl $(@D)/tools/hinl.c
endef

LINUX_CONFIG_LOCALVERSION = $(shell awk -F "=" '/^CONFIG_LOCALVERSION=/ {print $$2}' $(BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE))

define WIFI_HICHANNEL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/$(HICHANNEL_MODULE_NAME).ko \
		$(TARGET_DIR)/usr/lib/modules/$(KERNEL_VERSION)$(call qstrip,$(LINUX_CONFIG_LOCALVERSION))/extra/$(HICHANNEL_MODULE_NAME).ko
	$(INSTALL) -D -m 0755 $(@D)/tools/hinl $(TARGET_DIR)/usr/bin/hinl
	$(INSTALL) -D -m 0755 $(WIFI_HICHANNEL_PKGDIR)/files/hi-cycle \
		$(TARGET_DIR)/usr/sbin/hi-cycle
	$(INSTALL) -D -m 0755 $(WIFI_HICHANNEL_PKGDIR)/files/S99hi-cycle \
		$(TARGET_DIR)/etc/init.d/S99hi-cycle
endef

$(eval $(kernel-module))
$(eval $(generic-package))
