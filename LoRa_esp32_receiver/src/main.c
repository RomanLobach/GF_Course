#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "ssd1306.h"
#include "sx1276.h"
#include "ina226.h"

// ---- I2C: OLED + INA226 на спільній шині ----
#define I2C_SDA_GPIO     21
#define I2C_SCL_GPIO     22
#define OLED_I2C_ADDR    0x3C
#define INA226_I2C_ADDR  0x40 // типова адреса за замовчуванням (A0/A1 -> GND)

// ---- LoRa (SX1276) ----
#define LORA_SCK_GPIO   5
#define LORA_MISO_GPIO  19
#define LORA_MOSI_GPIO  27
#define LORA_CS_GPIO    18
#define LORA_RST_GPIO   23
#define LORA_FREQ_HZ    868000000UL

#define LORA_MSG_MAGIC     0xC6
#define LORA_CMD_LAMP_OFF  0x00
#define LORA_CMD_LAMP_ON   0x01

// ---- MOSFET ----
#define MOSFET_GPIO      4

// ---- ALERT (додатковий backup-канал; основна перевірка йде програмно) ----
#define ALERT_GPIO       35

#define CURRENT_LIMIT_A   1.0
#define VOLTAGE_LIMIT_V  20.0
#define POWER_LIMIT_W    20.0

#define DISPLAY_UPDATE_MS  500
#define LOOP_TICK_MS       10

static const char *TAG = "main";

static i2c_master_bus_handle_t s_i2c_bus;
static ssd1306_t s_display;
static sx1276_t s_radio;
static ina226_t s_power_sensor;

static bool s_lamp_command_on = false; // останній прийнятий по LoRa стан
static bool s_alert_active = false;
static double s_voltage_v = 0.0, s_current_a = 0.0, s_power_w = 0.0;

static void oled_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
}

static void mosfet_set(bool on)
{
    gpio_set_level(MOSFET_GPIO, on ? 1 : 0);
}

static void update_measurements_and_alert(void)
{
    s_voltage_v = ina226_read_bus_voltage_v(&s_power_sensor);
    s_current_a = ina226_read_current_a(&s_power_sensor);
    s_power_w   = ina226_read_power_w(&s_power_sensor);

    bool hw_alert = ina226_read_alert_flag(&s_power_sensor);
    bool sw_alert = (s_current_a > CURRENT_LIMIT_A) ||
                    (s_voltage_v > VOLTAGE_LIMIT_V) ||
                    (s_power_w > POWER_LIMIT_W);

    s_alert_active = hw_alert || sw_alert;

    if (s_alert_active) {
        mosfet_set(false); // миттєве примусове вимкнення, незалежно від команди по LoRa
    } else {
        mosfet_set(s_lamp_command_on); // повертаємось до останньої команди
    }
}

static void update_display(void)
{
    char line0[24], line1[24], line2[24], line3[24];

    snprintf(line0, sizeof(line0), "lamp state: %s", s_lamp_command_on ? "on" : "off");
    snprintf(line1, sizeof(line1), "V=%.2fv I=%.3fa", s_voltage_v, s_current_a);
    snprintf(line2, sizeof(line2), "P=%.3fw", s_power_w);
    snprintf(line3, sizeof(line3), "%s", s_alert_active ? "alert detected!" : "");

    ssd1306_clear(&s_display);
    ssd1306_draw_string(&s_display, 0, 0, line0);
    ssd1306_draw_string(&s_display, 2, 0, line1);
    ssd1306_draw_string(&s_display, 4, 0, line2);
    ssd1306_draw_string(&s_display, 6, 0, line3);
    ssd1306_flush(&s_display);
}

static void handle_lora_packet(const uint8_t *data, uint8_t len)
{
    if (len != 2 || data[0] != LORA_MSG_MAGIC) {
        ESP_LOGW(TAG, "LoRa: отримано некоректний пакет (len=%d)", len);
        return;
    }

    s_lamp_command_on = (data[1] == LORA_CMD_LAMP_ON);
    ESP_LOGI(TAG, "LoRa: lamp command received = %s", s_lamp_command_on ? "ON" : "OFF");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting: LILYGO T3 V1.6.1 LoRa lamp receiver + power monitor");

    gpio_config_t mosfet_conf = {
        .pin_bit_mask = 1ULL << MOSFET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&mosfet_conf);
    mosfet_set(false);

    gpio_config_t alert_conf = {
        .pin_bit_mask = 1ULL << ALERT_GPIO,
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&alert_conf);

    oled_i2c_init();
    ESP_ERROR_CHECK(ssd1306_init(&s_display, s_i2c_bus, OLED_I2C_ADDR));
    ESP_ERROR_CHECK(ina226_init(&s_power_sensor, s_i2c_bus, INA226_I2C_ADDR));

    esp_err_t lora_err = sx1276_init(&s_radio,
                                      LORA_SCK_GPIO, LORA_MISO_GPIO, LORA_MOSI_GPIO,
                                      LORA_CS_GPIO, LORA_RST_GPIO, LORA_FREQ_HZ);
    if (lora_err != ESP_OK) {
        ESP_LOGE(TAG, "LoRa init failed — прийом команд не працюватиме");
    } else {
        sx1276_start_receive(&s_radio);
    }

    int64_t last_display_update_us = 0;

    while (1) {
        uint8_t rx_buf[8];
        uint8_t rx_len = 0;
        if (sx1276_receive(&s_radio, rx_buf, sizeof(rx_buf), &rx_len)) {
            handle_lora_packet(rx_buf, rx_len);
        }

        update_measurements_and_alert();

        int64_t now = esp_timer_get_time();
        if (now - last_display_update_us >= (int64_t)DISPLAY_UPDATE_MS * 1000) {
            update_display();
            last_display_update_us = now;
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }
}