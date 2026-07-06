#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/watchdog.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "pico_arc452a21/app_state.h"
#include "pico_arc452a21/bmp280.h"
#include "pico_arc452a21/daikin_ir.h"
#include "pico_arc452a21/flash_layout.h"

#if PICO_ARC_ENABLE_IOT
#include "pico_arc452a21/iot_server.h"
#endif

#ifndef PICO_ARC_IR_GPIO
#define PICO_ARC_IR_GPIO 0
#endif

#ifndef PICO_ARC_I2C_INSTANCE
#define PICO_ARC_I2C_INSTANCE 0
#endif

#ifndef PICO_ARC_I2C_SDA_PIN
#define PICO_ARC_I2C_SDA_PIN 20
#endif

#ifndef PICO_ARC_I2C_SCL_PIN
#define PICO_ARC_I2C_SCL_PIN 21
#endif

#ifndef PICO_ARC_I2C_BAUD
#define PICO_ARC_I2C_BAUD 100000
#endif

#if PICO_ARC_I2C_INSTANCE == 1
#define APP_I2C i2c1
#else
#define APP_I2C i2c0
#endif

static app_state_snapshot_t s_snapshot;
static bmp280_t s_sensor;
static bool s_sensor_ready;
static bmp280_reading_t s_last_reading;
static int64_t s_state_updated_us;
static bool s_iot_ready;

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

static const char *sensor_name(daikin_sensor_t sensor)
{
    switch (sensor) {
    case DAIKIN_SENSOR_OFF:
        return "off";
    case DAIKIN_SENSOR_COMFORT:
        return "comfort";
    case DAIKIN_SENSOR_INTELLIGENT_EYE:
        return "eye";
    case DAIKIN_SENSOR_COMFORT_AND_INTELLIGENT_EYE:
        return "both";
    }
    return "unknown";
}

static const char *timing_name(daikin_timing_profile_t timing)
{
    return timing == DAIKIN_TIMING_PROFILE_CAPTURED ? "captured" : "nominal";
}

static int64_t state_updated_age_s(void)
{
    if (s_state_updated_us <= 0) {
        return 0;
    }
    int64_t age_us = (int64_t)time_us_64() - s_state_updated_us;
    return age_us > 0 ? age_us / 1000000 : 0;
}

static void format_state_json(bool ok,
                              const char *message,
                              char *response,
                              size_t response_size)
{
    const daikin_state_t *state = &s_snapshot.ac_state;
    snprintf(response, response_size,
             "{\"ok\":%s,\"message\":\"%s\",\"state\":{\"power\":\"%s\",\"mode\":\"%s\","
             "\"temperature\":%u,\"unit\":\"%s\",\"fan\":\"%s\",\"vswing\":%s,"
             "\"hswing\":%s,\"quiet\":%s,\"sensor\":\"%s\"},\"updated_age_s\":%lld,"
             "\"ir\":{\"polarity\":\"%s\",\"timing\":\"%s\",\"repeat\":%u,\"gap_ms\":%lu}}",
             ok ? "true" : "false",
             message == NULL ? "" : message,
             state->power ? "on" : "off",
             mode_name(state->mode),
             state->use_fahrenheit ? state->target_fahrenheit : state->target_celsius,
             state->use_fahrenheit ? "F" : "C",
             fan_name(state->fan),
             state->swing_vertical ? "true" : "false",
             state->swing_horizontal ? "true" : "false",
             state->quiet ? "true" : "false",
             sensor_name(state->sensor),
             (long long)state_updated_age_s(),
             s_snapshot.settings.ir_invert_out ? "invert" : "normal",
             timing_name(s_snapshot.settings.ir_timing_profile),
             (unsigned)s_snapshot.settings.ir_repeat_count,
             (unsigned long)s_snapshot.settings.ir_repeat_gap_ms);
}

static void lowercase(char *s)
{
    for (; *s != '\0'; ++s) {
        *s = (char)tolower((unsigned char)*s);
    }
}

static void set_temperature_f(daikin_state_t *state, uint8_t temp_f)
{
    state->use_fahrenheit = true;
    state->target_fahrenheit = temp_f;
    state->target_celsius = (uint8_t)((temp_f - 32) * 5 / 9);
}

static void set_temperature_c(daikin_state_t *state, uint8_t temp_c)
{
    state->use_fahrenheit = false;
    state->target_celsius = temp_c;
    state->target_fahrenheit = (uint8_t)((temp_c * 9 / 5) + 32);
}

static bool parse_on_off(const char *value, bool *out)
{
    if (strcmp(value, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_temperature(daikin_state_t *state, const char *value, const char *unit)
{
    char *end = NULL;
    long temp = strtol(value, &end, 10);
    if (end == value) {
        return false;
    }

    bool use_f = state->use_fahrenheit;
    if (*end == 'c') {
        use_f = false;
    } else if (*end == 'f') {
        use_f = true;
    } else if (unit != NULL) {
        if (strcmp(unit, "c") == 0 || strcmp(unit, "celsius") == 0) {
            use_f = false;
        } else if (strcmp(unit, "f") == 0 || strcmp(unit, "fahrenheit") == 0) {
            use_f = true;
        }
    }

    if (use_f) {
        if (temp < DAIKIN_MIN_TEMP_F || temp > DAIKIN_MAX_TEMP_F) {
            return false;
        }
        set_temperature_f(state, (uint8_t)temp);
    } else {
        if (temp < DAIKIN_MIN_TEMP_C || temp > DAIKIN_MAX_TEMP_C) {
            return false;
        }
        set_temperature_c(state, (uint8_t)temp);
    }
    return true;
}

static void print_help(void)
{
    puts("Commands:");
    puts("  status | send | save | help");
    puts("  on [temp] | off [temp] | 72 | 22c | temp 72 [f|c]");
    puts("  unit fahrenheit|celsius");
    puts("  mode auto|dry|cool|heat|fan");
    puts("  fan 1|2|3|4|5|auto|night");
    puts("  vswing on|off | hswing on|off | quiet on|off");
    puts("  sensor off|comfort|eye|both");
    puts("  polarity normal|invert | timing nominal|captured");
    puts("  repeat 1..10 [gap_ms]  # baseline: repeat 1 80");
    puts("  irtest [ms]       # continuous carrier burst for phone-camera testing");
    puts("  reboot");
}

static void print_status(void)
{
    const daikin_state_t *state = &s_snapshot.ac_state;
    printf("AC: power=%s mode=%s temp=%u %c fan=%s vswing=%s hswing=%s quiet=%s sensor=%s\n",
           state->power ? "on" : "off",
           mode_name(state->mode),
           state->use_fahrenheit ? state->target_fahrenheit : state->target_celsius,
           state->use_fahrenheit ? 'F' : 'C',
           fan_name(state->fan),
           state->swing_vertical ? "on" : "off",
           state->swing_horizontal ? "on" : "off",
           state->quiet ? "on" : "off",
           sensor_name(state->sensor));
    printf("IR: gpio=%u polarity=%s timing=%s repeat=%u gap=%lu ms\n",
           PICO_ARC_IR_GPIO,
           s_snapshot.settings.ir_invert_out ? "invert" : "normal",
           timing_name(s_snapshot.settings.ir_timing_profile),
           (unsigned)s_snapshot.settings.ir_repeat_count,
           (unsigned long)s_snapshot.settings.ir_repeat_gap_ms);
#if PICO_ARC_ENABLE_IOT
    if (s_iot_ready) {
        printf("IOT: mode=%s ssid=%s ip=%s rssi=%d\n",
               iot_server_is_connected() ? "sta" : "ap",
               iot_server_is_connected() ? iot_server_connected_ssid() : "Pico-ARC452A21",
               iot_server_ip_address(),
               iot_server_rssi());
    }
#else
    puts("IOT: disabled in this build");
#endif

#if PICO_ARC_ENABLE_SENSOR
    if (s_sensor_ready) {
        if (s_last_reading.has_humidity) {
            printf("ENV: %.2f C %.2f hPa %.2f %%RH\n",
                   s_last_reading.temperature_c,
                   s_last_reading.pressure_pa / 100.0f,
                   s_last_reading.humidity_percent);
        } else {
            printf("ENV: %.2f C %.2f hPa humidity=unavailable-on-BMP280\n",
                   s_last_reading.temperature_c,
                   s_last_reading.pressure_pa / 100.0f);
        }
    } else {
        puts("ENV: sensor not detected");
    }
#else
    puts("ENV: disabled in this build");
#endif
}

static void maybe_read_sensor(void)
{
#if !PICO_ARC_ENABLE_SENSOR
    return;
#endif
    if (!s_sensor_ready) {
        return;
    }
    pico_arc_status_t status = bmp280_read(&s_sensor, &s_last_reading);
    if (status != PICO_ARC_OK) {
        printf("BMP280/BME280 read failed: %d\n", status);
    }
}

static void save_snapshot(void)
{
    pico_arc_status_t status = app_state_save(&s_snapshot);
    printf("Save %s (%d)\n", status == PICO_ARC_OK ? "ok" : "failed", status);
}

static void apply_ir_settings(void)
{
    daikin_ir_set_invert_out(s_snapshot.settings.ir_invert_out);
    daikin_ir_set_timing_profile(s_snapshot.settings.ir_timing_profile);
    daikin_ir_set_repeat(s_snapshot.settings.ir_repeat_count,
                         s_snapshot.settings.ir_repeat_gap_ms);
}

static bool update_state_from_tokens(char *cmd, char *arg1, char *arg2, bool *should_send)
{
    daikin_state_t *state = &s_snapshot.ac_state;
    *should_send = true;

    if (strcmp(cmd, "status") == 0) {
        print_status();
        *should_send = false;
        return true;
    }
    if (strcmp(cmd, "help") == 0) {
        print_help();
        *should_send = false;
        return true;
    }
    if (strcmp(cmd, "save") == 0) {
        save_snapshot();
        *should_send = false;
        return true;
    }
    if (strcmp(cmd, "reboot") == 0) {
        puts("Rebooting in 1 second...");
        watchdog_enable(1000, true);
        *should_send = false;
        return true;
    }
    if (strcmp(cmd, "irtest") == 0) {
        uint32_t duration_ms = 1000;
        if (arg1 != NULL) {
            long parsed = strtol(arg1, NULL, 10);
            if (parsed < 1 || parsed > 10000) {
                return false;
            }
            duration_ms = (uint32_t)parsed;
        }
        pico_arc_status_t status = daikin_ir_test_carrier(duration_ms);
        printf("IR test %s (%d)\n", status == PICO_ARC_OK ? "ok" : "failed", status);
        *should_send = false;
        return status == PICO_ARC_OK;
    }
    if (strcmp(cmd, "send") == 0) {
        return true;
    }
    if (strcmp(cmd, "on") == 0 || strcmp(cmd, "off") == 0) {
        state->power = strcmp(cmd, "on") == 0;
        if (arg1 != NULL && !parse_temperature(state, arg1, arg2)) {
            return false;
        }
        return true;
    }
    if (strcmp(cmd, "temp") == 0) {
        return arg1 != NULL && parse_temperature(state, arg1, arg2);
    }
    if (strcmp(cmd, "unit") == 0 && arg1 != NULL) {
        uint8_t temp_f = state->target_fahrenheit;
        uint8_t temp_c = state->target_celsius;
        if (strcmp(arg1, "f") == 0 || strcmp(arg1, "fahrenheit") == 0) {
            set_temperature_f(state, temp_f);
            *should_send = false;
            return true;
        }
        if (strcmp(arg1, "c") == 0 || strcmp(arg1, "celsius") == 0) {
            set_temperature_c(state, temp_c);
            *should_send = false;
            return true;
        }
        return false;
    }
    if (strcmp(cmd, "mode") == 0 && arg1 != NULL) {
        if (strcmp(arg1, "auto") == 0) {
            state->mode = DAIKIN_MODE_AUTO;
        } else if (strcmp(arg1, "dry") == 0) {
            state->mode = DAIKIN_MODE_DRY;
        } else if (strcmp(arg1, "cool") == 0) {
            state->mode = DAIKIN_MODE_COOL;
        } else if (strcmp(arg1, "heat") == 0) {
            state->mode = DAIKIN_MODE_HEAT;
        } else if (strcmp(arg1, "fan") == 0) {
            state->mode = DAIKIN_MODE_FAN;
        } else {
            return false;
        }
        return true;
    }
    if (strcmp(cmd, "fan") == 0 && arg1 != NULL) {
        if (strcmp(arg1, "1") == 0) {
            state->fan = DAIKIN_FAN_SPEED_1;
        } else if (strcmp(arg1, "2") == 0) {
            state->fan = DAIKIN_FAN_SPEED_2;
        } else if (strcmp(arg1, "3") == 0) {
            state->fan = DAIKIN_FAN_SPEED_3;
        } else if (strcmp(arg1, "4") == 0) {
            state->fan = DAIKIN_FAN_SPEED_4;
        } else if (strcmp(arg1, "5") == 0) {
            state->fan = DAIKIN_FAN_SPEED_5;
        } else if (strcmp(arg1, "auto") == 0) {
            state->fan = DAIKIN_FAN_AUTO;
        } else if (strcmp(arg1, "night") == 0) {
            state->fan = DAIKIN_FAN_NIGHT;
        } else {
            return false;
        }
        return true;
    }
    if ((strcmp(cmd, "vswing") == 0 || strcmp(cmd, "hswing") == 0 ||
         strcmp(cmd, "quiet") == 0) &&
        arg1 != NULL) {
        bool value = false;
        if (!parse_on_off(arg1, &value)) {
            return false;
        }
        if (strcmp(cmd, "vswing") == 0) {
            state->swing_vertical = value;
        } else if (strcmp(cmd, "hswing") == 0) {
            state->swing_horizontal = value;
        } else {
            state->quiet = value;
        }
        return true;
    }
    if (strcmp(cmd, "sensor") == 0 && arg1 != NULL) {
        if (strcmp(arg1, "off") == 0) {
            state->sensor = DAIKIN_SENSOR_OFF;
        } else if (strcmp(arg1, "comfort") == 0) {
            state->sensor = DAIKIN_SENSOR_COMFORT;
        } else if (strcmp(arg1, "eye") == 0) {
            state->sensor = DAIKIN_SENSOR_INTELLIGENT_EYE;
        } else if (strcmp(arg1, "both") == 0) {
            state->sensor = DAIKIN_SENSOR_COMFORT_AND_INTELLIGENT_EYE;
        } else {
            return false;
        }
        return true;
    }
    if (strcmp(cmd, "polarity") == 0 && arg1 != NULL) {
        if (strcmp(arg1, "normal") == 0) {
            s_snapshot.settings.ir_invert_out = false;
        } else if (strcmp(arg1, "invert") == 0) {
            s_snapshot.settings.ir_invert_out = true;
        } else {
            return false;
        }
        apply_ir_settings();
        app_state_save(&s_snapshot);
        *should_send = false;
        return true;
    }
    if (strcmp(cmd, "timing") == 0 && arg1 != NULL) {
        if (strcmp(arg1, "nominal") == 0) {
            s_snapshot.settings.ir_timing_profile = DAIKIN_TIMING_PROFILE_NOMINAL;
        } else if (strcmp(arg1, "captured") == 0) {
            s_snapshot.settings.ir_timing_profile = DAIKIN_TIMING_PROFILE_CAPTURED;
        } else {
            return false;
        }
        apply_ir_settings();
        app_state_save(&s_snapshot);
        *should_send = false;
        return true;
    }
    if (strcmp(cmd, "repeat") == 0 && arg1 != NULL) {
        long repeat = strtol(arg1, NULL, 10);
        long gap = arg2 != NULL ? strtol(arg2, NULL, 10) : s_snapshot.settings.ir_repeat_gap_ms;
        if (repeat < 1 || repeat > 10 || gap < 0 || gap > 1000) {
            return false;
        }
        s_snapshot.settings.ir_repeat_count = (uint8_t)repeat;
        s_snapshot.settings.ir_repeat_gap_ms = (uint32_t)gap;
        apply_ir_settings();
        app_state_save(&s_snapshot);
        *should_send = false;
        return true;
    }

    return parse_temperature(state, cmd, arg1);
}

static void process_line(char *line)
{
    lowercase(line);
    char *save = NULL;
    char *cmd = strtok_r(line, " \t\r\n", &save);
    char *arg1 = strtok_r(NULL, " \t\r\n", &save);
    char *arg2 = strtok_r(NULL, " \t\r\n", &save);
    if (cmd == NULL) {
        return;
    }

    bool should_send = false;
    if (!update_state_from_tokens(cmd, arg1, arg2, &should_send)) {
        puts("Invalid command. Type help.");
        return;
    }

    if (should_send) {
        pico_arc_status_t status = daikin_ir_send_state(&s_snapshot.ac_state);
        printf("TX %s (%d)\n", status == PICO_ARC_OK ? "ok" : "failed", status);
        if (status == PICO_ARC_OK) {
            s_state_updated_us = (int64_t)time_us_64();
            app_state_save(&s_snapshot);
        }
    }
}

pico_arc_status_t pico_arc_command_json(const char *command, char *response, size_t response_size)
{
    if (command == NULL || response == NULL || response_size == 0) {
        return PICO_ARC_ERR_INVALID_ARG;
    }

    char line[96] = {};
    snprintf(line, sizeof(line), "%s", command);
    lowercase(line);

    char *save = NULL;
    char *cmd = strtok_r(line, " \t\r\n", &save);
    char *arg1 = strtok_r(NULL, " \t\r\n", &save);
    char *arg2 = strtok_r(NULL, " \t\r\n", &save);
    if (cmd == NULL) {
        snprintf(response, response_size, "{\"ok\":false,\"error\":\"empty command\"}");
        return PICO_ARC_ERR_INVALID_ARG;
    }

    bool should_send = false;
    if (!update_state_from_tokens(cmd, arg1, arg2, &should_send)) {
        snprintf(response, response_size, "{\"ok\":false,\"error\":\"invalid command\"}");
        return PICO_ARC_ERR_INVALID_ARG;
    }

    pico_arc_status_t status = PICO_ARC_OK;
    if (should_send) {
        status = daikin_ir_send_state(&s_snapshot.ac_state);
        if (status == PICO_ARC_OK) {
            s_state_updated_us = (int64_t)time_us_64();
            app_state_save(&s_snapshot);
        }
    }

    format_state_json(status == PICO_ARC_OK,
                      status == PICO_ARC_OK ? "ok" : "TX failed",
                      response,
                      response_size);
    return status;
}

static void init_sensor(void)
{
#if !PICO_ARC_ENABLE_SENSOR
    puts("BMP280/BME280 startup skipped");
    return;
#endif
    i2c_init(APP_I2C, PICO_ARC_I2C_BAUD);
    gpio_set_function(PICO_ARC_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICO_ARC_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICO_ARC_I2C_SDA_PIN);
    gpio_pull_up(PICO_ARC_I2C_SCL_PIN);

    pico_arc_status_t status = bmp280_init(&s_sensor, APP_I2C, 0x76);
    if (status != PICO_ARC_OK) {
        status = bmp280_init(&s_sensor, APP_I2C, 0x77);
    }

    s_sensor_ready = status == PICO_ARC_OK;
    if (s_sensor_ready) {
        printf("%s detected at I2C address 0x%02X\n",
               s_sensor.has_humidity ? "BME280" : "BMP280",
               s_sensor.address);
        maybe_read_sensor();
    } else {
        printf("BMP280/BME280 not detected on I2C%u SDA=%u SCL=%u\n",
               PICO_ARC_I2C_INSTANCE,
               PICO_ARC_I2C_SDA_PIN,
               PICO_ARC_I2C_SCL_PIN);
    }
}

int main(void)
{
    stdio_init_all();
    sleep_ms(1500);

    printf("\nPico-ARC452A21 booting\n");
    printf("Flash: total=%u firmware=%u storage=%u bytes\n",
           (unsigned)PICO_ARC_FLASH_TOTAL_BYTES,
           (unsigned)PICO_ARC_FLASH_FIRMWARE_BYTES,
           (unsigned)PICO_ARC_FLASH_STORAGE_BYTES);

    pico_arc_status_t load_status = app_state_load(&s_snapshot);
    printf("State load: %s (%d)\n",
           load_status == PICO_ARC_OK ? "restored" : "defaults",
           load_status);

    daikin_ir_config_t ir_config = DAIKIN_IR_DEFAULT_CONFIG(PICO_ARC_IR_GPIO);
    ir_config.invert_out = s_snapshot.settings.ir_invert_out;
    ir_config.repeat_count = s_snapshot.settings.ir_repeat_count;
    ir_config.repeat_gap_ms = s_snapshot.settings.ir_repeat_gap_ms;
    ir_config.timing_profile = s_snapshot.settings.ir_timing_profile;
    daikin_ir_init(&ir_config);

    init_sensor();
#if PICO_ARC_ENABLE_IOT
    iot_server_config_t iot_config = {
        .snapshot = &s_snapshot,
        .command_handler = pico_arc_command_json,
    };
    pico_arc_status_t iot_status = iot_server_start(&iot_config);
    s_iot_ready = iot_status == PICO_ARC_OK;
    printf("IoT server %s (%d)\n", s_iot_ready ? "ready" : "failed", iot_status);
#else
    puts("IoT server disabled in this build");
#endif
    print_help();
    print_status();

    char line[96] = {};
    size_t line_len = 0;
    absolute_time_t next_sensor_read = make_timeout_time_ms(2000);

    while (true) {
        int ch = getchar_timeout_us(1000);
        if (ch != PICO_ERROR_TIMEOUT) {
            if (ch == '\r' || ch == '\n') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    printf("\n");
                    process_line(line);
                    line_len = 0;
                    memset(line, 0, sizeof(line));
                }
            } else if (ch == '\b' || ch == 0x7F) {
                if (line_len > 0) {
                    --line_len;
                    printf("\b \b");
                }
            } else if (isprint((unsigned char)ch) && line_len + 1 < sizeof(line)) {
                line[line_len++] = (char)ch;
                putchar(ch);
            }
        }

        if (absolute_time_diff_us(get_absolute_time(), next_sensor_read) <= 0) {
            maybe_read_sensor();
            next_sensor_read = make_timeout_time_ms(2000);
        }

        if (s_iot_ready) {
#if PICO_ARC_ENABLE_IOT
            iot_server_poll();
#endif
        }
    }
}
