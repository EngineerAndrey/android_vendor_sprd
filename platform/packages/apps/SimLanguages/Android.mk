LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := $(call all-subdir-java-files)

LOCAL_JAVA_LIBRARIES := telephony-common
LOCAL_JAVA_LIBRARIES += framework

LOCAL_PACKAGE_NAME := SimLanguages
LOCAL_CERTIFICATE := platform

include $(BUILD_PACKAGE)

include $(call all-makefiles-under, $(LOCAL_PATH))

