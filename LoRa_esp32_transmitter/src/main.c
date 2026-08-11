#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"

#include "ssd1306.h"
#include "gps_nmea.h"
#include "button.h"
#include "buzzer.h"
#include "sx1276.h"

#define OLED_SDA_GPIO   21
#define OLED_SCL_GPIO   22
#define OLED_I2C_ADDR   0x3C

#define BUZZER_GPIO     0
#define BUTTON_GPIO     14

#define GPS_UART_NUM    UART_NUM_1
#define GPS_RX_GPIO     12
#define GPS_TX_GPIO     13
#define GPS_BAUD_RATE   9600

// ---- LoRa (SX1276), піни T3 V1.6.1 ----
#define LORA_SCK_GPIO   5
#define LORA_MISO_GPIO  19
#define LORA_MOSI_GPIO  27
#define LORA_CS_GPIO    18
#define LORA_RST_GPIO   23
#define LORA_FREQ_HZ    868000000UL

#define LORA_MSG_MAGIC     0xC6
#define LORA_CMD_LAMP_OFF  0x00
#define LORA_CMD_LAMP_ON   0x01

#define BUTTON_DEBOUNCE_MS   25
#define BUZZER_BEEP_MS       80
#define DISPLAY_UPDATE_MS    500
#define LOOP_TICK_MS         10

static const char *TAG = "main";

static i2c_master_bus_handle_t s_i2c_bus;
static ssd1306_t s_display;
static button_t s_button;
static gps_data_t s_gps = {0};
static sx1276_t s_radio;
static bool s_lamp_state = false;

static void oled_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
}

static void format_running_time(char *buf, size_t buf_size, int64_t uptime_us)
{
    uint32_t total_seconds = (uint32_t)(uptime_us / 1000000);
    uint32_t hours   = total_seconds / 3600;
    uint32_t minutes = (total_seconds / 60) % 60;
    uint32_t seconds = total_seconds % 60;
    snprintf(buf, buf_size, "running time: %u:%02u:%02u", hours, minutes, seconds);
}

static void update_display(void)
{
    char line0[24], line1[24], line2[24], line3[24];

    format_running_time(line0, sizeof(line0), esp_timer_get_time());
    snprintf(line1, sizeof(line1), "satellite qty: %d", s_gps.satellites);

    if (s_gps.has_fix) {
        snprintf(line2, sizeof(line2), "latd: %.5f", s_gps.latitude);
        snprintf(line3, sizeof(line3), "long: %.5f", s_gps.longitude);
    } else {
        snprintf(line2, sizeof(line2), "latd: ");
        snprintf(line3, sizeof(line3), "long: ");
    }

    ssd1306_clear(&s_display);
    ssd1306_draw_string(&s_display, 0, 0, line0);
    ssd1306_draw_string(&s_display, 2, 0, line1);
    ssd1306_draw_string(&s_display, 4, 0, line2);
    ssd1306_draw_string(&s_display, 6, 0, line3);
    ssd1306_flush(&s_display);
}

static void log_gps_debug(const gps_data_t *gps)
{
    ESP_LOGI(TAG, "---- GPS debug ----");
    ESP_LOGI(TAG, "UART lines seen total: %u | GGA sentences parsed: %u",
             (unsigned)gps->total_lines_seen, (unsigned)gps->total_gga_seen);
    ESP_LOGI(TAG, "UTC time: %s | fix_quality: %d | satellites: %d | HDOP: %.2f",
             gps->utc_time, gps->fix_quality, gps->satellites, gps->hdop);
    ESP_LOGI(TAG, "Altitude: %.1f m | has_fix: %s",
             gps->altitude_m, gps->has_fix ? "yes" : "no");
    if (gps->has_fix) {
        ESP_LOGI(TAG, "Lat: %.6f | Lon: %.6f", gps->latitude, gps->longitude);
    }
    ESP_LOGI(TAG, "Last raw $GPGGA: %s", gps->last_gga_raw);
    ESP_LOGI(TAG, "--------------------");
}

static void send_lamp_command(bool state)
{
    uint8_t packet[2] = { LORA_MSG_MAGIC, state ? LORA_CMD_LAMP_ON : LORA_CMD_LAMP_OFF };
    esp_err_t err = sx1276_send(&s_radio, packet, sizeof(packet), 2000);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LoRa: sent lamp command = %s", state ? "ON" : "OFF");
    } else {
        ESP_LOGW(TAG, "LoRa: send failed (%s)", esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting: LILYGO T3 V1.6.1 GPS tracker + LoRa lamp control");

    oled_i2c_init();
    ESP_ERROR_CHECK(ssd1306_init(&s_display, s_i2c_bus, OLED_I2C_ADDR));

    buzzer_init(BUZZER_GPIO);
    button_init(&s_button, BUTTON_GPIO, BUTTON_DEBOUNCE_MS);
    gps_init(GPS_UART_NUM, GPS_RX_GPIO, GPS_TX_GPIO, GPS_BAUD_RATE);

    esp_err_t lora_err = sx1276_init(&s_radio,
                                      LORA_SCK_GPIO, LORA_MISO_GPIO, LORA_MOSI_GPIO,
                                      LORA_CS_GPIO, LORA_RST_GPIO, LORA_FREQ_HZ);
    if (lora_err != ESP_OK) {
        ESP_LOGE(TAG, "LoRa init failed — кнопка й далі працюватиме, але сигнал не піде");
    }

    int64_t last_display_update_us = 0;
    int64_t last_gps_debug_us = 0;
    const int64_t GPS_DEBUG_INTERVAL_US = 12 * 1000 * 1000;

    while (1) {
        button_update(&s_button);
        if (button_consume_press_event(&s_button)) {
            ESP_LOGI(TAG, "Button pressed -> beep + LoRa toggle");
            buzzer_beep(BUZZER_BEEP_MS);

            s_lamp_state = !s_lamp_state;
            send_lamp_command(s_lamp_state);
        }
        buzzer_update();

        gps_poll(&s_gps);

        int64_t now = esp_timer_get_time();

        if (now - last_display_update_us >= (int64_t)DISPLAY_UPDATE_MS * 1000) {
            update_display();
            last_display_update_us = now;
        }

        if (now - last_gps_debug_us >= GPS_DEBUG_INTERVAL_US) {
            log_gps_debug(&s_gps);
            last_gps_debug_us = now;
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }
}