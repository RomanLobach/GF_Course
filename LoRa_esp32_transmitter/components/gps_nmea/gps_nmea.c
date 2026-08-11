#include "gps_nmea.h"
#include "driver/uart.h"
#include <stdlib.h>
#include <string.h>

#define GPS_MIN_SATELLITES 3
#define LINE_BUF_SIZE 96
#define GGA_MAX_FIELDS 15

static int s_uart_num;
static char s_line_buf[LINE_BUF_SIZE];
static size_t s_line_len = 0;

static double nmea_to_decimal(const char *raw, char hemisphere) {
  if (raw == NULL || raw[0] == '\0')
    return 0.0;
  double value = atof(raw);
  int degrees = (int)(value / 100);
  double minutes = value - (degrees * 100);
  double decimal = degrees + (minutes / 60.0);
  if (hemisphere == 'S' || hemisphere == 'W')
    decimal = -decimal;
  return decimal;
}

static int split_csv(char *line, char *fields[], int max_fields) {
  int count = 0;
  char *p = line;
  fields[count++] = p;
  while (*p && count < max_fields) {
    if (*p == ',') {
      *p = '\0';
      fields[count++] = p + 1;
    }
    p++;
  }
  return count;
}

static void parse_gga(char *line, gps_data_t *out) {
  // Копіюємо сирий рядок ДО розбору (split_csv псує оригінал)
  strncpy(out->last_gga_raw, line, GPS_RAW_LINE_LEN - 1);
  out->last_gga_raw[GPS_RAW_LINE_LEN - 1] = '\0';

  char *fields[GGA_MAX_FIELDS] = {0};
  int count = split_csv(line, fields, GGA_MAX_FIELDS);
  out->total_gga_seen++;

  if (count < 10)
    return; // недостатньо полів у реченні

  strncpy(out->utc_time, fields[1], GPS_UTC_TIME_LEN - 1);
  out->utc_time[GPS_UTC_TIME_LEN - 1] = '\0';

  out->fix_quality = atoi(fields[6]);
  out->satellites = atoi(fields[7]);
  out->hdop = fields[8][0] ? atof(fields[8]) : 0.0;
  out->altitude_m = fields[9][0] ? atof(fields[9]) : 0.0;
  out->has_fix =
      (out->fix_quality > 0) && (out->satellites >= GPS_MIN_SATELLITES);

  if (out->has_fix && fields[2][0] != '\0' && fields[4][0] != '\0') {
    out->latitude = nmea_to_decimal(fields[2], fields[3][0]);
    out->longitude = nmea_to_decimal(fields[4], fields[5][0]);
  }
}

void gps_init(int uart_num, int rx_gpio, int tx_gpio, int baud_rate) {
  s_uart_num = uart_num;

  uart_config_t cfg = {
      .baud_rate = baud_rate,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_ERROR_CHECK(uart_driver_install(s_uart_num, 1024, 0, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(s_uart_num, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(s_uart_num, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));
  s_line_len = 0;
}

void gps_poll(gps_data_t *out) {
  uint8_t byte;
  while (uart_read_bytes(s_uart_num, &byte, 1, 0) > 0) {
    if (byte == '\n') {
      s_line_buf[s_line_len] = '\0';

      if (s_line_len > 0) {
        out->total_lines_seen++;
      }

      if (s_line_len > 6 && (strncmp(s_line_buf, "$GPGGA", 6) == 0 ||
                             strncmp(s_line_buf, "$GNGGA", 6) == 0)) {
        parse_gga(s_line_buf, out);
      }
      s_line_len = 0;
    } else if (byte != '\r') {
      if (s_line_len < LINE_BUF_SIZE - 1) {
        s_line_buf[s_line_len++] = (char)byte;
      } else {
        s_line_len = 0;
      }
    }
  }
}