/* STM32N6570-DK Pipe2 capture test: IMX335 -> CSI-2 -> 640x640 on Pipe2.
 *
 * Same shape as tiku_stm32n6_dk_dcmipp_capture.c, but the capturing pipe is the
 * coupled Pipe2: the Pipe1 ISP (demosaic) output feeds Pipe2, which crops the
 * centre 1944x1944 of the 2592x1944 frame and downsizes it (3.04x) to 640x640
 * RGB565. Pipe1 is enabled only as the ISP's front end (flow selection) and is
 * never given a buffer or a capture request, so it writes nothing.
 *
 * The buffer is pre-filled with a pattern the pipe cannot produce, so a frame
 * the DCMIPP reports but never wrote shows up as unwritten pixels. The test
 * fails on unwritten pixels, a perfectly constant frame, a bus or overrun
 * error, or a timeout. A merely dark frame passes with a warning: the sensor
 * has no auto-exposure here. Any Pipe1 frame event is reported as a warning so
 * a pass can be attributed to Pipe2.
 *
 * Pipe2 had never delivered a frame on hardware before this test. On failure a
 * register dump (see pipe2_diag) shows which stage stopped.
 *
 * PIPE2_TEST_VARIANT (default 0) selects fallback experiments:
 *   0  coupled Pipe2, Pipe1 front end enabled, Pipe1 idle
 *   1  as 0, plus a Pipe1 192x192 snapshot into a scratch buffer in the same
 *      trigger (does the ISP only produce output while Pipe1 captures?)
 *   2  Pipe2 independent of the ISP, fed straight from the CSI (no demosaic, so
 *      the picture is not meaningful; only "does Pipe2 write memory at all")
 *
 * Build: make BOARD=stm32n6570_dk TIKU_STM32N6_DK_DCMIPP_PIPE2_640_TEST_ENABLE=1
 *        [CFLAGS+=-DPIPE2_TEST_VARIANT=1] */
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

#if defined(TIKU_STM32N6_DK_DCMIPP_PIPE2_640_TEST_ENABLE) && \
    TIKU_STM32N6_DK_DCMIPP_PIPE2_640_TEST_ENABLE

#if !defined(TIKU_BOARD_STM32N6570_DK)
#error "The STM32N6 DK Pipe2 640 test requires the STM32N6570-DK board"
#endif

#ifndef PIPE2_TEST_VARIANT
#define PIPE2_TEST_VARIANT  0
#endif
#if PIPE2_TEST_VARIANT < 0 || PIPE2_TEST_VARIANT > 2
#error "PIPE2_TEST_VARIANT must be 0, 1 or 2"
#endif

/* IMX335 profile is 2592x1944. The centre 1944x1944 square downsized to 640 is
 * 3.04x, well inside the 8x (ratio 8192..65535) pipe limit. */
#define SENSOR_WIDTH        2592U
#define SENSOR_HEIGHT       1944U
#define CROP_SIZE           1944U
#define CROP_X              ((SENSOR_WIDTH - CROP_SIZE) / 2U)
#define CROP_Y              0U
#define OUT_WIDTH           640U
#define OUT_HEIGHT          640U
#define OUT_PIXELS          (OUT_WIDTH * OUT_HEIGHT)
#define OUT_PITCH           (OUT_WIDTH * 2U) /* RGB565 */
#define OUT_BYTES           (OUT_PITCH * OUT_HEIGHT)
/* ratio = ceil(8192 * (src - 1) / dst), divider = ceil(1024 * dst / src). */
#define DOWNSIZE_RATIO      ((8192U * (CROP_SIZE - 1U) + OUT_WIDTH - 1U) / \
                             OUT_WIDTH)
#define DOWNSIZE_DIV        ((1024U * OUT_WIDTH + CROP_SIZE - 1U) / CROP_SIZE)

/* Variant 1: Pipe1 scratch snapshot, same geometry as the 192x192 capture test
 * (1536 crop is the largest whose 8x downsize stays in range). */
#define P1_CROP_SIZE        1536U
#define P1_CROP_X           ((SENSOR_WIDTH - P1_CROP_SIZE) / 2U)
#define P1_CROP_Y           ((SENSOR_HEIGHT - P1_CROP_SIZE) / 2U)
#define P1_OUT              192U
#define P1_PITCH            (P1_OUT * 2U)
#define P1_RATIO            ((8192U * (P1_CROP_SIZE - 1U) + P1_OUT - 1U) / \
                             P1_OUT)
#define P1_DIV              ((1024U * P1_OUT + P1_CROP_SIZE - 1U) / P1_CROP_SIZE)

#define CAPTURE_TIMEOUT_TICKS   (3U * TIKU_CLOCK_SECOND)
/* Streaming time before the snapshot is armed. */
#define CAPTURE_SETTLE_TICKS    TIKU_CLOCK_SECOND
/* Below this green spread the frame is reported as dark (a warning, not a
 * failure). A perfectly constant frame, or any pixel left unwritten, fails. */
#define DARK_PIXEL_SPREAD       8U
/* Written to the buffer before the capture; seeing it afterwards means the
 * pipe never wrote that pixel. */
#define CAPTURE_FILL_PATTERN    0xA5A5U
#define PREVIEW_COLS            40U
#define PREVIEW_ROWS            20U

/* Corrected single-bit ECC errors are harmless; the other CSI errors are
 * counted and reported. */
#define CSI_FRAME_IT0       (STM32N6_CSI_IT_SOF0 | STM32N6_CSI_IT_EOF0)
#define CSI_ERROR_IT0       (STM32N6_CSI_IT_SYNCERR | STM32N6_CSI_IT_WDERR | \
                             STM32N6_CSI_IT_SPKTERR | STM32N6_CSI_IT_IDERR | \
                             STM32N6_CSI_IT_ECCERR | STM32N6_CSI_IT_CRCERR | \
                             STM32N6_CSI_IT_CCFIFOF)
#define CSI_BENIGN_IT0      STM32N6_CSI_IT_CECCERR
#define CSI_LANE_IT1        STM32N6_CSI_IER1_ALL
#define CSI_ALL_IT0         (CSI_FRAME_IT0 | CSI_ERROR_IT0 | CSI_BENIGN_IT0)

static uint16_t pipe2_buffer[OUT_PIXELS]
    __attribute__((section(".axisram"), aligned(64)));
#if PIPE2_TEST_VARIANT == 1
static uint16_t pipe1_scratch[P1_OUT * P1_OUT]
    __attribute__((section(".axisram"), aligned(64)));
#endif

static struct tiku_timer pipe2_timer;
TIKU_PROCESS(stm32n6_dk_dcmipp_pipe2_640, "STM32N6 DK DCMIPP Pipe2 640");

typedef enum {
    PIPE2_STAGE_I2C = 1,
    PIPE2_STAGE_SENSOR_INIT,
    PIPE2_STAGE_ENABLE,
    PIPE2_STAGE_CSI,
    PIPE2_STAGE_CSI_IRQ,
    PIPE2_STAGE_PIPE_DIFF,
    PIPE2_STAGE_AXI_PLUG,
    PIPE2_STAGE_ISP_SETUP,
    PIPE2_STAGE_PIPE1_CONFIG,
    PIPE2_STAGE_PIPE1_POST,
    PIPE2_STAGE_PIPE1_ENABLE,
    PIPE2_STAGE_PIPE2_POST,
    PIPE2_STAGE_PIPE2_CONFIG,
    PIPE2_STAGE_PIPE2_CROP,
    PIPE2_STAGE_ISP_ENABLE,
    PIPE2_STAGE_POST_ENABLE,
    PIPE2_STAGE_PIPE2_ENABLE,
    PIPE2_STAGE_INTERRUPTS,
    PIPE2_STAGE_INPUT_ENABLE,
    PIPE2_STAGE_SENSOR_START,
    PIPE2_STAGE_START_CAPTURE,
    PIPE2_STAGE_TIMEOUT,
    PIPE2_STAGE_PIPE_ERROR,
    PIPE2_STAGE_FLAT_FRAME,
    PIPE2_STAGE_NOT_WRITTEN,
} pipe2_stage_t;

static uint8_t pipe2_sensor_streaming;

#define PIPE2_STEP(stage_id, call)                   \
    do {                                             \
        int rc_ = (call);                            \
        if (rc_ != TIKU_DCMIPP_OK) {                 \
            *failed_stage = (stage_id);              \
            *error_code = rc_;                       \
            return -1;                               \
        }                                            \
    } while (0)

/* Power the module and program the sensor, leaving it in standby. */
static int pipe2_sensor_init(pipe2_stage_t *failed_stage, int *error_code)
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
    TIKU_BOARD_CAMERA_RESET_RELEASE();      /* PC8 high */
    tiku_cpu_stm32n6_delay_ms(100U);
    TIKU_BOARD_CAMERA_ENABLE();             /* PD2 high */
    tiku_cpu_stm32n6_delay_ms(100U);

    rc = tiku_i2c_init(&i2c);
    if (rc != TIKU_I2C_OK) {
        *failed_stage = PIPE2_STAGE_I2C;
        *error_code = rc;
        return -1;
    }
    rc = tiku_stm32n6570_imx335_init();
    if (rc != TIKU_IMX335_OK) {
        *failed_stage = PIPE2_STAGE_SENSOR_INIT;
        *error_code = rc;
        return -1;
    }
    return 0;
}

/* Configure the receiver, the Pipe1 front end and ISP, and the Pipe2 pixel
 * pipe (crop, downsize, RGB565 packer), then start the sensor streaming. The
 * snapshot is armed later (see pipe2_arm). */
static int pipe2_pipeline_start(pipe2_stage_t *failed_stage, int *error_code)
{
    const tiku_dcmipp_csi_config_t csi = {
        .lane_count = TIKU_DCMIPP_CSI_TWO_DATA_LANES,
        .lane_mapping = TIKU_DCMIPP_CSI_LANES_PHYSICAL,
        .phy_bitrate = TIKU_DCMIPP_CSI_PHY_BT_1600,
        .data_type = TIKU_DCMIPP_CSI_DT_RAW10,
        .virtual_channel = 0U,
    };
    /* Pipe1 front end: flow selection for the ISP. */
    const tiku_dcmipp_pipe_config_t pipe1 = {
        .DataTypeMode = 0U,
        .DataTypeIDA = 0x2BU,
        .DataTypeIDB = 0x2BU,
        .frame_rate = 0U,
        .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB565,
    };
    const tiku_dcmipp_pipe_config_t pipe2 = {
        .DataTypeIDA = PIPE2_TEST_VARIANT == 2 ? 0x2BU : 0U,
        .frame_rate = 0U,
        .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB565,
    };
    const tiku_dcmipp_isp_config_t isp = {
        .demosaic = 1U, /* RGGB, linear interpolation. */
    };
    const tiku_dcmipp_postproc_config_t pipe2_post = {
        .crop_start = CROP_X | (CROP_Y << 16),
        .crop_size = CROP_SIZE | (CROP_SIZE << 16),
        .downsize_control = DOWNSIZE_DIV | (DOWNSIZE_DIV << 16),
        .downsize_ratio = DOWNSIZE_RATIO | (DOWNSIZE_RATIO << 16),
        .downsize_destination_size = OUT_WIDTH | (OUT_HEIGHT << 16),
        .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB565,
        .pitch = {OUT_PITCH, 0U, 0U},
    };
    uint32_t i;
    int rc;

    /* Fill the buffer with a pattern the pipe cannot produce, push it to
     * memory, then drop the cache lines. After the capture any pixel still
     * holding the pattern was never written. */
    for (i = 0U; i < OUT_PIXELS; ++i) {
        pipe2_buffer[i] = CAPTURE_FILL_PATTERN;
    }
    tiku_stm32n6_dcache_clean(pipe2_buffer, OUT_BYTES);
    tiku_stm32n6_dcache_invalidate(pipe2_buffer, OUT_BYTES);

    PIPE2_STEP(PIPE2_STAGE_ENABLE, tiku_dcmipp_arch_enable());
    PIPE2_STEP(PIPE2_STAGE_CSI, tiku_dcmipp_arch_config_csi(&csi));
    PIPE2_STEP(PIPE2_STAGE_CSI_IRQ,
               tiku_dcmipp_arch_csi_interrupt_enable(CSI_ALL_IT0,
                                                     CSI_LANE_IT1));
    PIPE2_STEP(PIPE2_STAGE_PIPE_DIFF,
               tiku_dcmipp_arch_set_pipe_diff(
                   PIPE2_TEST_VARIANT == 2 ? DCMIPP_PIPEDIFF_INDEPENDENT
                                           : DCMIPP_PIPEDIFF_COUPLED));
    /* Pipe2's IP-Plug client is not set up anywhere else. */
    PIPE2_STEP(PIPE2_STAGE_AXI_PLUG,
               tiku_dcmipp_arch_axi_plug_configure_pipe2());
    PIPE2_STEP(PIPE2_STAGE_ISP_SETUP, tiku_dcmipp_arch_isp_setup(&isp));

#if PIPE2_TEST_VARIANT != 2
    PIPE2_STEP(PIPE2_STAGE_PIPE1_CONFIG,
               tiku_dcmipp_arch_pipe_configure(PIPE1, &pipe1));
#if PIPE2_TEST_VARIANT == 1
    {
        const tiku_dcmipp_postproc_config_t pipe1_post = {
            .crop_start = P1_CROP_X | (P1_CROP_Y << 16),
            .crop_size = P1_CROP_SIZE | (P1_CROP_SIZE << 16),
            .downsize_control = P1_DIV | (P1_DIV << 16),
            .downsize_ratio = P1_RATIO | (P1_RATIO << 16),
            .downsize_destination_size = P1_OUT | (P1_OUT << 16),
            .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB565,
            .pitch = {P1_PITCH, 0U, 0U},
        };

        PIPE2_STEP(PIPE2_STAGE_PIPE1_POST,
                   tiku_dcmipp_arch_postprocessing_configure(PIPE1,
                                                             &pipe1_post));
    }
#endif
    PIPE2_STEP(PIPE2_STAGE_PIPE1_ENABLE, tiku_dcmipp_arch_pipe_enable(PIPE1));
#else
    (void)pipe1;
#endif

    PIPE2_STEP(PIPE2_STAGE_PIPE2_POST,
               tiku_dcmipp_arch_postprocessing_configure(PIPE2, &pipe2_post));
    PIPE2_STEP(PIPE2_STAGE_PIPE2_CONFIG,
               tiku_dcmipp_arch_pipe_configure(PIPE2, &pipe2));
    /* After pipe_configure, which rewrites the crop registers from its config
     * (zero): this call also sets the crop enable bit. */
    PIPE2_STEP(PIPE2_STAGE_PIPE2_CROP,
               tiku_dcmipp_arch_pipe_crop_rect(PIPE2, CROP_X, CROP_Y,
                                               CROP_SIZE, CROP_SIZE));
#if PIPE2_TEST_VARIANT != 2
    PIPE2_STEP(PIPE2_STAGE_ISP_ENABLE, tiku_dcmipp_arch_isp_enable());
#endif
    PIPE2_STEP(PIPE2_STAGE_POST_ENABLE,
               tiku_dcmipp_arch_postprocessing_enable(PIPE2));
#if PIPE2_TEST_VARIANT == 1
    PIPE2_STEP(PIPE2_STAGE_POST_ENABLE,
               tiku_dcmipp_arch_postprocessing_enable(PIPE1));
#endif
    PIPE2_STEP(PIPE2_STAGE_PIPE2_ENABLE, tiku_dcmipp_arch_pipe_enable(PIPE2));
    PIPE2_STEP(PIPE2_STAGE_INTERRUPTS,
               tiku_dcmipp_arch_pipe_interrupt_enable(
                   PIPE2, STM32N6_DCMIPP_IT_PIPE2_FRAME |
                              STM32N6_DCMIPP_IT_PIPE2_OVR |
                              STM32N6_DCMIPP_IT_AXI_TRANSFER_ERR));
#if PIPE2_TEST_VARIANT != 2
    /* Pipe1 events are enabled so a stray Pipe1 frame is visible. */
    PIPE2_STEP(PIPE2_STAGE_INTERRUPTS,
               tiku_dcmipp_arch_pipe_interrupt_enable(
                   PIPE1, STM32N6_DCMIPP_IT_PIPE1_FRAME |
                              STM32N6_DCMIPP_IT_PIPE1_OVR));
#endif
    PIPE2_STEP(PIPE2_STAGE_INPUT_ENABLE, tiku_dcmipp_arch_enable_input());

    /* Start streaming now, arm later: the first frames after a start are often
     * unexposed or blank. */
    rc = tiku_stm32n6570_imx335_start();
    if (rc != TIKU_IMX335_OK) {
        *failed_stage = PIPE2_STAGE_SENSOR_START;
        *error_code = rc;
        return -1;
    }
    pipe2_sensor_streaming = 1U;
    return 0;
}

/* Put the fill pattern back so a retried snapshot is checked afresh. */
static void pipe2_refill(void)
{
    uint32_t i;

    for (i = 0U; i < OUT_PIXELS; ++i) {
        pipe2_buffer[i] = CAPTURE_FILL_PATTERN;
    }
    tiku_stm32n6_dcache_clean(pipe2_buffer, OUT_BYTES);
    tiku_stm32n6_dcache_invalidate(pipe2_buffer, OUT_BYTES);
}

/* Arm the one-shot snapshot on Pipe2 (and, in variant 1, on Pipe1). */
static int pipe2_arm(pipe2_stage_t *failed_stage, int *error_code)
{
    const tiku_dcmipp_plane_address_t output = {
        .plane = {(uintptr_t)pipe2_buffer, 0U, 0U},
        .count = 1U,
    };

#if PIPE2_TEST_VARIANT == 1
    {
        const tiku_dcmipp_plane_address_t scratch = {
            .plane = {(uintptr_t)pipe1_scratch, 0U, 0U},
            .count = 1U,
        };

        PIPE2_STEP(PIPE2_STAGE_START_CAPTURE,
                   tiku_dcmipp_arch_pipe_start_capture(
                       PIPE1, &scratch, 0U, DCMIPP_CAPTURE_SNAPSHOT));
    }
#endif
    PIPE2_STEP(PIPE2_STAGE_START_CAPTURE,
               tiku_dcmipp_arch_pipe_start_capture(
                   PIPE2, &output, 0U, DCMIPP_CAPTURE_SNAPSHOT));
    return 0;
}

/* Leave the sensor stopped and the module in reset and unpowered. */
static void pipe2_teardown(void)
{
    if (pipe2_sensor_streaming != 0U) {
        (void)tiku_stm32n6570_imx335_stop();
        pipe2_sensor_streaming = 0U;
    }
    (void)tiku_dcmipp_arch_csi_interrupt_disable(CSI_ALL_IT0, CSI_LANE_IT1);
    TIKU_BOARD_CAMERA_RESET_ASSERT();
    TIKU_BOARD_CAMERA_DISABLE();
    tiku_i2c_close();
}

/* Green is the only 6-bit channel of RGB565 and carries most of the luma. */
static unsigned pipe2_green(uint16_t pixel)
{
    return (unsigned)((pixel >> 5) & 0x3FU) << 2;
}

static unsigned pipe2_green_min;
static unsigned pipe2_green_max;
/* Pixels still holding the fill pattern, i.e. never written by the pipe. */
static uint32_t pipe2_unwritten;

/* Print min/max/mean of the green channel and a checksum. Returns the spread. */
static unsigned pipe2_stats(void)
{
    unsigned min = 255U;
    unsigned max = 0U;
    unsigned long long sum = 0ULL;
    uint32_t checksum = 2166136261UL;
    uint32_t unwritten = 0U;
    uint32_t i;

    for (i = 0U; i < OUT_PIXELS; ++i) {
        uint16_t pixel = pipe2_buffer[i];
        unsigned g = pipe2_green(pixel);

        if (pixel == CAPTURE_FILL_PATTERN) {
            ++unwritten;
        }
        if (g < min) {
            min = g;
        }
        if (g > max) {
            max = g;
        }
        sum += g;
        checksum = (checksum ^ pixel) * 16777619UL;
    }
    pipe2_green_min = min;
    pipe2_green_max = max;
    pipe2_unwritten = unwritten;
    TIKU_PRINTF("PIPE2: green min=%u max=%u mean=%u unwritten_pixels=%u "
                "checksum=0x%x\n", min, max, (unsigned)(sum / OUT_PIXELS),
                (unsigned)unwritten, (unsigned)checksum);
    return max - min;
}

/* ASCII picture, one character per 16x32 block of pixels. */
static void pipe2_preview(void)
{
    static const char ramp[] = " .:-=+*#%@";
    const uint32_t block_w = OUT_WIDTH / PREVIEW_COLS;
    const uint32_t block_h = OUT_HEIGHT / PREVIEW_ROWS;
    const unsigned range = pipe2_green_max > pipe2_green_min ?
                           pipe2_green_max - pipe2_green_min : 1U;
    uint32_t row;
    uint32_t col;

    for (row = 0U; row < PREVIEW_ROWS; ++row) {
        char line[PREVIEW_COLS + 1U];

        for (col = 0U; col < PREVIEW_COLS; ++col) {
            unsigned sum = 0U;
            uint32_t x;
            uint32_t y;

            for (y = 0U; y < block_h; ++y) {
                for (x = 0U; x < block_w; ++x) {
                    sum += pipe2_green(
                        pipe2_buffer[(row * block_h + y) * OUT_WIDTH +
                                     col * block_w + x]);
                }
            }
            sum /= block_w * block_h;
            /* Stretch the frame's own min..max over the whole ramp. */
            if (sum < pipe2_green_min) {
                sum = pipe2_green_min;
            }
            line[col] = ramp[((sum - pipe2_green_min) *
                              (sizeof(ramp) - 2U)) / range];
        }
        line[PREVIEW_COLS] = '\0';
        TIKU_PRINTF("PIPE2: |%s|\n", line);
    }
}

/* Failure dump: where did the frame stop? Pipe2 flow/capture/status, the
 * common status, the Pipe1 front end that feeds the ISP, and the IP-Plug. */
static void pipe2_diag(void)
{
    uint32_t frames2 = 0U;
    uint32_t frames1 = 0U;

    (void)tiku_dcmipp_arch_get_frame_count(PIPE2, &frames2);
    (void)tiku_dcmipp_arch_get_frame_count(PIPE1, &frames1);
    TIKU_PRINTF("PIPE2: pipe2 frames=%u FSCR=0x%x FCTCR=0x%x SR=0x%x "
                "PPCR=0x%x\n", (unsigned)frames2,
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P2FSCR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P2FCTCR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P2SR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P2PPCR));
    TIKU_PRINTF("PIPE2: pipe1 frames=%u FSCR=0x%x FCTCR=0x%x SR=0x%x "
                "CMSR2=0x%x\n", (unsigned)frames1,
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P1FSCR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P1FCTCR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P1SR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_CMSR2));
    TIKU_PRINTF("PIPE2: ipplug IPGR3=0x%x IPC5R1=0x%x\n",
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_IPGR3),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_IPC5R1));
}

/* Account for one DCMIPP event. Returns 1 when Pipe2 completed a frame, -1 on a
 * fatal bus or overrun error (its code goes to *code), otherwise 0. Lane
 * control and corrected-ECC errors are counted but never fatal. */
static int pipe2_event(tiku_event_data_t data, uint32_t *sof,
                       uint32_t *lane_errors, uint32_t *other_errors,
                       uint32_t *pipe1_frames, int *code)
{
    tiku_dcmipp_event_t event;

    if (tiku_dcmipp_arch_event_from_data(data, &event) != TIKU_DCMIPP_OK) {
        return 0;
    }
    if (event.type == DCMIPP_EVENT_FRAME_COMPLETE) {
        if (event.pipe == PIPE2) {
            return 1;
        }
        if (event.pipe == PIPE1) {
            ++*pipe1_frames;
        }
        return 0;
    }
    if (event.type == DCMIPP_EVENT_CSI_SOF) {
        ++*sof;
    } else if (event.type == DCMIPP_EVENT_ERROR) {
        if (event.error == DCMIPP_ERROR_CSI_DPHY_LANE ||
            event.error == DCMIPP_ERROR_CSI_CORRECTED_ECC) {
            ++*lane_errors;
        } else if (event.error == DCMIPP_ERROR_AXI_TRANSFER ||
                   event.error == DCMIPP_ERROR_PIPE_OVERRUN) {
            *code = (int)event.error;
            return -1;
        } else {
            /* Typically one CSI sync error from arming mid-stream. */
            ++*other_errors;
        }
    }
    return 0;
}

static void pipe2_fail(pipe2_stage_t stage, int error_code)
{
    TIKU_PRINTF("PIPE2: FAIL stage=%u code=%d\n", (unsigned)stage,
                error_code);
    TIKU_PRINTF("PIPE2: result=FAIL\n");
}

TIKU_PROCESS_THREAD(stm32n6_dk_dcmipp_pipe2_640, ev, data)
{
    static pipe2_stage_t failed_stage;
    static int error_code;
    static int failed;
    static int captured;
    static int settled;
    static uint32_t sof_count;
    static uint32_t lane_errors;
    static uint32_t other_errors;
    static uint32_t pipe1_frames;
    static int retried;
    static int retry;

    TIKU_PROCESS_BEGIN();

    failed_stage = PIPE2_STAGE_I2C;
    error_code = 0;
    failed = 0;
    captured = 0;
    sof_count = 0U;
    lane_errors = 0U;
    other_errors = 0U;
    pipe1_frames = 0U;
    retried = 0;

    if (pipe2_sensor_init(&failed_stage, &error_code) != 0 ||
        pipe2_pipeline_start(&failed_stage, &error_code) != 0) {
        pipe2_teardown();
        pipe2_fail(failed_stage, error_code);
    } else {
        /* The build stamp shows which image is running (an SWD reset boots
         * the previous one first). */
        TIKU_PRINTF("PIPE2: build %s %s, variant %d, sensor streaming, "
                    "settling\n", __DATE__, __TIME__, PIPE2_TEST_VARIANT);
        tiku_timer_set_event(&pipe2_timer, CAPTURE_SETTLE_TICKS);
        settled = 0;
        while (!settled) {
            TIKU_PROCESS_WAIT_EVENT();
            if (ev == TIKU_DCMIPP_EVENT_ID) {
                (void)pipe2_event((tiku_event_data_t)data, &sof_count,
                                  &lane_errors, &other_errors, &pipe1_frames,
                                  &error_code);
            } else if (ev == TIKU_EVENT_TIMER) {
                settled = 1;
            }
        }
        sof_count = 0U;
        pipe1_frames = 0U;

        do {
            retry = 0;
            captured = 0;
            if (retried) {
                pipe2_refill();
            }
            if (pipe2_arm(&failed_stage, &error_code) != 0) {
                failed = 1;
            } else {
                TIKU_PRINTF("PIPE2: snapshot armed on pipe2, waiting for a "
                            "%ux%u frame\n", OUT_WIDTH, OUT_HEIGHT);
                tiku_timer_set_event(&pipe2_timer, CAPTURE_TIMEOUT_TICKS);
            }
            while (!captured && !failed) {
                TIKU_PROCESS_WAIT_EVENT();
                if (ev == TIKU_DCMIPP_EVENT_ID) {
                    int result = pipe2_event((tiku_event_data_t)data, &sof_count,
                                             &lane_errors, &other_errors,
                                             &pipe1_frames, &error_code);

                    if (result > 0) {
                        captured = 1;
                    } else if (result < 0) {
                        failed_stage = PIPE2_STAGE_PIPE_ERROR;
                        failed = 1;
                    }
                } else if (ev == TIKU_EVENT_TIMER) {
                    failed_stage = PIPE2_STAGE_TIMEOUT;
                    error_code = (int)sof_count;
                    failed = 1;
                }
            }

            if (captured) {
                unsigned spread;

                /* Give any write still in flight time to land, then drop the cache
                 * lines: DCMIPP wrote the buffer behind the cache's back. */
                tiku_cpu_stm32n6_delay_ms(100U);
                tiku_stm32n6_dcache_invalidate(pipe2_buffer, OUT_BYTES);
                TIKU_PRINTF("PIPE2: frame complete, sof=%u lane_errors=%u "
                            "other_errors=%u pipe1_frames=%u\n",
                            (unsigned)sof_count, (unsigned)lane_errors,
                            (unsigned)other_errors, (unsigned)pipe1_frames);
    #if PIPE2_TEST_VARIANT == 0
                if (pipe1_frames != 0U) {
                    TIKU_PRINTF("PIPE2: warning: pipe1 reported %u frame(s); the "
                                "result is not attributable to pipe2 alone\n",
                                (unsigned)pipe1_frames);
                }
    #endif

                spread = pipe2_stats();
                pipe2_preview();
                if (pipe2_unwritten != 0U) {
                    /* The pipe reported a frame but left pixels untouched. */
                    failed_stage = PIPE2_STAGE_NOT_WRITTEN;
                    error_code = (int)pipe2_unwritten;
                    failed = 1;
                } else if (spread == 0U) {
                    if (!retried) {
                        /* The first frames after stream start can be unexposed:
                         * try one more snapshot before calling it a failure. */
                        retried = 1;
                        retry = 1;
                        TIKU_PRINTF("PIPE2: flat frame, retry=1\n");
                    } else {
                        failed_stage = PIPE2_STAGE_FLAT_FRAME;
                        error_code = 0;
                        failed = 1;
                    }
                } else if (spread < DARK_PIXEL_SPREAD) {
                    TIKU_PRINTF("PIPE2: warning: dark frame (green spread %u); "
                                "point the camera at a lit scene\n", spread);
                }
            }
        } while (retry);

        if (failed) {
            pipe2_diag();
        }
        pipe2_teardown();

        if (failed) {
            if (failed_stage == PIPE2_STAGE_TIMEOUT) {
                TIKU_PRINTF("PIPE2: no frame in time, sof=%u "
                            "lane_errors=%u other_errors=%u pipe1_frames=%u\n",
                            (unsigned)sof_count, (unsigned)lane_errors,
                            (unsigned)other_errors, (unsigned)pipe1_frames);
            }
            pipe2_fail(failed_stage, error_code);
        } else {
            TIKU_PRINTF("PIPE2: result=PASS\n");
        }
    }

    while (1) {
        TIKU_PROCESS_WAIT_EVENT();
    }

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&stm32n6_dk_dcmipp_pipe2_640);

#endif /* TIKU_STM32N6_DK_DCMIPP_PIPE2_640_TEST_ENABLE */
