# tests.mk - firmware test registry, selection and validation.
#
# Included by the main Makefile when TIKU_TESTS_ENABLE=1 (or when the goal is
# test-list / test-config).  This file only computes variables; it never adds
# to SRCS or CFLAGS itself.  The Makefile appends, after the platform blocks:
#
#     SRCS   += $(sort $(TEST_SRCS))
#     CFLAGS += $(TEST_CFLAGS)
#
# Outputs
#   TESTS_SELECTED       names of the enabled tests
#   TEST_SRCS            their sources (may repeat; the Makefile applies $(sort))
#   TEST_CFLAGS          their -D flags
#   TIKU_NPU_ENABLE      forced to 1 when a selected test needs the NPU
#   TIKU_TESTS_DK_IMAGE  1 when a DK image test is selected (apps/apps.mk and
#                        the edge-detect check use it)
#
# Selecting tests (lowest to highest priority)
#   1. tests.config      TEST_<name> = 1           (copy tests.config.example)
#   2. TIKU_TESTS="a b"  turns those tests on      (make TIKU_TESTS="npu_mem")
#   3. command line      make TEST_<name>=1
#
# Adding a test: add its name to TESTS_ALL and give it a T.<name>.* block.
#   .srcs      source files
#   .macro     defined as -D<macro>=1
#   .board     BOARD it requires
#   .group     at most one enabled test per group
#   .requires  tests that must also be enabled
#   .excludes  tests that cannot be enabled with this one
#   .needs     build option it forces on (NPU)
#   .vars      parameters passed as -D<var>=$(<var>) when the variable is set
#   .note      text shown by `make test-list`

TESTS_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

# A target defined here must not become the default goal.  Restored at the end.
_TESTS_SAVED_GOAL := $(.DEFAULT_GOAL)

-include $(TESTS_DIR)/tests.config

# ---------------------------------------------------------------------------
# Shared sources
# ---------------------------------------------------------------------------
IMX335_DRV      := drivers/stm32n6/imx335/tiku_stm32n6570_imx335.c
CORRUPT_FIXTURE := $(TESTS_DIR)/npu/fixtures/stm32n6_identity_corrupt.network_rel.c

# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------
TESTS_ALL := \
    region \
    dk_tft_single dk_tft_colorcycle dk_dcmipp_csi192 dk_imx335_stream \
    dk_dcmipp_capture dk_dcmipp_pipe2_640 dk_dcmipp_dual \
    npu_mem npu_mem_negative npu_embedded npu_run npu_vfs npu_preflight \
    npu_pool npu_resource npu_lifecycle

T.region.srcs  := $(TESTS_DIR)/tiku_stm32n6_region_acceptance.c
T.region.macro := TIKU_STM32N6_REGION_TEST_ENABLE

# DK images: each autostarts its own process, so at most one may link.
T.dk_tft_single.srcs  := $(TESTS_DIR)/tiku_stm32n6_dk_tft_single.c
T.dk_tft_single.macro := TIKU_STM32N6_DK_TFT_TEST_ENABLE

T.dk_tft_colorcycle.srcs  := $(TESTS_DIR)/tiku_stm32n6_dk_tft_colorcycle.c
T.dk_tft_colorcycle.macro := TIKU_STM32N6_DK_TFT_COLORCYCLE_TEST_ENABLE
T.dk_tft_colorcycle.vars  := TFT_COLOR_ROTATION_SECONDS

T.dk_dcmipp_csi192.srcs  := $(TESTS_DIR)/tiku_stm32n6_dk_dcmipp_csi192.c
T.dk_dcmipp_csi192.macro := TIKU_STM32N6_DK_DCMIPP_CSI192_TEST_ENABLE

T.dk_imx335_stream.srcs  := $(TESTS_DIR)/tiku_stm32n6_dk_imx335_stream.c $(IMX335_DRV)
T.dk_imx335_stream.macro := TIKU_STM32N6_DK_IMX335_TEST_ENABLE

T.dk_dcmipp_capture.srcs  := $(TESTS_DIR)/tiku_stm32n6_dk_dcmipp_capture.c $(IMX335_DRV)
T.dk_dcmipp_capture.macro := TIKU_STM32N6_DK_DCMIPP_CAPTURE_TEST_ENABLE

T.dk_dcmipp_pipe2_640.srcs  := $(TESTS_DIR)/tiku_stm32n6_dk_dcmipp_pipe2_640.c $(IMX335_DRV)
T.dk_dcmipp_pipe2_640.macro := TIKU_STM32N6_DK_DCMIPP_PIPE2_640_TEST_ENABLE
T.dk_dcmipp_pipe2_640.vars  := PIPE2_TEST_VARIANT
T.dk_dcmipp_pipe2_640.note  := PIPE2_TEST_VARIANT=0|1|2 picks the Pipe2 fallback experiment

T.dk_dcmipp_dual.srcs  := $(TESTS_DIR)/tiku_stm32n6_dk_dcmipp_dual_pipe.c $(IMX335_DRV)
T.dk_dcmipp_dual.macro := TIKU_STM32N6_DK_DCMIPP_DUAL_TEST_ENABLE
T.dk_dcmipp_dual.vars  := DUAL_P1_FORMAT DUAL_CYCLES DUAL_MAD_LIMIT
T.dk_dcmipp_dual.note  := DUAL_P1_FORMAT=565|888 selects the Pipe1 output format

$(foreach t,dk_tft_single dk_tft_colorcycle dk_dcmipp_csi192 dk_imx335_stream \
    dk_dcmipp_capture dk_dcmipp_pipe2_640 dk_dcmipp_dual,\
    $(eval T.$(t).board := stm32n6570_dk)$(eval T.$(t).group := dk_image))

# NPU tests: each needs the NPU build.  Most autostart their own process; the
# coupling between embedded, resource and vfs is described in the notes.
T.npu_mem.srcs  := $(TESTS_DIR)/tiku_npu_mem_acceptance.c
T.npu_mem.macro := TIKU_NPU_MEM_TEST_ENABLE

T.npu_mem_negative.macro    := TIKU_NPU_MEM_NEGATIVE_TEST_ENABLE
T.npu_mem_negative.requires := npu_mem

T.npu_embedded.srcs  := $(TESTS_DIR)/tiku_npu_embedded_acceptance.c
T.npu_embedded.macro := TIKU_NPU_EMBEDDED_TEST_ENABLE
T.npu_embedded.note  := with npu_resource on, runs the resource checks itself; not autostarted when npu_vfs is on

T.npu_run.macro    := TIKU_NPU_RUN_TEST_ENABLE
T.npu_run.requires := npu_embedded
T.npu_run.note     := adds the run-trace checks to npu_embedded

T.npu_vfs.srcs  := $(TESTS_DIR)/tiku_npu_vfs_acceptance.c
T.npu_vfs.macro := TIKU_NPU_VFS_TEST_ENABLE

T.npu_preflight.srcs  := $(TESTS_DIR)/tiku_npu_preflight_acceptance.c $(CORRUPT_FIXTURE)
T.npu_preflight.macro := TIKU_NPU_PREFLIGHT_TEST_ENABLE

T.npu_pool.srcs  := $(TESTS_DIR)/tiku_npu_pool_acceptance.c $(CORRUPT_FIXTURE)
T.npu_pool.macro := TIKU_NPU_POOL_TEST_ENABLE

T.npu_resource.srcs  := $(TESTS_DIR)/tiku_npu_resource_acceptance.c
T.npu_resource.macro := TIKU_NPU_RESOURCE_TEST_ENABLE
T.npu_resource.note  := not autostarted when npu_embedded is on (embedded runs its checks)

T.npu_lifecycle.srcs  := $(TESTS_DIR)/tiku_npu_lifecycle_acceptance.c
T.npu_lifecycle.macro := TIKU_NPU_LIFECYCLE_TEST_ENABLE

$(foreach t,npu_mem npu_mem_negative npu_embedded npu_run npu_vfs npu_preflight \
    npu_pool npu_resource npu_lifecycle,$(eval T.$(t).needs := NPU))

# ---------------------------------------------------------------------------
# Selection
# ---------------------------------------------------------------------------
# TIKU_TESTS="a b" turns those tests on.  An unknown name is an error.
$(foreach t,$(TIKU_TESTS),\
    $(if $(filter $(t),$(TESTS_ALL)),$(eval TEST_$(t) := 1),\
    $(error TIKU_TESTS: unknown test '$(t)'; run `make test-list`)))

# A TEST_<lowercase> variable that names no test is a typo in the config.  A
# variable with an uppercase letter (TEST_ENABLE, TEST_KITS_*) belongs to
# someone else and is skipped.
_tests_strip = $(if $(2),$(call _tests_strip,$(subst $(firstword $(2)),,$(1)),$(wordlist 2,64,$(2))),$(1))
_TESTS_LOWER := a b c d e f g h i j k l m n o p q r s t u v w x y z 0 1 2 3 4 5 6 7 8 9 _
$(foreach v,$(filter TEST_%,$(.VARIABLES)),\
    $(if $(call _tests_strip,$(patsubst TEST_%,%,$(v)),$(_TESTS_LOWER)),,\
    $(if $(filter $(patsubst TEST_%,%,$(v)),$(TESTS_ALL)),,\
    $(error $(v): no such test; run `make test-list`))))

TESTS_SELECTED := $(strip $(foreach t,$(TESTS_ALL),$(if $(filter 1,$(TEST_$(t))),$(t))))

# ---------------------------------------------------------------------------
# Validation (each failure names the tests involved)
# ---------------------------------------------------------------------------
_tests_in_group = $(foreach t,$(TESTS_SELECTED),$(if $(filter $(1),$(T.$(t).group)),$(t)))
TESTS_GROUPS    := $(sort $(foreach t,$(TESTS_SELECTED),$(T.$(t).group)))

_TESTS_CHECK := \
    $(foreach t,$(TESTS_SELECTED),\
        $(if $(T.$(t).board),$(if $(filter $(T.$(t).board),$(BOARD)),,\
            $(error test '$(t)' requires BOARD=$(T.$(t).board) (BOARD=$(BOARD))))) \
        $(foreach r,$(T.$(t).requires),$(if $(filter $(r),$(TESTS_SELECTED)),,\
            $(error test '$(t)' requires test '$(r)' to be enabled))) \
        $(foreach x,$(T.$(t).excludes),$(if $(filter $(x),$(TESTS_SELECTED)),\
            $(error tests '$(t)' and '$(x)' cannot be enabled together)))) \
    $(foreach g,$(TESTS_GROUPS),\
        $(if $(word 2,$(call _tests_in_group,$(g))),\
            $(error only one '$(g)' test may be enabled, got: $(call _tests_in_group,$(g)))))

# Build options the selected tests need.
_TESTS_NEEDS := $(sort $(foreach t,$(TESTS_SELECTED),$(T.$(t).needs)))
ifneq ($(filter NPU,$(_TESTS_NEEDS)),)
TIKU_NPU_ENABLE := 1
endif
# A command-line TIKU_NPU_ENABLE=0 beats the assignment above; refuse it.
ifneq ($(filter NPU,$(_TESTS_NEEDS)),)
ifneq ($(TIKU_NPU_ENABLE),1)
$(error NPU tests are enabled but TIKU_NPU_ENABLE=$(TIKU_NPU_ENABLE))
endif
endif

ifneq ($(filter dk_image,$(TESTS_GROUPS)),)
TIKU_TESTS_DK_IMAGE := 1
endif

# Standalone NPU tests each autostart a process and share one NPU and one model
# store.  Running several at once is allowed, but warn.
_TESTS_NPU_AUTO := \
    $(filter npu_mem npu_lifecycle npu_pool npu_preflight npu_vfs,$(TESTS_SELECTED)) \
    $(if $(filter npu_vfs,$(TESTS_SELECTED)),,$(filter npu_embedded,$(TESTS_SELECTED))) \
    $(if $(filter npu_embedded,$(TESTS_SELECTED)),,$(filter npu_resource,$(TESTS_SELECTED)))
ifneq ($(word 2,$(_TESTS_NPU_AUTO)),)
$(warning NPU tests autostart together ($(strip $(_TESTS_NPU_AUTO))); they share the NPU and the model store)
endif

# ---------------------------------------------------------------------------
# Outputs
# ---------------------------------------------------------------------------
TEST_SRCS := $(foreach t,$(TESTS_SELECTED),$(T.$(t).srcs))
TEST_CFLAGS := $(foreach t,$(TESTS_SELECTED),\
    $(if $(T.$(t).macro),-D$(T.$(t).macro)=1) \
    $(foreach v,$(T.$(t).vars),$(if $($(v)),-D$(v)=$($(v)))))

# ---------------------------------------------------------------------------
# Helper targets
# ---------------------------------------------------------------------------
.PHONY: test-list test-config

test-list:
	$(foreach t,$(TESTS_ALL),$(info [$(if $(filter $(t),$(TESTS_SELECTED)),x, )] $(t)$(if $(T.$(t).group), group=$(T.$(t).group))$(if $(T.$(t).board), board=$(T.$(t).board))$(if $(T.$(t).requires), requires=$(T.$(t).requires))$(if $(T.$(t).note), -- $(T.$(t).note))))@:

test-config:
	$(info tests    : $(if $(TESTS_SELECTED),$(TESTS_SELECTED),(none)))
	$(info sources  : $(sort $(TEST_SRCS)))
	$(info cflags   : $(TEST_CFLAGS))
	$(info NPU      : $(if $(filter NPU,$(_TESTS_NEEDS)),forced on,not required))
	$(info DK image : $(if $(TIKU_TESTS_DK_IMAGE),yes,no))
	@:

.DEFAULT_GOAL := $(_TESTS_SAVED_GOAL)
