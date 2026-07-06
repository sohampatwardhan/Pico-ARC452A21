#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pico_arc452a21/daikin_ir.h"
#include "pico_arc452a21/status.h"

#define APP_WIFI_CREDENTIAL_SLOTS 3
#define APP_WIFI_SSID_MAX_LEN 32
#define APP_WIFI_PASSWORD_MAX_LEN 64
#define APP_HOSTNAME_MAX_LEN 32
#define APP_HOSTNAME_DEFAULT "pico-arc452a21"

typedef struct {
    bool ir_invert_out;
    daikin_timing_profile_t ir_timing_profile;
    uint8_t ir_repeat_count;
    uint32_t ir_repeat_gap_ms;
} app_settings_t;

typedef struct {
    char ssid[APP_WIFI_SSID_MAX_LEN + 1];
    char password[APP_WIFI_PASSWORD_MAX_LEN + 1];
} app_wifi_credential_t;

typedef struct {
    app_settings_t settings;
    daikin_state_t ac_state;
    char hostname[APP_HOSTNAME_MAX_LEN + 1];
    app_wifi_credential_t wifi[APP_WIFI_CREDENTIAL_SLOTS];
} app_state_snapshot_t;

void app_state_defaults(app_state_snapshot_t *snapshot);
pico_arc_status_t app_state_load(app_state_snapshot_t *snapshot);
pico_arc_status_t app_state_save(const app_state_snapshot_t *snapshot);
