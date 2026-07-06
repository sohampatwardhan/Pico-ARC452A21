#include "pico_arc452a21/app_state.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/address_mapped.h"
#include "hardware/sync.h"
#include "pico_arc452a21/flash_layout.h"

#define APP_STATE_MAGIC 0x50415243u
#define APP_STATE_VERSION 4u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    app_state_snapshot_t snapshot;
    uint32_t checksum;
} persisted_state_t;

_Static_assert(sizeof(persisted_state_t) <= FLASH_SECTOR_SIZE,
               "persisted app state must fit in one flash sector");

static uint32_t checksum32(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

void app_state_defaults(app_state_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    daikin_state_init_cool_fahrenheit(&snapshot->ac_state, 72);
    snapshot->settings.ir_invert_out = false;
    snapshot->settings.ir_timing_profile = DAIKIN_TIMING_PROFILE_NOMINAL;
    snapshot->settings.ir_repeat_count = 1;
    snapshot->settings.ir_repeat_gap_ms = 80;
    memcpy(snapshot->hostname, APP_HOSTNAME_DEFAULT, sizeof(APP_HOSTNAME_DEFAULT));
}

pico_arc_status_t app_state_load(app_state_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return PICO_ARC_ERR_INVALID_ARG;
    }

    app_state_defaults(snapshot);

    const persisted_state_t *stored =
        (const persisted_state_t *)(XIP_BASE + PICO_ARC_FLASH_APP_CONFIG_OFFSET);
    if (stored->magic != APP_STATE_MAGIC ||
        stored->version != APP_STATE_VERSION ||
        stored->size != sizeof(app_state_snapshot_t)) {
        return PICO_ARC_ERR_NOT_FOUND;
    }

    persisted_state_t temp;
    memcpy(&temp, stored, sizeof(temp));
    uint32_t expected = temp.checksum;
    temp.checksum = 0;
    if (checksum32(&temp, sizeof(temp)) != expected) {
        return PICO_ARC_ERR_CHECKSUM;
    }

    *snapshot = stored->snapshot;
    if (snapshot->settings.ir_repeat_count == 0) {
        snapshot->settings.ir_repeat_count = 1;
    }
    return PICO_ARC_OK;
}

pico_arc_status_t app_state_save(const app_state_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return PICO_ARC_ERR_INVALID_ARG;
    }

    persisted_state_t persisted = {
        .magic = APP_STATE_MAGIC,
        .version = APP_STATE_VERSION,
        .size = sizeof(app_state_snapshot_t),
        .snapshot = *snapshot,
        .checksum = 0,
    };
    persisted.checksum = checksum32(&persisted, sizeof(persisted));

    uint8_t sector[FLASH_SECTOR_SIZE] = {};
    memcpy(sector, &persisted, sizeof(persisted));

    uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(PICO_ARC_FLASH_APP_CONFIG_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(PICO_ARC_FLASH_APP_CONFIG_OFFSET, sector, sizeof(sector));
    restore_interrupts(interrupts);

    return PICO_ARC_OK;
}
