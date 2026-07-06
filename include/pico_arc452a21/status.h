#pragma once

typedef enum {
    PICO_ARC_OK = 0,
    PICO_ARC_ERR_INVALID_ARG = -1,
    PICO_ARC_ERR_INVALID_STATE = -2,
    PICO_ARC_ERR_IO = -3,
    PICO_ARC_ERR_NOT_FOUND = -4,
    PICO_ARC_ERR_CHECKSUM = -5,
} pico_arc_status_t;
