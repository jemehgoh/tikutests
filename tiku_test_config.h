/*
 * tiku_test_config.h - compile-time checks on the set of enabled tests.
 *
 * tests/tests.mk enforces these rules when the Makefile selects tests.  This
 * header repeats them for builds that bypass it, such as EXTRA_CFLAGS=-D...
 * Every firmware test includes it, so a bad combination stops the build in
 * whichever test file is compiled.
 *
 * The rules, as in tests.mk:
 *   - every test autostarts its own process, so the enabled tests must form
 *     one group: a test and the tests it requires or runs with;
 *   - a test that needs another (requires) is an error without it.
 *
 * When a test is added to tests.mk, add its macro below.
 */
#ifndef TIKU_TEST_CONFIG_H
#define TIKU_TEST_CONFIG_H

/* 1 when a test macro is defined as non-zero, 0 when undefined or 0. */
#define TIKU_TEST_ON_(x) ((x) + 0 ? 1 : 0)

/* Requires. */
#if TIKU_TEST_ON_(TIKU_NPU_MEM_NEGATIVE_TEST_ENABLE) && \
    !TIKU_TEST_ON_(TIKU_NPU_MEM_TEST_ENABLE)
#error "TIKU_NPU_MEM_NEGATIVE_TEST_ENABLE needs TIKU_NPU_MEM_TEST_ENABLE"
#endif
#if TIKU_TEST_ON_(TIKU_NPU_RUN_TEST_ENABLE) && \
    !TIKU_TEST_ON_(TIKU_NPU_EMBEDDED_TEST_ENABLE)
#error "TIKU_NPU_RUN_TEST_ENABLE needs TIKU_NPU_EMBEDDED_TEST_ENABLE"
#endif

/* One group per image.  Tests that belong together count once: the memory
 * pair, and the embedded group.  The embedded group links resource and vfs
 * only through npu_embedded, as in tests.mk: with embedded off they are two
 * unrelated tests.  (A run test without embedded is caught above.) */
#define TIKU_TEST_GROUPS_ \
    ( TIKU_TEST_ON_(TIKU_STM32N6_REGION_TEST_ENABLE) \
    + TIKU_TEST_ON_(TIKU_STM32N6_DK_TFT_TEST_ENABLE) \
    + TIKU_TEST_ON_(TIKU_STM32N6_DK_TFT_COLORCYCLE_TEST_ENABLE) \
    + TIKU_TEST_ON_(TIKU_STM32N6_DK_DCMIPP_CSI192_TEST_ENABLE) \
    + TIKU_TEST_ON_(TIKU_STM32N6_DK_IMX335_TEST_ENABLE) \
    + TIKU_TEST_ON_(TIKU_STM32N6_DK_DCMIPP_CAPTURE_TEST_ENABLE) \
    + TIKU_TEST_ON_(TIKU_STM32N6_DK_DCMIPP_PIPE2_640_TEST_ENABLE) \
    + TIKU_TEST_ON_(TIKU_STM32N6_DK_DCMIPP_DUAL_TEST_ENABLE) \
    + (TIKU_TEST_ON_(TIKU_NPU_MEM_TEST_ENABLE) \
       || TIKU_TEST_ON_(TIKU_NPU_MEM_NEGATIVE_TEST_ENABLE)) \
    + (TIKU_TEST_ON_(TIKU_NPU_EMBEDDED_TEST_ENABLE) ? 1 \
       : TIKU_TEST_ON_(TIKU_NPU_RESOURCE_TEST_ENABLE) \
         + TIKU_TEST_ON_(TIKU_NPU_VFS_TEST_ENABLE)) \
    + TIKU_TEST_ON_(TIKU_NPU_PREFLIGHT_TEST_ENABLE) \
    + TIKU_TEST_ON_(TIKU_NPU_POOL_TEST_ENABLE) \
    + TIKU_TEST_ON_(TIKU_NPU_LIFECYCLE_TEST_ENABLE) )

#if TIKU_TEST_GROUPS_ > 1
#error "More than one test is enabled. Every test autostarts its own process; enable one test, with the tests it requires or runs with (see tests/tests.mk)"
#endif

#endif /* TIKU_TEST_CONFIG_H */
