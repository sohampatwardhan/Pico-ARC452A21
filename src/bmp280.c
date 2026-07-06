#include "pico_arc452a21/bmp280.h"

#include <string.h>

#include "pico/time.h"

#define I2C_TIMEOUT_US 50000u

#define REG_ID 0xD0
#define REG_RESET 0xE0
#define REG_CTRL_HUM 0xF2
#define REG_STATUS 0xF3
#define REG_CTRL_MEAS 0xF4
#define REG_CONFIG 0xF5
#define REG_PRESS_MSB 0xF7
#define REG_CALIB_00 0x88
#define REG_CALIB_H1 0xA1
#define REG_CALIB_H2 0xE1

static pico_arc_status_t read_regs(bmp280_t *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    int wrote = i2c_write_timeout_us(dev->i2c, dev->address, &reg, 1, true, I2C_TIMEOUT_US);
    if (wrote != 1) {
        return PICO_ARC_ERR_IO;
    }
    int read = i2c_read_timeout_us(dev->i2c, dev->address, buf, len, false, I2C_TIMEOUT_US);
    return read == (int)len ? PICO_ARC_OK : PICO_ARC_ERR_IO;
}

static pico_arc_status_t write_reg(bmp280_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    int wrote = i2c_write_timeout_us(dev->i2c, dev->address, buf, sizeof(buf), false, I2C_TIMEOUT_US);
    return wrote == (int)sizeof(buf) ? PICO_ARC_OK : PICO_ARC_ERR_IO;
}

static uint16_t u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t s16_le(const uint8_t *p)
{
    return (int16_t)u16_le(p);
}

static int16_t sign_extend_12(uint16_t value)
{
    return (int16_t)((value & 0x0800u) ? (value | 0xF000u) : value);
}

static pico_arc_status_t read_calibration(bmp280_t *dev)
{
    uint8_t calib[26] = {};
    pico_arc_status_t status = read_regs(dev, REG_CALIB_00, calib, sizeof(calib));
    if (status != PICO_ARC_OK) {
        return status;
    }

    dev->dig_t1 = u16_le(&calib[0]);
    dev->dig_t2 = s16_le(&calib[2]);
    dev->dig_t3 = s16_le(&calib[4]);
    dev->dig_p1 = u16_le(&calib[6]);
    dev->dig_p2 = s16_le(&calib[8]);
    dev->dig_p3 = s16_le(&calib[10]);
    dev->dig_p4 = s16_le(&calib[12]);
    dev->dig_p5 = s16_le(&calib[14]);
    dev->dig_p6 = s16_le(&calib[16]);
    dev->dig_p7 = s16_le(&calib[18]);
    dev->dig_p8 = s16_le(&calib[20]);
    dev->dig_p9 = s16_le(&calib[22]);

    if (!dev->has_humidity) {
        return PICO_ARC_OK;
    }

    status = read_regs(dev, REG_CALIB_H1, &dev->dig_h1, 1);
    if (status != PICO_ARC_OK) {
        return status;
    }

    uint8_t hcal[7] = {};
    status = read_regs(dev, REG_CALIB_H2, hcal, sizeof(hcal));
    if (status != PICO_ARC_OK) {
        return status;
    }

    dev->dig_h2 = s16_le(&hcal[0]);
    dev->dig_h3 = hcal[2];
    dev->dig_h4 = sign_extend_12(((uint16_t)hcal[3] << 4) | (hcal[4] & 0x0F));
    dev->dig_h5 = sign_extend_12(((uint16_t)hcal[5] << 4) | (hcal[4] >> 4));
    dev->dig_h6 = (int8_t)hcal[6];
    return PICO_ARC_OK;
}

pico_arc_status_t bmp280_init(bmp280_t *dev, i2c_inst_t *i2c, uint8_t address)
{
    if (dev == NULL || i2c == NULL) {
        return PICO_ARC_ERR_INVALID_ARG;
    }

    memset(dev, 0, sizeof(*dev));
    dev->i2c = i2c;
    dev->address = address;

    uint8_t id = 0;
    pico_arc_status_t status = read_regs(dev, REG_ID, &id, 1);
    if (status != PICO_ARC_OK) {
        return status;
    }

    if (id != 0x58 && id != 0x60) {
        return PICO_ARC_ERR_NOT_FOUND;
    }
    dev->has_humidity = id == 0x60;

    status = write_reg(dev, REG_RESET, 0xB6);
    if (status != PICO_ARC_OK) {
        return status;
    }
    sleep_ms(5);

    status = read_calibration(dev);
    if (status != PICO_ARC_OK) {
        return status;
    }

    if (dev->has_humidity) {
        status = write_reg(dev, REG_CTRL_HUM, 0x01);
        if (status != PICO_ARC_OK) {
            return status;
        }
    }

    status = write_reg(dev, REG_CONFIG, 0xA0);
    if (status != PICO_ARC_OK) {
        return status;
    }

    return write_reg(dev, REG_CTRL_MEAS, 0x27);
}

static int32_t compensate_temperature(bmp280_t *dev, int32_t adc_t)
{
    int32_t var1 = ((((adc_t >> 3) - ((int32_t)dev->dig_t1 << 1))) *
                    ((int32_t)dev->dig_t2)) >>
                   11;
    int32_t var2 = (((((adc_t >> 4) - ((int32_t)dev->dig_t1)) *
                      ((adc_t >> 4) - ((int32_t)dev->dig_t1))) >>
                     12) *
                    ((int32_t)dev->dig_t3)) >>
                   14;
    dev->t_fine = var1 + var2;
    return (dev->t_fine * 5 + 128) >> 8;
}

static uint32_t compensate_pressure(bmp280_t *dev, int32_t adc_p)
{
    int64_t var1 = ((int64_t)dev->t_fine) - 128000;
    int64_t var2 = var1 * var1 * (int64_t)dev->dig_p6;
    var2 = var2 + ((var1 * (int64_t)dev->dig_p5) << 17);
    var2 = var2 + (((int64_t)dev->dig_p4) << 35);
    var1 = ((var1 * var1 * (int64_t)dev->dig_p3) >> 8) +
           ((var1 * (int64_t)dev->dig_p2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dev->dig_p1) >> 33;

    if (var1 == 0) {
        return 0;
    }

    int64_t p = 1048576 - adc_p;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dev->dig_p9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dev->dig_p8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dev->dig_p7) << 4);
    return (uint32_t)p;
}

static uint32_t compensate_humidity(bmp280_t *dev, int32_t adc_h)
{
    int32_t v_x1 = dev->t_fine - 76800;
    v_x1 = (((((adc_h << 14) - (((int32_t)dev->dig_h4) << 20) -
               (((int32_t)dev->dig_h5) * v_x1)) +
              16384) >>
             15) *
            (((((((v_x1 * ((int32_t)dev->dig_h6)) >> 10) *
                 (((v_x1 * ((int32_t)dev->dig_h3)) >> 11) + 32768)) >>
                10) +
               2097152) *
                  ((int32_t)dev->dig_h2) +
              8192) >>
             14));
    v_x1 = v_x1 -
           (((((v_x1 >> 15) * (v_x1 >> 15)) >> 7) * ((int32_t)dev->dig_h1)) >> 4);
    if (v_x1 < 0) {
        v_x1 = 0;
    }
    if (v_x1 > 419430400) {
        v_x1 = 419430400;
    }
    return (uint32_t)(v_x1 >> 12);
}

pico_arc_status_t bmp280_read(bmp280_t *dev, bmp280_reading_t *reading)
{
    if (dev == NULL || reading == NULL) {
        return PICO_ARC_ERR_INVALID_ARG;
    }

    uint8_t buf[8] = {};
    size_t len = dev->has_humidity ? 8 : 6;
    pico_arc_status_t status = read_regs(dev, REG_PRESS_MSB, buf, len);
    if (status != PICO_ARC_OK) {
        return status;
    }

    int32_t adc_p = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
    int32_t adc_t = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);

    int32_t temp_c_x100 = compensate_temperature(dev, adc_t);
    uint32_t pressure_q24_8 = compensate_pressure(dev, adc_p);

    reading->temperature_c = (float)temp_c_x100 / 100.0f;
    reading->pressure_pa = (float)pressure_q24_8 / 256.0f;
    reading->humidity_percent = 0.0f;
    reading->has_humidity = dev->has_humidity;

    if (dev->has_humidity) {
        int32_t adc_h = ((int32_t)buf[6] << 8) | buf[7];
        reading->humidity_percent = (float)compensate_humidity(dev, adc_h) / 1024.0f;
    }

    return PICO_ARC_OK;
}
