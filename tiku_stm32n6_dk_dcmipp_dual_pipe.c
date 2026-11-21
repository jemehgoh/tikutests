/* STM32N6570-DK dual-pipe capture test: Pipe1 192x192 + Pipe2 640x640.
 *
 * IMX335 -> CSI-2 -> ISP (demosaic). Pipe1 crops the centre 1536x1536 and
 * downsizes it 8x to 192x192 (the NPU input); the coupled Pipe2 crops the
 * centre 1944x1944 and downsizes it 3.04x to 640x640 RGB565 (the display
 * stream). Both pipes are armed back to back as one-shot snapshots, DUAL_CYCLES
 * times, 2 s apart. Per cycle the test checks:
 *   - one frame-complete event per pipe, no duplicates, no bus/overrun error;
 *   - both completed on the same sensor frame (CSI start-of-frame count);
 *   - every pixel of both buffers was written (fill-pattern check) and neither
 *     frame is flat;
 *   - the two pictures show the same scene: the mean absolute difference of
 *     6x6 block means between Pipe1's frame and the matching centre of Pipe2's
 *     frame stays below DUAL_MAD_LIMIT (use a static, lit scene).
 * The second and later cycles also show that a snapshot can be re-armed.
 *
 * DUAL_P1_FORMAT selects Pipe1's output: 565 (default) or 888 (RGB888, the
 * likely NPU input format). Pipe2 is always RGB565.
 *
 * Build: make BOARD=stm32n6570_dk TIKU_STM32N6_DK_DCMIPP_DUAL_TEST_ENABLE=1
 *        [EXTRA_CFLAGS=-DDUAL_P1_FORMAT=888] */
#include <stddef.h>
#include <stdint.h>

#include <kernel/process/tiku_process.h>
#include <kernel/timers/tiku_timer.h>

#include <drivers/stm32n6/imx335/tiku_stm32n6570_imx335.h>

#include <arch/stm32n6/tiku_cache_arch.h>
#include <arch/stm32n6/tiku_cpu_common.h>
#include <arch/stm32n6/tiku_dcmipp_arch.h>
#include <arch/stm32n6/tiku_device_select.h>
#include <arch/stm32n6/tiku_gpio_arch.h>
#include <arch/stm32n6/tiku_i2c_arch.h>
#include <arch/stm32n6/tiku_stm32n6_regs.h>

#include "tiku_test_config.h"

#if defined(TIKU_STM32N6_DK_DCMIPP_DUAL_TEST_ENABLE) && \
    TIKU_STM32N6_DK_DCMIPP_DUAL_TEST_ENABLE

#if !defined(TIKU_BOARD_STM32N6570_DK)
#error "The STM32N6 DK dual-pipe test requires the STM32N6570-DK board"
#endif

#ifndef DUAL_P1_FORMAT
#define DUAL_P1_FORMAT      565
#endif
#if DUAL_P1_FORMAT != 565 && DUAL_P1_FORMAT != 888
#error "DUAL_P1_FORMAT must be 565 or 888"
#endif
#ifndef DUAL_CYCLES
#define DUAL_CYCLES         5U
#endif
#ifndef DUAL_MAD_LIMIT
/* Measured on hardware: matching pictures 0.2..1.2, shifted control about 20. */
#define DUAL_MAD_LIMIT      4U
#endif

/* IMX335 profile is 2592x1944. */
#define SENSOR_WIDTH        2592U
#define SENSOR_HEIGHT       1944U

/* Pipe2: centre 1944 square -> 640x640 RGB565. */
#define P2_CROP_SIZE        1944U
#define P2_CROP_X           ((SENSOR_WIDTH - P2_CROP_SIZE) / 2U)
#define P2_CROP_Y           0U
#define P2_W                640U
#define P2_H                640U
#define P2_PIXELS           (P2_W * P2_H)
#define P2_PITCH            (P2_W * 2U)
#define P2_BYTES            (P2_PITCH * P2_H)
#define P2_RATIO            ((8192U * (P2_CROP_SIZE - 1U) + P2_W - 1U) / P2_W)
#define P2_DIV              ((1024U * P2_W + P2_CROP_SIZE - 1U) / P2_CROP_SIZE)

/* Pipe1: centre 1536 square -> 192x192 (largest crop within the 8x limit). */
#define P1_CROP_SIZE        1536U
#define P1_CROP_X           ((SENSOR_WIDTH - P1_CROP_SIZE) / 2U)
#define P1_CROP_Y           ((SENSOR_HEIGHT - P1_CROP_SIZE) / 2U)
#define P1_W                192U
#define P1_H                192U
#define P1_PIXELS           (P1_W * P1_H)
#if DUAL_P1_FORMAT == 888
#define P1_BPP              3U
#define P1_PACKER           TIKU_DCMIPP_PIXEL_PACKER_RGB888
#else
#define P1_BPP              2U
#define P1_PACKER           TIKU_DCMIPP_PIXEL_PACKER_RGB565
#endif
#define P1_PITCH            (P1_W * P1_BPP)
#define P1_BYTES            (P1_PITCH * P1_H)
#define P1_RATIO            ((8192U * (P1_CROP_SIZE - 1U) + P1_W - 1U) / P1_W)
#define P1_DIV              ((1024U * P1_W + P1_CROP_SIZE - 1U) / P1_CROP_SIZE)

/* Consistency grid: Pipe1's whole frame vs the part of Pipe2's frame it
 * covers (1536/1944 * 640 = 506 pixels; 6 blocks of 84 = 504, centred). */
#define GRID                6U
#define P1_BLOCK            (P1_W / GRID)
#define P2_BLOCK            84U
#define P2_GRID_X0          ((P2_W - GRID * P2_BLOCK) / 2U)
#define P2_GRID_Y0          ((P2_H - GRID * P2_BLOCK) / 2U)

#define SETTLE_TICKS        TIKU_CLOCK_SECOND
#define CYCLE_TIMEOUT_TICKS (3U * TIKU_CLOCK_SECOND)
#define CYCLE_GAP_TICKS     (2U * TIKU_CLOCK_SECOND)
#define FILL_BYTE           0xA5U
#define FILL_WORD           0xA5A5U

#define CSI_FRAME_IT0       (STM32N6_CSI_IT_SOF0 | STM32N6_CSI_IT_EOF0)
#define CSI_ERROR_IT0       (STM32N6_CSI_IT_SYNCERR | STM32N6_CSI_IT_WDERR | \
                             STM32N6_CSI_IT_SPKTERR | STM32N6_CSI_IT_IDERR | \
                             STM32N6_CSI_IT_ECCERR | STM32N6_CSI_IT_CRCERR | \
                             STM32N6_CSI_IT_CCFIFOF)
#define CSI_BENIGN_IT0      STM32N6_CSI_IT_CECCERR
#define CSI_LANE_IT1        STM32N6_CSI_IER1_ALL
#define CSI_ALL_IT0         (CSI_FRAME_IT0 | CSI_ERROR_IT0 | CSI_BENIGN_IT0)

static uint8_t p1_buf[P1_PIXELS * 3U]
    __attribute__((section(".axisram"), aligned(64)));
static uint16_t p2_buf[P2_PIXELS]
    __attribute__((section(".axisram"), aligned(64)));

static struct tiku_timer dual_timer;
TIKU_PROCESS(stm32n6_dk_dcmipp_dual, "STM32N6 DK DCMIPP dual pipe");

typedef enum {
    DUAL_STAGE_I2C = 1,
    DUAL_STAGE_SENSOR_INIT,
    DUAL_STAGE_ENABLE,
    DUAL_STAGE_CSI,
    DUAL_STAGE_CSI_IRQ,
    DUAL_STAGE_PIPE_DIFF,
    DUAL_STAGE_AXI_PLUG,
    DUAL_STAGE_ISP_SETUP,
    DUAL_STAGE_P1_CONFIG,
    DUAL_STAGE_P1_POST,
    DUAL_STAGE_P1_ENABLE,
    DUAL_STAGE_P2_POST,
    DUAL_STAGE_P2_CONFIG,
    DUAL_STAGE_P2_CROP,
    DUAL_STAGE_ISP_ENABLE,
    DUAL_STAGE_POST_ENABLE,
    DUAL_STAGE_P2_ENABLE,
    DUAL_STAGE_INTERRUPTS,
    DUAL_STAGE_INPUT_ENABLE,
    DUAL_STAGE_SENSOR_START,
    DUAL_STAGE_START_CAPTURE,
} dual_stage_t;

static uint8_t dual_sensor_streaming;

#define DUAL_STEP(stage_id, call)                    \
    do {                                             \
        int rc_ = (call);                            \
        if (rc_ != TIKU_DCMIPP_OK) {                 \
            *failed_stage = (stage_id);              \
            *error_code = rc_;                       \
            return -1;                               \
        }                                            \
    } while (0)

/* Per-cycle event bookkeeping. */
#define GOT_P1              0x1U
#define GOT_P2              0x2U
typedef struct {
    uint32_t got;
    uint32_t sof;
    uint32_t sof_p1;
    uint32_t sof_p2;
    uint32_t dup1;
    uint32_t dup2;
    uint32_t lane_errors;
    uint32_t other_errors;
    int fatal;
} dual_events_t;

/* Power the module and program the sensor, leaving it in standby. */
static int dual_sensor_init(dual_stage_t *failed_stage, int *error_code)
{
    const tiku_i2c_config_t i2c = {
        .speed = TIKU_I2C_SPEED_STANDARD,
    };
    int rc;

    /* PC8 (camera reset) and the I2C pads sit in the VDDIO4 pad bank, which
     * reads 0 and drives nothing until its supply is declared valid. */
    TIKU_REG32(STM32N6_PWR_SVMCR1) |= STM32N6_PWR_SVMCR1_VDDIO4SV;
    tiku_cpu_stm32n6_delay_ms(1U);

    /* Same edge order as ST's BSP_CAMERA_HwReset: PC8 low, PD2 low, PC8 high,
     * PD2 high, 100 ms apart. PC8 must rise before PD2. */
    TIKU_BOARD_CAMERA_INIT();
    tiku_cpu_stm32n6_delay_ms(100U);
    TIKU_BOARD_CAMERA_RESET_RELEASE();
    tiku_cpu_stm32n6_delay_ms(100U);
    TIKU_BOARD_CAMERA_ENABLE();
    tiku_cpu_stm32n6_delay_ms(100U);

    rc = tiku_i2c_init(&i2c);
    if (rc != TIKU_I2C_OK) {
        *failed_stage = DUAL_STAGE_I2C;
        *error_code = rc;
        return -1;
    }
    rc = tiku_stm32n6570_imx335_init();
    if (rc != TIKU_IMX335_OK) {
        *failed_stage = DUAL_STAGE_SENSOR_INIT;
        *error_code = rc;
        return -1;
    }
    return 0;
}

/* Put the fill pattern into both buffers and push it to memory, then drop the
 * cache lines, so any pixel still holding it after a capture was not written. */
static void dual_refill(void)
{
    uint32_t i;

    for (i = 0U; i < sizeof(p1_buf); ++i) {
        p1_buf[i] = FILL_BYTE;
    }
    for (i = 0U; i < P2_PIXELS; ++i) {
        p2_buf[i] = FILL_WORD;
    }
    tiku_stm32n6_dcache_clean(p1_buf, sizeof(p1_buf));
    tiku_stm32n6_dcache_invalidate(p1_buf, sizeof(p1_buf));
    tiku_stm32n6_dcache_clean(p2_buf, sizeof(p2_buf));
    tiku_stm32n6_dcache_invalidate(p2_buf, sizeof(p2_buf));
}

/* Configure the receiver, the ISP, both pipes, then start the sensor. */
static int dual_pipeline_start(dual_stage_t *failed_stage, int *error_code)
{
    const tiku_dcmipp_csi_config_t csi = {
        .lane_count = TIKU_DCMIPP_CSI_TWO_DATA_LANES,
        .lane_mapping = TIKU_DCMIPP_CSI_LANES_PHYSICAL,
        .phy_bitrate = TIKU_DCMIPP_CSI_PHY_BT_1600,
        .data_type = TIKU_DCMIPP_CSI_DT_RAW10,
        .virtual_channel = 0U,
    };
    const tiku_dcmipp_pipe_config_t pipe1 = {
        .DataTypeMode = 0U,
        .DataTypeIDA = 0x2BU,
        .DataTypeIDB = 0x2BU,
        .frame_rate = 0U,
        .pixel_packer = P1_PACKER,
    };
    const tiku_dcmipp_pipe_config_t pipe2 = {
        .frame_rate = 0U,
        .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB565,
    };
    const tiku_dcmipp_isp_config_t isp = {
        .demosaic = 1U, /* RGGB, linear interpolation. */
    };
    const tiku_dcmipp_postproc_config_t pipe1_post = {
        .crop_start = P1_CROP_X | (P1_CROP_Y << 16),
        .crop_size = P1_CROP_SIZE | (P1_CROP_SIZE << 16),
        .downsize_control = P1_DIV | (P1_DIV << 16),
        .downsize_ratio = P1_RATIO | (P1_RATIO << 16),
        .downsize_destination_size = P1_W | (P1_H << 16),
        .pixel_packer = P1_PACKER,
        .pitch = {P1_PITCH, 0U, 0U},
    };
    const tiku_dcmipp_postproc_config_t pipe2_post = {
        .crop_start = P2_CROP_X | (P2_CROP_Y << 16),
        .crop_size = P2_CROP_SIZE | (P2_CROP_SIZE << 16),
        .downsize_control = P2_DIV | (P2_DIV << 16),
        .downsize_ratio = P2_RATIO | (P2_RATIO << 16),
        .downsize_destination_size = P2_W | (P2_H << 16),
        .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB565,
        .pitch = {P2_PITCH, 0U, 0U},
    };
    int rc;

    dual_refill();

    DUAL_STEP(DUAL_STAGE_ENABLE, tiku_dcmipp_arch_enable());
    DUAL_STEP(DUAL_STAGE_CSI, tiku_dcmipp_arch_config_csi(&csi));
    DUAL_STEP(DUAL_STAGE_CSI_IRQ,
              tiku_dcmipp_arch_csi_interrupt_enable(CSI_ALL_IT0,
                                                    CSI_LANE_IT1));
    DUAL_STEP(DUAL_STAGE_PIPE_DIFF,
              tiku_dcmipp_arch_set_pipe_diff(DCMIPP_PIPEDIFF_COUPLED));
    DUAL_STEP(DUAL_STAGE_AXI_PLUG,
              tiku_dcmipp_arch_axi_plug_configure_pipe2());
    DUAL_STEP(DUAL_STAGE_ISP_SETUP, tiku_dcmipp_arch_isp_setup(&isp));

    /* Pipe1 (this is also the ISP's front end for the coupled Pipe2). */
    DUAL_STEP(DUAL_STAGE_P1_CONFIG,
              tiku_dcmipp_arch_pipe_configure(PIPE1, &pipe1));
    DUAL_STEP(DUAL_STAGE_P1_POST,
              tiku_dcmipp_arch_postprocessing_configure(PIPE1, &pipe1_post));
    DUAL_STEP(DUAL_STAGE_P1_ENABLE, tiku_dcmipp_arch_pipe_enable(PIPE1));

    /* Pipe2: pipe_configure rewrites the crop registers from its config
     * (zero), so the crop-with-enable-bit call must come after it. */
    DUAL_STEP(DUAL_STAGE_P2_POST,
              tiku_dcmipp_arch_postprocessing_configure(PIPE2, &pipe2_post));
    DUAL_STEP(DUAL_STAGE_P2_CONFIG,
              tiku_dcmipp_arch_pipe_configure(PIPE2, &pipe2));
    DUAL_STEP(DUAL_STAGE_P2_CROP,
              tiku_dcmipp_arch_pipe_crop_rect(PIPE2, P2_CROP_X, P2_CROP_Y,
                                              P2_CROP_SIZE, P2_CROP_SIZE));

    DUAL_STEP(DUAL_STAGE_ISP_ENABLE, tiku_dcmipp_arch_isp_enable());
    DUAL_STEP(DUAL_STAGE_POST_ENABLE,
              tiku_dcmipp_arch_postprocessing_enable(PIPE1));
    DUAL_STEP(DUAL_STAGE_POST_ENABLE,
              tiku_dcmipp_arch_postprocessing_enable(PIPE2));
    DUAL_STEP(DUAL_STAGE_P2_ENABLE, tiku_dcmipp_arch_pipe_enable(PIPE2));
    DUAL_STEP(DUAL_STAGE_INTERRUPTS,
              tiku_dcmipp_arch_pipe_interrupt_enable(
                  PIPE1, STM32N6_DCMIPP_IT_PIPE1_FRAME |
                             STM32N6_DCMIPP_IT_PIPE1_OVR |
                             STM32N6_DCMIPP_IT_AXI_TRANSFER_ERR));
    DUAL_STEP(DUAL_STAGE_INTERRUPTS,
              tiku_dcmipp_arch_pipe_interrupt_enable(
                  PIPE2, STM32N6_DCMIPP_IT_PIPE2_FRAME |
                             STM32N6_DCMIPP_IT_PIPE2_OVR));
    DUAL_STEP(DUAL_STAGE_INPUT_ENABLE, tiku_dcmipp_arch_enable_input());

    rc = tiku_stm32n6570_imx335_start();
    if (rc != TIKU_IMX335_OK) {
        *failed_stage = DUAL_STAGE_SENSOR_START;
        *error_code = rc;
        return -1;
    }
    dual_sensor_streaming = 1U;
    return 0;
}

/* Arm both snapshots back to back: nothing between the two calls, so both
 * requests latch before the next frame start. */
static int dual_arm(dual_stage_t *failed_stage, int *error_code)
{
    const tiku_dcmipp_plane_address_t out1 = {
        .plane = {(uintptr_t)p1_buf, 0U, 0U},
        .count = 1U,
    };
    const tiku_dcmipp_plane_address_t out2 = {
        .plane = {(uintptr_t)p2_buf, 0U, 0U},
        .count = 1U,
    };
    int rc1 = tiku_dcmipp_arch_pipe_start_capture(PIPE1, &out1, 0U,
                                                  DCMIPP_CAPTURE_SNAPSHOT);
    int rc2 = tiku_dcmipp_arch_pipe_start_capture(PIPE2, &out2, 0U,
                                                  DCMIPP_CAPTURE_SNAPSHOT);

    if (rc1 != TIKU_DCMIPP_OK || rc2 != TIKU_DCMIPP_OK) {
        *failed_stage = DUAL_STAGE_START_CAPTURE;
        *error_code = rc1 != TIKU_DCMIPP_OK ? rc1 : rc2;
        return -1;
    }
    return 0;
}

/* Leave the sensor stopped and the module in reset and unpowered. */
static void dual_teardown(void)
{
    if (dual_sensor_streaming != 0U) {
        (void)tiku_stm32n6570_imx335_stop();
        dual_sensor_streaming = 0U;
    }
    (void)tiku_dcmipp_arch_csi_interrupt_disable(CSI_ALL_IT0, CSI_LANE_IT1);
    TIKU_BOARD_CAMERA_RESET_ASSERT();
    TIKU_BOARD_CAMERA_DISABLE();
    tiku_i2c_close();
}

/* Account for one DCMIPP event. A fatal bus/overrun error sets ev->fatal to
 * its code. Lane control and corrected-ECC errors are counted, not fatal. */
static void dual_event(tiku_event_data_t data, dual_events_t *ev)
{
    tiku_dcmipp_event_t event;

    if (tiku_dcmipp_arch_event_from_data(data, &event) != TIKU_DCMIPP_OK) {
        return;
    }
    if (event.type == DCMIPP_EVENT_FRAME_COMPLETE) {
        if (event.pipe == PIPE1) {
            if ((ev->got & GOT_P1) != 0U) {
                ++ev->dup1;
            } else {
                ev->got |= GOT_P1;
                ev->sof_p1 = ev->sof;
            }
        } else if (event.pipe == PIPE2) {
            if ((ev->got & GOT_P2) != 0U) {
                ++ev->dup2;
            } else {
                ev->got |= GOT_P2;
                ev->sof_p2 = ev->sof;
            }
        }
    } else if (event.type == DCMIPP_EVENT_CSI_SOF) {
        ++ev->sof;
    } else if (event.type == DCMIPP_EVENT_ERROR) {
        if (event.error == DCMIPP_ERROR_CSI_DPHY_LANE ||
            event.error == DCMIPP_ERROR_CSI_CORRECTED_ECC) {
            ++ev->lane_errors;
        } else if (event.error == DCMIPP_ERROR_AXI_TRANSFER ||
                   event.error == DCMIPP_ERROR_PIPE_OVERRUN) {
            ev->fatal = (int)event.error;
        } else {
            ++ev->other_errors;
        }
    }
}

/* Green (6-bit channel of RGB565, or the middle byte of RGB888) as 0..255. */
static unsigned dual_p1_green(uint32_t pixel_index)
{
#if DUAL_P1_FORMAT == 888
    return p1_buf[pixel_index * 3U + 1U];
#else
    const uint16_t *w = (const uint16_t *)p1_buf;

    return (unsigned)((w[pixel_index] >> 5) & 0x3FU) << 2;
#endif
}

static unsigned dual_p2_green(uint32_t pixel_index)
{
    return (unsigned)((p2_buf[pixel_index] >> 5) & 0x3FU) << 2;
}

/* True when pixel i of Pipe1 still holds the fill pattern. */
static int dual_p1_unwritten(uint32_t i)
{
#if DUAL_P1_FORMAT == 888
    return p1_buf[i * 3U] == FILL_BYTE && p1_buf[i * 3U + 1U] == FILL_BYTE &&
           p1_buf[i * 3U + 2U] == FILL_BYTE;
#else
    return ((const uint16_t *)p1_buf)[i] == FILL_WORD;
#endif
}

/* Unwritten pixel counts and green spread (max - min) of both frames. */
static void dual_frame_stats(uint32_t *unw1, unsigned *spread1,
                             uint32_t *unw2, unsigned *spread2)
{
    unsigned min = 255U;
    unsigned max = 0U;
    uint32_t i;

    *unw1 = 0U;
    for (i = 0U; i < P1_PIXELS; ++i) {
        unsigned g = dual_p1_green(i);

        if (dual_p1_unwritten(i)) {
            ++*unw1;
        }
        if (g < min) {
            min = g;
        }
        if (g > max) {
            max = g;
        }
    }
    *spread1 = max - min;

    min = 255U;
    max = 0U;
    *unw2 = 0U;
    for (i = 0U; i < P2_PIXELS; ++i) {
        unsigned g = dual_p2_green(i);

        if (p2_buf[i] == FILL_WORD) {
            ++*unw2;
        }
        if (g < min) {
            min = g;
        }
        if (g > max) {
            max = g;
        }
    }
    *spread2 = max - min;
}

/* Mean absolute difference of the GRIDxGRID block means of Pipe1's frame and
 * the matching centre of Pipe2's frame. */
static unsigned dual_consistency(int32_t shift_x)
{
    unsigned total = 0U;
    uint32_t by;
    uint32_t bx;

    for (by = 0U; by < GRID; ++by) {
        for (bx = 0U; bx < GRID; ++bx) {
            unsigned long sum1 = 0UL;
            unsigned long sum2 = 0UL;
            unsigned m1;
            unsigned m2;
            uint32_t x;
            uint32_t y;

            for (y = 0U; y < P1_BLOCK; ++y) {
                for (x = 0U; x < P1_BLOCK; ++x) {
                    sum1 += dual_p1_green((by * P1_BLOCK + y) * P1_W +
                                          bx * P1_BLOCK + x);
                }
            }
            for (y = 0U; y < P2_BLOCK; ++y) {
                for (x = 0U; x < P2_BLOCK; ++x) {
                    sum2 += dual_p2_green(
                        (P2_GRID_Y0 + by * P2_BLOCK + y) * P2_W +
                        (uint32_t)((int32_t)P2_GRID_X0 + shift_x) +
                        bx * P2_BLOCK + x);
                }
            }
            m1 = (unsigned)(sum1 / (P1_BLOCK * P1_BLOCK));
            m2 = (unsigned)(sum2 / (P2_BLOCK * P2_BLOCK));
            total += m1 > m2 ? m1 - m2 : m2 - m1;
        }
    }
    /* Tenths of a grey level, so a near-perfect match is not rounded to 0. */
    return (total * 10U) / (GRID * GRID);
}

/* Failure dump: which stage of which pipe stopped. */
static void dual_diag(void)
{
    uint32_t f1 = 0U;
    uint32_t f2 = 0U;

    (void)tiku_dcmipp_arch_get_frame_count(PIPE1, &f1);
    (void)tiku_dcmipp_arch_get_frame_count(PIPE2, &f2);
    TIKU_PRINTF("DUAL: pipe1 frames=%u FSCR=0x%x FCTCR=0x%x SR=0x%x "
                "PPCR=0x%x\n", (unsigned)f1,
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P1FSCR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P1FCTCR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P1SR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P1PPCR));
    TIKU_PRINTF("DUAL: pipe2 frames=%u FSCR=0x%x FCTCR=0x%x SR=0x%x "
                "PPCR=0x%x\n", (unsigned)f2,
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P2FSCR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P2FCTCR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P2SR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P2PPCR));
    TIKU_PRINTF("DUAL: common CMSR2=0x%x ipplug IPGR3=0x%x IPC5R1=0x%x\n",
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_CMSR2),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_IPGR3),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_IPC5R1));
}

TIKU_PROCESS_THREAD(stm32n6_dk_dcmipp_dual, ev, data)
{
    static dual_stage_t failed_stage;
    static int error_code;
    static dual_events_t st;
    static uint32_t cycle;
    static uint32_t passed;
    static int waiting;
    static int ok;

    TIKU_PROCESS_BEGIN();

    failed_stage = DUAL_STAGE_I2C;
    error_code = 0;
    passed = 0U;

    if (dual_sensor_init(&failed_stage, &error_code) != 0 ||
        dual_pipeline_start(&failed_stage, &error_code) != 0) {
        dual_teardown();
        TIKU_PRINTF("DUAL: FAIL stage=%u code=%d\n", (unsigned)failed_stage,
                    error_code);
        TIKU_PRINTF("DUAL: result=FAIL\n");
    } else {
        TIKU_PRINTF("DUAL: build %s %s, pipe1 %ux%u %s, pipe2 %ux%u RGB565, "
                    "settling\n", __DATE__, __TIME__, P1_W, P1_H,
                    DUAL_P1_FORMAT == 888 ? "RGB888" : "RGB565", P2_W, P2_H);
        tiku_timer_set_event(&dual_timer, SETTLE_TICKS);
        do {
            TIKU_PROCESS_WAIT_EVENT();
            if (ev == TIKU_DCMIPP_EVENT_ID) {
                dual_event((tiku_event_data_t)data, &st);
            }
        } while (ev != TIKU_EVENT_TIMER);

        for (cycle = 1U; cycle <= DUAL_CYCLES; ++cycle) {
            ok = 1;
            st.got = 0U;
            st.sof = 0U;
            st.sof_p1 = 0U;
            st.sof_p2 = 0U;
            st.dup1 = 0U;
            st.dup2 = 0U;
            st.fatal = 0;
            dual_refill();

            if (dual_arm(&failed_stage, &error_code) != 0) {
                TIKU_PRINTF("DUAL: cycle=%u arm failed stage=%u code=%d\n",
                            (unsigned)cycle, (unsigned)failed_stage,
                            error_code);
                ok = 0;
            } else {
                tiku_timer_set_event(&dual_timer, CYCLE_TIMEOUT_TICKS);
                waiting = 1;
                while (waiting) {
                    TIKU_PROCESS_WAIT_EVENT();
                    if (ev == TIKU_DCMIPP_EVENT_ID) {
                        dual_event((tiku_event_data_t)data, &st);
                        if (st.got == (GOT_P1 | GOT_P2) || st.fatal != 0) {
                            waiting = 0;
                        }
                    } else if (ev == TIKU_EVENT_TIMER) {
                        waiting = 0;
                    }
                }
                tiku_timer_stop(&dual_timer);
            }

            if (ok) {
                if (st.fatal != 0) {
                    TIKU_PRINTF("DUAL: cycle=%u bus/overrun error %d\n",
                                (unsigned)cycle, st.fatal);
                    ok = 0;
                } else if (st.got != (GOT_P1 | GOT_P2)) {
                    TIKU_PRINTF("DUAL: cycle=%u timeout got=0x%x sof=%u "
                                "lane_err=%u other_err=%u\n",
                                (unsigned)cycle, (unsigned)st.got,
                                (unsigned)st.sof, (unsigned)st.lane_errors,
                                (unsigned)st.other_errors);
                    ok = 0;
                }
            }
            if (ok) {
                uint32_t unw1;
                uint32_t unw2;
                unsigned spread1;
                unsigned spread2;
                unsigned mad;
                unsigned mad_ctl;

                /* Let any write still in flight land, then drop the cache
                 * lines: the DCMIPP wrote behind the cache's back. */
                tiku_cpu_stm32n6_delay_ms(100U);
                tiku_stm32n6_dcache_invalidate(p1_buf, sizeof(p1_buf));
                tiku_stm32n6_dcache_invalidate(p2_buf, sizeof(p2_buf));
                dual_frame_stats(&unw1, &spread1, &unw2, &spread2);
                mad = dual_consistency(0);
                /* Negative control: the same comparison with Pipe2's grid
                 * moved 168 pixels left must disagree much more. */
                mad_ctl = dual_consistency(-168);
                TIKU_PRINTF("DUAL: cycle=%u sof_p1=%u sof_p2=%u dup=%u/%u "
                            "unwritten=%u/%u spread=%u/%u mad=%u.%u "
                            "ctl=%u.%u lane_err=%u other_err=%u\n",
                            (unsigned)cycle,
                            (unsigned)st.sof_p1, (unsigned)st.sof_p2,
                            (unsigned)st.dup1, (unsigned)st.dup2,
                            (unsigned)unw1, (unsigned)unw2, spread1, spread2,
                            mad / 10U, mad % 10U, mad_ctl / 10U,
                            mad_ctl % 10U, (unsigned)st.lane_errors,
                            (unsigned)st.other_errors);
#if DUAL_P1_FORMAT == 888
                {
                    uint32_t c = (P1_H / 2U) * P1_W + P1_W / 2U;

                    TIKU_PRINTF("DUAL: pipe1 centre pixel bytes %u %u %u\n",
                                (unsigned)p1_buf[c * 3U],
                                (unsigned)p1_buf[c * 3U + 1U],
                                (unsigned)p1_buf[c * 3U + 2U]);
                }
#endif
                if (st.dup1 != 0U || st.dup2 != 0U) {
                    TIKU_PRINTF("DUAL: cycle=%u duplicate frame events\n",
                                (unsigned)cycle);
                    ok = 0;
                }
                if (st.sof_p1 != st.sof_p2) {
                    TIKU_PRINTF("DUAL: cycle=%u pipes completed on different "
                                "frames\n", (unsigned)cycle);
                    ok = 0;
                }
                if (unw1 != 0U || unw2 != 0U) {
                    TIKU_PRINTF("DUAL: cycle=%u unwritten pixels\n",
                                (unsigned)cycle);
                    ok = 0;
                }
                if (spread1 == 0U || spread2 == 0U) {
                    TIKU_PRINTF("DUAL: cycle=%u flat frame\n",
                                (unsigned)cycle);
                    ok = 0;
                }
                if (mad > DUAL_MAD_LIMIT * 10U) {
                    TIKU_PRINTF("DUAL: cycle=%u pictures disagree (mad %u > "
                                "%u)\n", (unsigned)cycle, mad / 10U,
                                (unsigned)DUAL_MAD_LIMIT);
                    ok = 0;
                }
            }
            if (!ok) {
                dual_diag();
            } else {
                ++passed;
            }

            if (cycle < DUAL_CYCLES) {
                tiku_timer_set_event(&dual_timer, CYCLE_GAP_TICKS);
                do {
                    TIKU_PROCESS_WAIT_EVENT();
                } while (ev != TIKU_EVENT_TIMER);
            }
        }

        dual_teardown();
        TIKU_PRINTF("DUAL: cycles=%u pass=%u\n", (unsigned)DUAL_CYCLES,
                    (unsigned)passed);
        TIKU_PRINTF("DUAL: result=%s\n",
                    passed == DUAL_CYCLES ? "PASS" : "FAIL");
    }

    while (1) {
        TIKU_PROCESS_WAIT_EVENT();
    }

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&stm32n6_dk_dcmipp_dual);

#endif /* TIKU_STM32N6_DK_DCMIPP_DUAL_TEST_ENABLE */
