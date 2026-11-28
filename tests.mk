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
#
# Tests and apps never share an image.  With TIKU_TESTS_ENABLE=1 the main
# Makefile does not include apps/apps.mk, and this file stops the build for
# anything else that owns the image's autostart processes: HAS_TESTS=1
# (TikuBench), APP=, DEMO= and the edge detection app.  Selecting no test is
# an error too.
#
# Selecting tests (lowest to highest priority)
#   1. tests.config      TEST_<name> = 1           (copy tests.config.example)
#   2. TIKU_TESTS="a b"  turns those tests on      (make TIKU_TESTS="npu_mem")
#   3. command line      make TEST_<name>=1
#
# One image, one test: every firmware test autostarts its own process, so the
# enabled tests must form a single connected group, where two tests are
# connected when one lists the other in .requires or .with.  Enabling two
# unrelated tests is an error.  There is no override.
#
# Adding a test: add its name to TESTS_ALL and give it a T.<name>.* block.
#   .srcs      source files
#   .macro     defined as -D<macro>=1
#   .board     BOARD it requires
#   .group     at most one enabled test per group
#   .requires  tests that must also be enabled
#   .with      tests that may run together with this one without requiring it
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
T.npu_embedded.with  := npu_resource npu_vfs
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
# Exclusion: tests own the image's autostart processes, so nothing else may.
# Checked only for a test build; the helper goals include this file with the
# flag off.  These read command-line and environment values, which exist this
# early; a makefile included later that sets one of them is not seen.
# ---------------------------------------------------------------------------
ifeq ($(TIKU_TESTS_ENABLE),1)
ifeq ($(HAS_TESTS),1)
$(error TIKU_TESTS_ENABLE=1 cannot be combined with HAS_TESTS=1: TikuBench's tests and these tests each own the image's autostart processes. Use one or the other)
endif
ifneq ($(strip $(APP)),)
$(error TIKU_TESTS_ENABLE=1 cannot be combined with APP=$(APP): an app and a test each own the image's autostart processes)
endif
ifneq ($(strip $(DEMO)),)
$(error TIKU_TESTS_ENABLE=1 cannot be combined with DEMO=$(DEMO): a demo and a test each own the image's autostart processes)
endif
ifeq ($(TIKU_STM32N6_DK_EDGE_DETECT_ENABLE),1)
$(error TIKU_TESTS_ENABLE=1 cannot be combined with TIKU_STM32N6_DK_EDGE_DETECT_ENABLE=1: the edge detection app and a test each own the image's autostart processes)
endif
ifeq ($(TESTS_SELECTED),)
ifeq ($(filter test-list test-config,$(MAKECMDGOALS)),)
$(error TIKU_TESTS_ENABLE=1 but no test is selected. Set TEST_<name>=1 in tests/tests.config or on the make line; run `make test-list`)
endif
endif
endif

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

# One image, one test.  The selected tests must be connected through .requires
# and .with (in either direction).  _tests_nbrs lists a test's neighbours,
# _tests_expand grows a set by one step along selected neighbours, and four
# doublings reach every test in a registry of up to 16 steps.
_tests_nbrs = $(sort $(T.$(1).requires) $(T.$(1).with)     $(foreach u,$(TESTS_ALL),$(if $(filter $(1),$(T.$(u).requires) $(T.$(u).with)),$(u))))
_tests_expand = $(sort $(1) $(foreach t,$(1),$(filter $(TESTS_SELECTED),$(call _tests_nbrs,$(t)))))
_tests_reach  = $(call _tests_expand,$(call _tests_expand,$(1)))
_tests_reach4 = $(call _tests_reach,$(call _tests_reach,$(1)))
_TESTS_FIRST  := $(firstword $(TESTS_SELECTED))
_TESTS_GROUP1 := $(call _tests_reach4,$(_TESTS_FIRST))
_TESTS_OTHERS := $(filter-out $(_TESTS_GROUP1),$(TESTS_SELECTED))
ifneq ($(_TESTS_OTHERS),)
$(error tests cannot be enabled together: '$(strip $(_TESTS_GROUP1))' and '$(strip $(_TESTS_OTHERS))' are unrelated, and every test autostarts its own process. Enable one test, with the tests it requires or runs with)
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
	$(foreach t,$(TESTS_ALL),$(info [$(if $(filter $(t),$(TESTS_SELECTED)),x, )] $(t)$(if $(T.$(t).group), group=$(T.$(t).group))$(if $(T.$(t).board), board=$(T.$(t).board))$(if $(T.$(t).requires), requires=$(T.$(t).requires))$(if $(T.$(t).with), with=$(T.$(t).with))$(if $(T.$(t).note), -- $(T.$(t).note))))@:

test-config:
	$(info tests    : $(if $(TESTS_SELECTED),$(TESTS_SELECTED),(none)))
	$(info sources  : $(sort $(TEST_SRCS)))
	$(info cflags   : $(TEST_CFLAGS))
	$(info NPU      : $(if $(filter NPU,$(_TESTS_NEEDS)),forced on,not required))
	@:

.DEFAULT_GOAL := $(_TESTS_SAVED_GOAL)
