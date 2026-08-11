#include "ina226.h"

#define REG_CONFIG        0x00
#define REG_BUS_VOLTAGE   0x02
#define REG_POWER         0x03
#define REG_CURRENT       0x04
#define REG_CALIBRATION   0x05
#define REG_MASK_ENABLE   0x06
#define REG_ALERT_LIMIT   0x07

#define CONFIG_DEFAULT     0x4127 // AVG=1, VBUSCT/VSHCT=1.1мс, режим: неперервно shunt+bus

// Калібрування для шунта 0.1 Ом (стандартний приклад з датащита INA226):
// Current_LSB = 100 uA -> Cal = 0.00512 / (Current_LSB * R_SHUNT) = 512
#define CALIBRATION_VALUE   512
#define CURRENT_LSB_A       0.0001
#define POWER_LSB_W         (25.0 * CURRENT_LSB_A) // 0.0025 W
#define BUS_VOLTAGE_LSB_V   0.00125

#define MASK_ENABLE_POL_BIT (1 << 11) // Power Over-Limit
#define MASK_ENABLE_AFF_BIT (1 << 4)  // Alert Function Flag (статус)

#define POWER_ALERT_LIMIT_W 20.0

static esp_err_t write_reg16(ina226_t *sensor, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return i2c_master_transmit(sensor->dev, buf, sizeof(buf), 100);
}

static uint16_t read_reg16(ina226_t *sensor, uint8_t reg)
{
    uint8_t rx[2] = {0};
    i2c_master_transmit_receive(sensor->dev, &reg, 1, rx, sizeof(rx), 100);
    return (uint16_t)((rx[0] << 8) | rx[1]);
}

esp_err_t ina226_init(ina226_t *sensor, i2c_master_bus_handle_t bus_handle, uint8_t i2c_addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &sensor->dev);
    if (err != ESP_OK) return err;

    err = write_reg16(sensor, REG_CONFIG, CONFIG_DEFAULT);
    if (err != ESP_OK) return err;

    err = write_reg16(sensor, REG_CALIBRATION, CALIBRATION_VALUE);
    if (err != ESP_OK) return err;

    uint16_t alert_limit_raw = (uint16_t)(POWER_ALERT_LIMIT_W / POWER_LSB_W);
    err = write_reg16(sensor, REG_ALERT_LIMIT, alert_limit_raw);
    if (err != ESP_OK) return err;

    return write_reg16(sensor, REG_MASK_ENABLE, MASK_ENABLE_POL_BIT);
}

double ina226_read_bus_voltage_v(ina226_t *sensor)
{
    uint16_t raw = read_reg16(sensor, REG_BUS_VOLTAGE);
    return raw * BUS_VOLTAGE_LSB_V;
}

double ina226_read_current_a(ina226_t *sensor)
{
    int16_t raw = (int16_t)read_reg16(sensor, REG_CURRENT); // регістр зі знаком
    return raw * CURRENT_LSB_A;
}

double ina226_read_power_w(ina226_t *sensor)
{
    uint16_t raw = read_reg16(sensor, REG_POWER);
    return raw * POWER_LSB_W;
}

bool ina226_read_alert_flag(ina226_t *sensor)
{
    uint16_t mask_enable = read_reg16(sensor, REG_MASK_ENABLE);
    return (mask_enable & MASK_ENABLE_AFF_BIT) != 0;
}