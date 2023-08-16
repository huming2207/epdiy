#include "include/epd_board.h"
#include "render_method.h"

#ifdef RENDER_METHOD_I2S
#include <string.h>
#include <stdint.h>

#include "esp_log.h"

// output a row to the display.
#include "render_i2s.h"
#include "render.h"
#include "i2s_data_bus.h"
#include "display_ops.h"
#include "lut.h"
#include "include/epd_internals.h"
#include "rmt_pulse.h"

#include <stdint.h>

static const epd_ctrl_state_t NoChangeState = {0};

/**
 * Waits until all previously submitted data has been written.
 * Then, the following operations are initiated:
 *
 *  - Previously submitted data is latched to the output register.
 *  - The RMT peripheral is set up to pulse the vertical (gate) driver for
 *  `output_time_dus` / 10 microseconds.
 *  - The I2S peripheral starts transmission of the current buffer to
 *  the source driver.
 *  - The line buffers are switched.
 *
 * This sequence of operations allows for pipelining data preparation and
 * transfer, reducing total refresh times.
 */
static void IRAM_ATTR i2s_output_row(uint32_t output_time_dus) {
    while (i2s_is_busy() || rmt_busy()) {
    };

    epd_ctrl_state_t* ctrl_state = epd_ctrl_state();
    epd_ctrl_state_t mask = NoChangeState;


    ctrl_state->ep_sth = true;
    ctrl_state->ep_latch_enable = true;
    mask.ep_sth = true;
    mask.ep_latch_enable = true;
    epd_board->set_ctrl(ctrl_state, &mask);

    mask = NoChangeState;
    ctrl_state->ep_latch_enable = false;
    mask.ep_latch_enable = true;
    epd_board->set_ctrl(ctrl_state, &mask);

#if defined(CONFIG_EPD_DISPLAY_TYPE_ED097TC2) ||                               \
    defined(CONFIG_EPD_DISPLAY_TYPE_ED133UT2)
    pulse_ckv_ticks(output_time_dus, 1, false);
#else
    pulse_ckv_ticks(output_time_dus, 50, false);
#endif

    i2s_start_line_output();
    i2s_switch_buffer();
}

/** Line i2s_output_row, but resets skip indicator. */
static void IRAM_ATTR i2s_write_row(RenderContext_t *ctx, uint32_t output_time_dus) {
    i2s_output_row(output_time_dus);
    ctx->skipping = 0;
}

/** Skip a row without writing to it. */
static void IRAM_ATTR i2s_skip_row(RenderContext_t *ctx,
                            uint8_t pipeline_finish_time) {
    // output previously loaded row, fill buffer with no-ops.
    if (ctx->skipping < 2) {
        memset((void*)i2s_get_current_buffer(), 0x00, EPD_LINE_BYTES);
        i2s_output_row(pipeline_finish_time);
    } else {
#if defined(CONFIG_EPD_DISPLAY_TYPE_ED097TC2) || defined(CONFIG_EPD_DISPLAY_TYPE_ED133UT2)
        pulse_ckv_ticks(5, 5, false);
#else
  // According to the spec, the OC4 maximum CKV frequency is 200kHz.
        pulse_ckv_ticks(45, 5, false);
#endif
    }
    ctx->skipping++;
}

/**
 * Start a draw cycle.
 */
static void i2s_start_frame() {
    while (i2s_is_busy() || rmt_busy()) {
    };

    epd_ctrl_state_t* ctrl_state = epd_ctrl_state();
    epd_ctrl_state_t mask = NoChangeState;

    ctrl_state->ep_mode = true;
    mask.ep_mode = true;
    epd_board->set_ctrl(ctrl_state, &mask);

    pulse_ckv_us(1, 1, true);

    // This is very timing-sensitive!
    mask = NoChangeState;
    ctrl_state->ep_stv = false;
    mask.ep_stv = true;
    epd_board->set_ctrl(ctrl_state, &mask);
    //busy_delay(240);
    pulse_ckv_us(1000, 100, false);
    mask = NoChangeState;
    ctrl_state->ep_stv = true;
    mask.ep_stv = true;
    epd_board->set_ctrl(ctrl_state, &mask);
    //pulse_ckv_us(0, 10, true);
    pulse_ckv_us(1, 1, true);
    pulse_ckv_us(1, 1, true);
    pulse_ckv_us(1, 1, true);
    pulse_ckv_us(1, 1, true);

    mask = NoChangeState;
    ctrl_state->ep_output_enable = true;
    mask.ep_output_enable = true;
    epd_board->set_ctrl(ctrl_state, &mask);
}

/**
 * End a draw cycle.
 */
static void i2s_end_frame() {
    epd_ctrl_state_t* ctrl_state = epd_ctrl_state();
    epd_ctrl_state_t mask = NoChangeState;

    ctrl_state->ep_stv = false;
    mask.ep_stv = true;
    epd_board->set_ctrl(ctrl_state, &mask);
    pulse_ckv_us(1, 1, true);
    pulse_ckv_us(1, 1, true);
    pulse_ckv_us(1, 1, true);
    pulse_ckv_us(1, 1, true);
    pulse_ckv_us(1, 1, true);
    mask = NoChangeState;
    ctrl_state->ep_mode = false;
    mask.ep_mode = true;
    epd_board->set_ctrl(ctrl_state, &mask);
    pulse_ckv_us(0, 10, true);
    mask = NoChangeState;
    ctrl_state->ep_output_enable = false;
    mask.ep_output_enable = true;
    epd_board->set_ctrl(ctrl_state, &mask);
    pulse_ckv_us(1, 1, true);
    pulse_ckv_us(1, 1, true);
    pulse_ckv_us(1, 1, true);
}

void i2s_do_update(RenderContext_t *ctx) {

    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        prepare_frame(ctx);

        // start both feeder tasks
        xTaskNotifyGive(ctx->feed_tasks[!xPortGetCoreID()]);
        xTaskNotifyGive(ctx->feed_tasks[xPortGetCoreID()]);

        // transmission is started in renderer threads, now wait util it's done
        xSemaphoreTake(ctx->frame_done, portMAX_DELAY);

        for (int i = 0; i < NUM_FEED_THREADS; i++) {
            xSemaphoreTake(ctx->feed_done_smphr[i], portMAX_DELAY);
        }

        ctx->current_frame++;

        // make the watchdog happy.
        if (k % 10 == 0) {
            vTaskDelay(0);
        }
    }
}

void IRAM_ATTR epd_push_pixels_i2s(RenderContext_t *ctx, EpdRect area, short time, int color) {

    uint8_t row[EPD_LINE_BYTES] = {0};

    const uint8_t color_choice[4] = {DARK_BYTE, CLEAR_BYTE, 0x00, 0xFF};
    for (uint32_t i = 0; i < area.width; i++) {
        uint32_t position = i + area.x % 4;
        uint8_t mask =
            color_choice[color] & (0b00000011 << (2 * (position % 4)));
        row[area.x / 4 + position / 4] |= mask;
    }
    reorder_line_buffer((uint32_t *)row);

    i2s_start_frame();

    for (int i = 0; i < EPD_HEIGHT; i++) {
        // before are of interest: skip
        if (i < area.y) {
            i2s_skip_row(ctx, time);
            // start area of interest: set row data
        } else if (i == area.y) {
            i2s_switch_buffer();
            memcpy((void*)i2s_get_current_buffer(), row, EPD_LINE_BYTES);
            i2s_switch_buffer();
            memcpy((void*)i2s_get_current_buffer(), row, EPD_LINE_BYTES);

            i2s_write_row(ctx, time * 10);
            // load nop row if done with area
        } else if (i >= area.y + area.height) {
            i2s_skip_row(ctx, time);
            // output the same as before
        } else {
            i2s_write_row(ctx, time * 10);
        }
    }
    // Since we "pipeline" row output, we still have to latch out the last row.
    i2s_write_row(ctx, time * 10);

    i2s_end_frame();
}


void IRAM_ATTR i2s_output_frame(RenderContext_t *ctx, int thread_id) {
    uint8_t line_buf[EPD_WIDTH];

    ctx->skipping = 0;
    EpdRect area = ctx->area;
    enum EpdDrawMode mode = ctx->mode;
    int frame_time = ctx->frame_time;

    lut_func_t input_calc_func = get_lut_function();

    i2s_start_frame();
    for (int i = 0; i < FRAME_LINES; i++) {
        LineQueue_t *lq = &ctx->line_queues[0];

        memset(line_buf, 0, EPD_WIDTH);
        while (lq_read(lq, line_buf) < 0) {
        };

        ctx->lines_consumed += 1;

        if (ctx->drawn_lines != NULL && !ctx->drawn_lines[i - area.y]) {
            i2s_skip_row(ctx, frame_time);
            continue;
        }

        (*input_calc_func)((uint32_t*)line_buf, (uint8_t*)i2s_get_current_buffer(),
                           ctx->conversion_lut, EPD_WIDTH);
        i2s_write_row(ctx, frame_time);
    }
    if (!ctx->skipping) {
        // Since we "pipeline" row output, we still have to latch out the
        // last row.
        i2s_write_row(ctx, frame_time);
    }
    i2s_end_frame();

    xSemaphoreGive(ctx->feed_done_smphr[thread_id]);
    xSemaphoreGive(ctx->frame_done);
}

inline int min(int x, int y) { return x < y ? x : y; }
inline int max(int x, int y) { return x > y ? x : y; }

void IRAM_ATTR i2s_feed_frame(RenderContext_t *ctx, int thread_id) {
    uint8_t line_buf[EPD_LINE_BYTES];
    uint8_t input_line[EPD_WIDTH];

    // line must be able to hold 2-pixel-per-byte or 1-pixel-per-byte data
    memset(input_line, 0x00, EPD_WIDTH);

    LineQueue_t *lq = &ctx->line_queues[thread_id];

    EpdRect area = ctx->area;

    lut_func_t input_calc_func = get_lut_function();

    int min_y, max_y, bytes_per_line, pixels_per_byte;
    const uint8_t *ptr_start;
    get_buffer_params(ctx, &bytes_per_line, &ptr_start, &min_y, &max_y, &pixels_per_byte);

    const EpdRect crop_to = ctx->crop_to;
    const bool horizontally_cropped = !(crop_to.x == 0 && crop_to.width == area.width);
    const bool vertically_cropped = !(crop_to.y == 0 && crop_to.height == area.height);
    int crop_x = (horizontally_cropped ? crop_to.x : 0);
    int crop_w = (horizontally_cropped ? crop_to.width : 0);
    int crop_y = (vertically_cropped ? crop_to.y : 0);
    int crop_h = (vertically_cropped ? crop_to.height : 0);

    // interval of the output line that is needed
    // FIXME: only lookup needed parts
    int line_start_x = area.x + (horizontally_cropped ? crop_to.x : 0);
    int line_end_x = line_start_x + (horizontally_cropped ? crop_to.width : area.width);
    line_start_x = min(max(line_start_x, 0), EPD_WIDTH);
    line_end_x = min(max(line_end_x, 0), EPD_WIDTH);

    int l = 0;
    while (l = atomic_fetch_add(&ctx->lines_prepared, 1),
           l < FRAME_LINES) {
        // if (thread_id) gpio_set_level(15, 0);
        ctx->line_threads[l] = thread_id;

        if (l < min_y || l >= max_y ||
            (ctx->drawn_lines != NULL &&
             !ctx->drawn_lines[l - area.y])) {
            uint8_t *buf = NULL;
            while (buf == NULL)
                buf = lq_current(lq);
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            continue;
        }

        uint32_t *lp = (uint32_t *)input_line;
        bool shifted = false;
        const uint8_t *ptr = ptr_start + bytes_per_line * (l - min_y);

        if (area.width == EPD_WIDTH && area.x == 0 && !ctx->error) {
            lp = (uint32_t *)ptr;
        } else if (!ctx->error) {
            uint8_t *buf_start = (uint8_t *)input_line;
            uint32_t line_bytes = bytes_per_line;

            int min_x = area.x + crop_x;
            if (min_x >= 0) {
                buf_start += min_x / pixels_per_byte;
            } else {
                // reduce line_bytes to actually used bytes
                // ptr was already adjusted above
                line_bytes += min_x / pixels_per_byte;
            }
            line_bytes =
                min(line_bytes, EPD_WIDTH / pixels_per_byte - (uint32_t)(buf_start - input_line));

            memcpy(buf_start, ptr, line_bytes);

            int cropped_width = (horizontally_cropped ? crop_w : area.width);
            /// consider half-byte shifts in two-pixel-per-Byte mode.
            if (pixels_per_byte == 2) {
                // mask last nibble for uneven width
                if (cropped_width % 2 == 1 &&
                    min_x / 2 + cropped_width / 2 + 1 < EPD_WIDTH) {
                    *(buf_start + line_bytes - 1) |= 0xF0;
                }
                if (area.x % 2 == 1 && !(crop_x % 2 == 1) &&
                    min_x < EPD_WIDTH) {
                    shifted = true;
                    uint32_t remaining = (uint32_t)input_line + EPD_WIDTH / 2 -
                                         (uint32_t)buf_start;
                    uint32_t to_shift = min(line_bytes + 1, remaining);
                    // shift one nibble to right
                    nibble_shift_buffer_right(buf_start, to_shift);
                }
                // consider bit shifts in bit buffers
            } else if (pixels_per_byte == 8) {
                // mask last n bits if width is not divisible by 8
                if (cropped_width % 8 != 0 && bytes_per_line + 1 < EPD_WIDTH) {
                    uint8_t mask = 0;
                    for (int s = 0; s < cropped_width % 8; s++) {
                        mask = (mask << 1) | 1;
                    }
                    *(buf_start + line_bytes - 1) |= ~mask;
                }

                if (min_x % 8 != 0 && min_x < EPD_WIDTH) {
                    // shift to right
                    shifted = true;
                    uint32_t remaining = (uint32_t)input_line + EPD_WIDTH / 8 -
                                         (uint32_t)buf_start;
                    uint32_t to_shift = min(line_bytes + 1, remaining);
                    bit_shift_buffer_right(buf_start, to_shift, min_x % 8);
                }
            }
            lp = (uint32_t *)input_line;
        }

        uint8_t *buf = NULL;
        while (buf == NULL)
            buf = lq_current(lq);

        memcpy(buf, lp, lq->element_size);

        if (line_start_x > 0 || line_end_x < EPD_WIDTH) {
            mask_line_buffer(line_buf, line_start_x, line_end_x);
        }

        lq_commit(lq);

        if (shifted) {
            memset(input_line, 255, EPD_WIDTH / pixels_per_byte);
        }
    }
}

#endif
