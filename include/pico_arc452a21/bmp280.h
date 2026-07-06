#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"
#include "pico_arc452a21/status.h"

typedef struct {
    i2c_inst_t *i2c;
    uint8_t address;
    bool has_humidity;
    uint16_t dig_t1;
    int16_t dig_t2;
    int16_t dig_t3;
    uint16_t dig_p1;
    int16_t dig_p2;
    int16_t dig_p3;
    int16_t dig_p4;
    int16_t dig_p5;
    int16_t dig_p6;
    int16_t dig_p7;
    int16_t dig_p8;
    int16_t dig_p9;
    uint8_t dig_h1;
    int16_t dig_h2;
    uint8_t dig_h3;
    int16_t dig_h4;
    int16_t dig_h5;
    int8_t dig_h6;
    int32_t t_fine;
} bmp280_t;

typedef struct {
    float temperature_c;
    float pressure_pa;
    float humidity_percent;
    bool has_humidity;
} bmp280_reading_t;

pico_arc_status_t bmp280_init(bmp280_t *dev, i2c_inst_t *i2c, uint8_t address);
pico_arc_status_t bmp280_read(bmp280_t *dev, bmp280_reading_t *reading);
