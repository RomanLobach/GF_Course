#pragma once
#include <stdint.h>
#include <stdbool.h>

#define GPS_UTC_TIME_LEN 12
#define GPS_RAW_LINE_LEN 96

typedef struct {
    bool has_fix;
    int fix_quality;    // 0 = немає фіксу, 1 = GPS fix, 2 = DGPS fix
    int satellites;
    double hdop;
    double altitude_m;
    double latitude;
    double longitude;
    char utc_time[GPS_UTC_TIME_LEN];
    char last_gga_raw[GPS_RAW_LINE_LEN];

    uint32_t total_lines_seen; // скільки будь-яких NMEA-рядків прийнято
    uint32_t total_gga_seen;   // скільки саме $GPGGA/$GNGGA розібрано
} gps_data_t;

void gps_init(int uart_num, int rx_gpio, int tx_gpio, int baud_rate);
void gps_poll(gps_data_t *out);