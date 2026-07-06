#pragma once

#include <stddef.h>

#include "pico_arc452a21/app_state.h"
#include "pico_arc452a21/status.h"

typedef pico_arc_status_t (*iot_command_handler_t)(const char *command,
                                                   char *response,
                                                   size_t response_size);

typedef struct {
    app_state_snapshot_t *snapshot;
    iot_command_handler_t command_handler;
} iot_server_config_t;

pico_arc_status_t iot_server_start(const iot_server_config_t *config);
void iot_server_poll(void);
bool iot_server_is_connected(void);
const char *iot_server_ip_address(void);
const char *iot_server_connected_ssid(void);
int iot_server_rssi(void);
