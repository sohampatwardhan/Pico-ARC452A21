#include "pico_arc452a21/daikin_ir.h"

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"
#include "pico_arc452a21/daikin_frame.h"

enum {
    DAIKIN_SYMBOL_COUNT = 292,
};

typedef struct {
    uint16_t mark_us;
    uint16_t space_us;
} ir_symbol_t;

typedef struct {
    const char *name;
    ir_symbol_t preamble[6];
    ir_symbol_t section_leader;
    ir_symbol_t section_gap;
    ir_symbol_t bit_zero;
    ir_symbol_t bit_one;
    ir_symbol_t terminal_mark;
} daikin_timing_config_t;

static const daikin_timing_config_t TIMING_PROFILES[] = {
    [DAIKIN_TIMING_PROFILE_CAPTURED] = {
        .name = "captured",
        .preamble = {
            {531, 330},
            {510, 356},
            {507, 359},
            {504, 362},
            {500, 389},
            {474, 25076},
        },
        .section_leader = {3462, 1728},
        .section_gap = {437, 2743},
        .bit_zero = {438, 428},
        .bit_one = {438, 1294},
        .terminal_mark = {439, 0},
    },
    [DAIKIN_TIMING_PROFILE_NOMINAL] = {
        .name = "nominal",
        .preamble = {
            {531, 330},
            {510, 356},
            {507, 359},
            {504, 362},
            {500, 389},
            {474, 25076},
        },
        .section_leader = {3360, 1760},
        .section_gap = {360, 32300},
        .bit_zero = {360, 520},
        .bit_one = {360, 1370},
        .terminal_mark = {360, 0},
    },
};

static unsigned int s_gpio;
static unsigned int s_pwm_slice;
static unsigned int s_pwm_channel;
static bool s_initialized;
static bool s_invert_out;
static uint8_t s_repeat_count = 1;
static uint32_t s_repeat_gap_ms = 80;
static daikin_timing_profile_t s_timing_profile = DAIKIN_TIMING_PROFILE_NOMINAL;

static const daikin_timing_config_t *current_timing(void)
{
    return &TIMING_PROFILES[s_timing_profile];
}

static const char *mode_name(daikin_mode_t mode)
{
    switch (mode) {
    case DAIKIN_MODE_AUTO:
        return "auto";
    case DAIKIN_MODE_DRY:
        return "dry";
    case DAIKIN_MODE_COOL:
        return "cool";
    case DAIKIN_MODE_HEAT:
        return "heat";
    case DAIKIN_MODE_FAN:
        return "fan";
    }
    return "unknown";
}

static const char *fan_name(daikin_fan_t fan)
{
    switch (fan) {
    case DAIKIN_FAN_SPEED_1:
        return "1";
    case DAIKIN_FAN_SPEED_2:
        return "2";
    case DAIKIN_FAN_SPEED_3:
        return "3";
    case DAIKIN_FAN_SPEED_4:
        return "4";
    case DAIKIN_FAN_SPEED_5:
        return "5";
    case DAIKIN_FAN_AUTO:
        return "auto";
    case DAIKIN_FAN_NIGHT:
        return "night";
    }
    return "unknown";
}

static void carrier_off(void)
{
    pwm_set_enabled(s_pwm_slice, false);
    gpio_set_function(s_gpio, GPIO_FUNC_SIO);
    gpio_set_dir(s_gpio, GPIO_OUT);
    gpio_put(s_gpio, s_invert_out ? 1 : 0);
}

static void carrier_on(void)
{
    gpio_set_function(s_gpio, GPIO_FUNC_PWM);
    pwm_set_enabled(s_pwm_slice, true);
}

static void send_symbol(ir_symbol_t symbol)
{
    if (symbol.mark_us > 0) {
        carrier_on();
        busy_wait_us(symbol.mark_us);
    }

    carrier_off();
    if (symbol.space_us > 0) {
        busy_wait_us(symbol.space_us);
    }
}

static void log_section(const char *label, const uint8_t *section, size_t len)
{
    printf("DAIKIN_TX_%s", label);
    for (size_t i = 0; i < len; ++i) {
        printf(",%02X", section[i]);
    }
    printf("\n");
}

static void log_frame(const daikin_frame_t *frame)
{
    log_section("SECTION_1", frame->section_1, DAIKIN_FRAME_SECTION_1_LEN);
    log_section("SECTION_2", frame->section_2, DAIKIN_FRAME_SECTION_2_LEN);
    log_section("SECTION_3", frame->section_3, DAIKIN_FRAME_SECTION_3_LEN);
}

static void append_symbol(ir_symbol_t symbols[DAIKIN_SYMBOL_COUNT],
                          size_t *count,
                          ir_symbol_t symbol)
{
    symbols[*count] = symbol;
    ++(*count);
}

static void append_bytes_lsb(ir_symbol_t symbols[DAIKIN_SYMBOL_COUNT],
                             size_t *count,
                             const uint8_t *bytes,
                             size_t len,
                             const daikin_timing_config_t *timing)
{
    for (size_t i = 0; i < len; ++i) {
        for (uint8_t bit = 0; bit < 8; ++bit) {
            append_symbol(symbols, count,
                          (bytes[i] & (1u << bit)) ? timing->bit_one : timing->bit_zero);
        }
    }
}

static pico_arc_status_t build_symbols(const daikin_frame_t *frame,
                                       ir_symbol_t symbols[DAIKIN_SYMBOL_COUNT],
                                       size_t *symbol_count)
{
    const daikin_timing_config_t *timing = current_timing();
    size_t count = 0;
    for (size_t i = 0; i < sizeof(timing->preamble) / sizeof(timing->preamble[0]); ++i) {
        append_symbol(symbols, &count, timing->preamble[i]);
    }

    append_symbol(symbols, &count, timing->section_leader);
    append_bytes_lsb(symbols, &count, frame->section_1, DAIKIN_FRAME_SECTION_1_LEN, timing);

    append_symbol(symbols, &count, timing->section_gap);
    append_symbol(symbols, &count, timing->section_leader);
    append_bytes_lsb(symbols, &count, frame->section_2, DAIKIN_FRAME_SECTION_2_LEN, timing);

    append_symbol(symbols, &count, timing->section_gap);
    append_symbol(symbols, &count, timing->section_leader);
    append_bytes_lsb(symbols, &count, frame->section_3, DAIKIN_FRAME_SECTION_3_LEN, timing);

    append_symbol(symbols, &count, timing->terminal_mark);

    *symbol_count = count;
    return count == DAIKIN_SYMBOL_COUNT ? PICO_ARC_OK : PICO_ARC_ERR_INVALID_STATE;
}

pico_arc_status_t daikin_ir_init(const daikin_ir_config_t *config)
{
    if (config == NULL || config->carrier_hz == 0) {
        return PICO_ARC_ERR_INVALID_ARG;
    }

    s_gpio = config->ir_gpio;
    s_pwm_slice = pwm_gpio_to_slice_num(s_gpio);
    s_pwm_channel = pwm_gpio_to_channel(s_gpio);
    s_invert_out = config->invert_out;
    s_repeat_count = config->repeat_count == 0 ? 1 : config->repeat_count;
    s_repeat_gap_ms = config->repeat_gap_ms;
    s_timing_profile = config->timing_profile;

    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t wrap = (sys_hz / config->carrier_hz) - 1;
    if (wrap > 65535u) {
        wrap = 65535u;
    }

    uint16_t level = (uint16_t)((float)(wrap + 1u) * config->carrier_duty_cycle);
    gpio_set_drive_strength(s_gpio, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_slew_rate(s_gpio, GPIO_SLEW_RATE_FAST);
    pwm_config pwm_cfg = pwm_get_default_config();
    pwm_config_set_wrap(&pwm_cfg, (uint16_t)wrap);
    pwm_init(s_pwm_slice, &pwm_cfg, false);
    pwm_set_chan_level(s_pwm_slice, s_pwm_channel, level);
    pwm_set_output_polarity(s_pwm_slice,
                            s_pwm_channel == PWM_CHAN_A ? s_invert_out : false,
                            s_pwm_channel == PWM_CHAN_B ? s_invert_out : false);
    carrier_off();
    s_initialized = true;

    printf("IR ready on GPIO %u at %lu Hz, timing=%s, repeat=%u, gap=%lu ms\n",
           s_gpio,
           (unsigned long)config->carrier_hz,
           current_timing()->name,
           (unsigned)s_repeat_count,
           (unsigned long)s_repeat_gap_ms);
    return PICO_ARC_OK;
}

pico_arc_status_t daikin_ir_set_invert_out(bool invert_out)
{
    if (!s_initialized) {
        return PICO_ARC_ERR_INVALID_STATE;
    }
    s_invert_out = invert_out;
    pwm_set_output_polarity(s_pwm_slice,
                            s_pwm_channel == PWM_CHAN_A ? s_invert_out : false,
                            s_pwm_channel == PWM_CHAN_B ? s_invert_out : false);
    carrier_off();
    return PICO_ARC_OK;
}

void daikin_ir_set_repeat(uint8_t repeat_count, uint32_t repeat_gap_ms)
{
    s_repeat_count = repeat_count == 0 ? 1 : repeat_count;
    s_repeat_gap_ms = repeat_gap_ms;
}

pico_arc_status_t daikin_ir_set_timing_profile(daikin_timing_profile_t profile)
{
    if (profile > DAIKIN_TIMING_PROFILE_NOMINAL) {
        return PICO_ARC_ERR_INVALID_ARG;
    }
    s_timing_profile = profile;
    return PICO_ARC_OK;
}

pico_arc_status_t daikin_ir_test_carrier(uint32_t duration_ms)
{
    if (!s_initialized) {
        return PICO_ARC_ERR_INVALID_STATE;
    }

    if (duration_ms == 0) {
        duration_ms = 1000;
    } else if (duration_ms > 10000) {
        duration_ms = 10000;
    }

    printf("IR carrier test: GPIO %u, polarity=%s, duration=%lu ms\n",
           s_gpio,
           s_invert_out ? "invert" : "normal",
           (unsigned long)duration_ms);
    carrier_on();
    sleep_ms(duration_ms);
    carrier_off();
    return PICO_ARC_OK;
}

pico_arc_status_t daikin_ir_send_state(const daikin_state_t *state)
{
    if (state == NULL) {
        return PICO_ARC_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return PICO_ARC_ERR_INVALID_STATE;
    }

    daikin_frame_t frame;
    pico_arc_status_t status = daikin_frame_build(state, &frame);
    if (status != PICO_ARC_OK) {
        return status;
    }
    log_frame(&frame);

    ir_symbol_t symbols[DAIKIN_SYMBOL_COUNT];
    size_t symbol_count = 0;
    status = build_symbols(&frame, symbols, &symbol_count);
    if (status != PICO_ARC_OK) {
        return status;
    }

    printf("Sending Daikin frame: power=%s mode=%s temp=%u %c fan=%s symbols=%u timing=%s\n",
           state->power ? "on" : "off",
           mode_name(state->mode),
           state->use_fahrenheit ? state->target_fahrenheit : state->target_celsius,
           state->use_fahrenheit ? 'F' : 'C',
           fan_name(state->fan),
           (unsigned)symbol_count,
           current_timing()->name);

    for (uint8_t repeat = 0; repeat < s_repeat_count; ++repeat) {
        for (size_t i = 0; i < symbol_count; ++i) {
            send_symbol(symbols[i]);
        }

        if (repeat + 1 < s_repeat_count && s_repeat_gap_ms > 0) {
            sleep_ms(s_repeat_gap_ms);
        }
    }

    carrier_off();
    return PICO_ARC_OK;
}
