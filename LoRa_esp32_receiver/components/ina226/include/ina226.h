#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

typedef struct {
    i2c_master_dev_handle_t dev;
} ina226_t;

esp_err_t ina226_init(ina226_t *sensor, i2c_master_bus_handle_t bus_handle, uint8_t i2c_addr);

double ina226_read_bus_voltage_v(ina226_t *sensor);
double ina226_read_current_a(ina226_t *sensor);
double ina226_read_power_w(ina226_t *sensor);

// Читає Alert Function Flag (POL, 20W) прямо з регістру Mask/Enable по I2C —
// надійно навіть без pull-up на фізичному ALERT-піні.
bool ina226_read_alert_flag(ina226_t *sensor);