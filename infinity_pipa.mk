#
# Copyright (C) 2021 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)

# Inherit some common lineage stuff.
$(call inherit-product, vendor/infinity/config/common_full_tablet_wifionly.mk)

# Inherit from pipa device
$(call inherit-product, device/xiaomi/pipa/device.mk)

# Inherit keys
$(call inherit-product, vendor/lineage-priv/keys/keys.mk)

# Infinity flags
INFINITY_BUILD_TYPE := OFFICIAL
INFINITY_MAINTAINER := MufasaXz
TARGET_FACE_UNLOCK_SUPPORTED := true 
TARGET_SUPPORTS_BLUR := true
WITH_GAPPS := true

PRODUCT_NAME := infinity_pipa
PRODUCT_DEVICE := pipa
PRODUCT_MANUFACTURER := Xiaomi
PRODUCT_BRAND := Xiaomi
PRODUCT_MODEL := Pad 6

PRODUCT_CHARACTERISTICS := tablet
TARGET_SUPPORTS_QUICK_TAP := false

PRODUCT_GMS_CLIENTID_BASE := android-xiaomi

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildFingerprint=Xiaomi/pipa_pad_cn/pipa:14/UKQ1.240624.001/OS2.0.14.0.UMZCNXM:user/release-keys  \
