# SPDX-License-Identifier: GPL-2.0-only
# Resolve build-selected compiler commands without building tools or packages.
TOPDIR:=$(CURDIR)
include $(TOPDIR)/rules.mk
ifneq ($(CONFIG_USE_LLVM_HOST)$(CONFIG_USE_LLVM_PREBUILT)$(CONFIG_USE_LLVM_BUILD),)
include $(INCLUDE_DIR)/bpf.mk
endif
.PHONY: ax6000-ci-toolchain
ax6000-ci-toolchain:
	@printf '%s\n' 'host_cc=$(HOSTCC_NOCACHE)' "target_cc=$$(command -v '$(TARGET_CC_NOCACHE)' || true)" 'bpf_clang=$(CLANG)' 'bpf_llvm_strip=$(LLVM_STRIP)'
